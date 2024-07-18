/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 * Copyright 2018 Aric Stewart
 * Copyright 2021 Rémi Bernon for CodeWeavers
 * Copyright 2024 BrunoSX 
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winuser.h"
#include "winreg.h"
#include "wingdi.h"
#include "winnls.h"
#include "winternl.h"
#include "winsock2.h"

#include "dbt.h"
#include "setupapi.h"
#include "initguid.h"
#include "devguid.h"
#include "xinput.h"

#include "wine/debug.h"

/* Not defined in the headers, used only by XInputGetStateEx */
#define XINPUT_GAMEPAD_GUIDE 0x0400

#define SERVER_PORT 7949
#define CLIENT_PORT 7947
#define BUFFER_SIZE 64

#define REQUEST_CODE_GET_GAMEPAD 8
#define REQUEST_CODE_GET_GAMEPAD_STATE 9
#define REQUEST_CODE_RELEASE_GAMEPAD 10

#define IDX_BUTTON_A 0
#define IDX_BUTTON_B 1
#define IDX_BUTTON_X 2
#define IDX_BUTTON_Y 3
#define IDX_BUTTON_L1 4
#define IDX_BUTTON_R1 5
#define IDX_BUTTON_L2 10
#define IDX_BUTTON_R2 11
#define IDX_BUTTON_SELECT 6
#define IDX_BUTTON_START 7
#define IDX_BUTTON_L3 8
#define IDX_BUTTON_R3 9

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

struct xinput_controller
{
    CRITICAL_SECTION crit;
    XINPUT_CAPABILITIES caps;
    XINPUT_STATE state;
    XINPUT_GAMEPAD last_keystroke;
    BOOL enabled;
    BOOL connected;
    int id;
};

static struct xinput_controller controller;
static CRITICAL_SECTION_DEBUG controller_critsect_debug = 
{
    0, 0, &controller.crit,
    {&controller_critsect_debug.ProcessLocksList, &controller_critsect_debug.ProcessLocksList},
    0, 0, {(DWORD_PTR)(__FILE__ ": controller.crit")}
};

static struct xinput_controller controller = 
{
    .crit = {&controller_critsect_debug, -1, 0, 0, 0, 0},
    .enabled = FALSE,
    .connected = FALSE,
    .id = 0
};

static HANDLE start_event;
static BOOL thread_running = FALSE;

static SOCKET server_sock = INVALID_SOCKET;
static BOOL winsock_loaded = FALSE;
static char xinput_min_index = 3;

static void close_server_socket(void) 
{
    if (server_sock != INVALID_SOCKET) 
    {
        closesocket(server_sock);
        server_sock = INVALID_SOCKET;
    }
    
    if (winsock_loaded) 
    {
        WSACleanup();
        winsock_loaded = FALSE;
    }
}

static BOOL create_server_socket(void)
{    
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    const UINT reuse_addr = 1;
    ULONG non_blocking = 1;
    int res;
    
    close_server_socket();
    
    winsock_loaded = WSAStartup(MAKEWORD(2,2), &wsa_data) == NO_ERROR;
    if (!winsock_loaded) return FALSE;
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);
    
    server_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (server_sock == INVALID_SOCKET) return FALSE;
    
    res = setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse_addr, sizeof(reuse_addr));
    if (res == SOCKET_ERROR) return FALSE;
    
    ioctlsocket(server_sock, FIONBIO, &non_blocking);

    res = bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (res == SOCKET_ERROR) return FALSE;
    
    return TRUE;
}

static void get_gamepad_request(void)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(CLIENT_PORT);
    
    buffer[0] = REQUEST_CODE_GET_GAMEPAD;
    buffer[1] = 1;
    buffer[2] = 1;
    sendto(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static void release_gamepad_request(void)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    client_addr.sin_port = htons(CLIENT_PORT);
    
    buffer[0] = REQUEST_CODE_RELEASE_GAMEPAD;
    sendto(server_sock, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&client_addr, sizeof(client_addr));
}

static BOOL controller_check_caps(void)
{
    XINPUT_CAPABILITIES *caps = &controller.caps;
    memset(caps, 0, sizeof(XINPUT_CAPABILITIES));
    
    caps->Gamepad.wButtons = 0xffff;
    caps->Gamepad.bLeftTrigger = (1u << (sizeof(caps->Gamepad.bLeftTrigger) + 1)) - 1;
    caps->Gamepad.bRightTrigger = (1u << (sizeof(caps->Gamepad.bRightTrigger) + 1)) - 1;
    caps->Gamepad.sThumbLX = (1u << (sizeof(caps->Gamepad.sThumbLX) + 1)) - 1;
    caps->Gamepad.sThumbLY = (1u << (sizeof(caps->Gamepad.sThumbLY) + 1)) - 1;
    caps->Gamepad.sThumbRX = (1u << (sizeof(caps->Gamepad.sThumbRX) + 1)) - 1;
    caps->Gamepad.sThumbRY = (1u << (sizeof(caps->Gamepad.sThumbRY) + 1)) - 1;

    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    return TRUE;
}

static void controller_destroy(void)
{   
    EnterCriticalSection(&controller.crit);
    thread_running = FALSE;
    release_gamepad_request();
    xinput_min_index = 3;
    
    controller.enabled = FALSE;
    controller.connected = FALSE;
    
    close_server_socket();
    LeaveCriticalSection(&controller.crit);
}

static void controller_init(void)
{
    memset(&controller.state, 0, sizeof(controller.state));
    controller_check_caps();
    controller.connected = TRUE;
    controller.enabled = TRUE;
}

static void controller_update_state(char *buffer)
{
    int i, gamepad_id;
    char dpad;
    short buttons, thumb_lx, thumb_ly, thumb_rx, thumb_ry;
    XINPUT_STATE *state = &controller.state;
    
    EnterCriticalSection(&controller.crit);
    
    gamepad_id = *(int*)(buffer + 2);
    if (buffer[1] != 1 || gamepad_id != controller.id) 
    {
        controller.connected = FALSE;
        memset(&controller.state, 0, sizeof(controller.state));
        LeaveCriticalSection(&controller.crit);
        return;
    }
    
    buttons = *(short*)(buffer + 6);    
    dpad = buffer[8];
    
    thumb_lx = *(short*)(buffer + 9);
    thumb_ly = *(short*)(buffer + 11);
    thumb_rx = *(short*)(buffer + 13);
    thumb_ry = *(short*)(buffer + 15);

    state->Gamepad.wButtons = 0;
    for (i = 0; i < 10; i++)
    {    
        if ((buttons & (1<<i))) {
            switch (i)
            {
            case IDX_BUTTON_A: state->Gamepad.wButtons |= XINPUT_GAMEPAD_A; break;
            case IDX_BUTTON_B: state->Gamepad.wButtons |= XINPUT_GAMEPAD_B; break;
            case IDX_BUTTON_X: state->Gamepad.wButtons |= XINPUT_GAMEPAD_X; break;
            case IDX_BUTTON_Y: state->Gamepad.wButtons |= XINPUT_GAMEPAD_Y; break;
            case IDX_BUTTON_L1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
            case IDX_BUTTON_R1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
            case IDX_BUTTON_SELECT: state->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK; break;
            case IDX_BUTTON_START: state->Gamepad.wButtons |= XINPUT_GAMEPAD_START; break;
            case IDX_BUTTON_L3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB; break;
            case IDX_BUTTON_R3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB; break;
            }
        }
    }
    
    state->Gamepad.bLeftTrigger = (buttons & (1<<10)) ? 255 : 0;
    state->Gamepad.bRightTrigger = (buttons & (1<<11)) ? 255 : 0;

    switch (dpad)
    {
    case 0: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP; break;
    case 1: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 2: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
    case 3: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 4: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
    case 5: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 6: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
    case 7: state->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_UP; break;
    }

    state->Gamepad.sThumbLX = thumb_lx;
    state->Gamepad.sThumbLY = -thumb_ly;
    state->Gamepad.sThumbRX = thumb_rx;
    state->Gamepad.sThumbRY = -thumb_ry;
    
    state->dwPacketNumber++;
    LeaveCriticalSection(&controller.crit);
}

static DWORD WINAPI controller_read_thread_proc(void *param) {
    int res;
    char buffer[BUFFER_SIZE];
    BOOL started = FALSE;
    DWORD curr_time, last_time;
    
    SetThreadDescription(GetCurrentThread(), L"wine_xinput_controller_read");
    if (server_sock == INVALID_SOCKET && !create_server_socket()) 
    {
        SetEvent(start_event);
        return 0;
    }
    
    get_gamepad_request();
    
    last_time = GetCurrentTime();
    while (thread_running)
    {
        res = recvfrom(server_sock, buffer, BUFFER_SIZE, 0, NULL, 0);
        if (res <= 0)
        {
            if (WSAGetLastError() != WSAEWOULDBLOCK) break;
            
            curr_time = GetCurrentTime();
            if ((curr_time - last_time) >= 2000) {
                get_gamepad_request();
                last_time = curr_time;
            }
            
            Sleep(16);
            continue;
        }
        
        if (buffer[0] == REQUEST_CODE_GET_GAMEPAD) 
        {
            int gamepad_id;
            gamepad_id = *(int*)(buffer + 1);
            
            EnterCriticalSection(&controller.crit);
            if (gamepad_id > 0) 
            {
                controller.id = gamepad_id;
                if (!controller.connected) controller_init();
            }
            else if (gamepad_id == 0) 
            {
                controller.id = 0;
                controller.connected = FALSE;       
            }
            LeaveCriticalSection(&controller.crit);
            
            if (!started) 
            {
                started = TRUE;
                SetEvent(start_event);    
            }
        }
        else if (buffer[0] == REQUEST_CODE_GET_GAMEPAD_STATE && controller.connected)
        {
            controller_update_state(buffer);
        }
    }
    
    return 0;
}

static BOOL WINAPI start_read_thread_once(INIT_ONCE *once, void *param, void **context)
{
    HANDLE thread;
    
    thread_running = TRUE;

    start_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!start_event) ERR("failed to create start event, error %lu\n", GetLastError());   
    
    thread = CreateThread(NULL, 0, controller_read_thread_proc, NULL, 0, NULL);
    if (!thread) ERR("failed to create read thread, error %lu\n", GetLastError());
    CloseHandle(thread);
    
    WaitForSingleObject(start_event, 2000);
    CloseHandle(start_event);
    
    return TRUE;
}

static void start_read_thread(void)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&init_once, start_read_thread_once, NULL, NULL);
}

static BOOL controller_is_connected(DWORD index) 
{
    BOOL connected;
    EnterCriticalSection(&controller.crit);
    connected = index == 0 && controller.connected;
    LeaveCriticalSection(&controller.crit);
    return connected;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    TRACE("inst %p, reason %lu, reserved %p.\n", inst, reason, reserved);

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        break;
    case DLL_PROCESS_DETACH:
        if (reserved) break;
        controller_destroy();
        break;
    }
    return TRUE;
}

void WINAPI DECLSPEC_HOTPATCH XInputEnable(BOOL enable)
{
    TRACE("enable %d.\n", enable);

    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
    start_read_thread();

    if (!controller.connected) return;
    controller.enabled = enable;    
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputSetState(DWORD index, XINPUT_VIBRATION *vibration)
{
    TRACE("index %lu, vibration %p.\n", index, vibration);

    start_read_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_SUCCESS;
}

/* Some versions of SteamOverlayRenderer hot-patch XInputGetStateEx() and call
 * XInputGetState() in the hook, so we need a wrapper. */
static DWORD xinput_get_state(DWORD index, XINPUT_STATE *state)
{
    if (!state) return ERROR_BAD_ARGUMENTS;

    start_read_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (index < xinput_min_index) xinput_min_index = index;
    if (index == xinput_min_index) index = 0;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;
    
    EnterCriticalSection(&controller.crit);
    *state = controller.state;
    LeaveCriticalSection(&controller.crit);
    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetState(DWORD index, XINPUT_STATE *state)
{
    DWORD ret;

    TRACE("index %lu, state %p.\n", index, state);

    ret = xinput_get_state(index, state);
    if (ret != ERROR_SUCCESS) return ret;

    /* The main difference between this and the Ex version is the media guide button */
    state->Gamepad.wButtons &= ~XINPUT_GAMEPAD_GUIDE;

    return ERROR_SUCCESS;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetStateEx(DWORD index, XINPUT_STATE *state)
{
    TRACE("index %lu, state %p.\n", index, state);

    return xinput_get_state(index, state);
}

static const int JS_STATE_OFF = 0;
static const int JS_STATE_LOW = 1;
static const int JS_STATE_HIGH = 2;

static int joystick_state(const SHORT value)
{
    if (value > 20000) return JS_STATE_HIGH;
    if (value < -20000) return JS_STATE_LOW;
    return JS_STATE_OFF;
}

static WORD js_vk_offs(const int x, const int y)
{
    if (y == JS_STATE_OFF)
    {
      /*if (x == JS_STATE_OFF) shouldn't get here */
        if (x == JS_STATE_LOW) return 3; /* LEFT */
      /*if (x == JS_STATE_HIGH)*/ return 2; /* RIGHT */
    }
    if (y == JS_STATE_HIGH)
    {
        if (x == JS_STATE_OFF) return 0; /* UP */
        if (x == JS_STATE_LOW) return 4; /* UPLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 5; /* UPRIGHT */
    }
  /*if (y == JS_STATE_LOW)*/
    {
        if (x == JS_STATE_OFF) return 1; /* DOWN */
        if (x == JS_STATE_LOW) return 7; /* DOWNLEFT */
      /*if (x == JS_STATE_HIGH)*/ return 6; /* DOWNRIGHT */
    }
}

static DWORD check_joystick_keystroke(XINPUT_KEYSTROKE *keystroke, const SHORT *cur_x, const SHORT *cur_y, 
                                      SHORT *last_x, SHORT *last_y, const WORD base_vk)
{
    int cur_vk = 0, cur_x_st, cur_y_st;
    int last_vk = 0, last_x_st, last_y_st;

    cur_x_st = joystick_state(*cur_x);
    cur_y_st = joystick_state(*cur_y);
    if (cur_x_st || cur_y_st)
        cur_vk = base_vk + js_vk_offs(cur_x_st, cur_y_st);

    last_x_st = joystick_state(*last_x);
    last_y_st = joystick_state(*last_y);
    if (last_x_st || last_y_st)
        last_vk = base_vk + js_vk_offs(last_x_st, last_y_st);

    if (cur_vk != last_vk)
    {
        if (last_vk)
        {
            /* joystick was set, and now different. send a KEYUP event, and set
             * last pos to centered, so the appropriate KEYDOWN event will be
             * sent on the next call. */
            keystroke->VirtualKey = last_vk;
            keystroke->Unicode = 0; /* unused */
            keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
            keystroke->UserIndex = 0;
            keystroke->HidCode = 0;

            *last_x = 0;
            *last_y = 0;

            return ERROR_SUCCESS;
        }

        /* joystick was unset, send KEYDOWN. */
        keystroke->VirtualKey = cur_vk;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;

        *last_x = *cur_x;
        *last_y = *cur_y;

        return ERROR_SUCCESS;
    }

    *last_x = *cur_x;
    *last_y = *cur_y;

    return ERROR_EMPTY;
}

static BOOL trigger_is_on(const BYTE value)
{
    return value > 30;
}

static DWORD check_for_keystroke(XINPUT_KEYSTROKE *keystroke)
{
    const XINPUT_GAMEPAD *cur;
    DWORD ret = ERROR_EMPTY;
    int i;

    static const struct
    {
        int mask;
        WORD vk;
    } buttons[] = {
        { XINPUT_GAMEPAD_DPAD_UP, VK_PAD_DPAD_UP },
        { XINPUT_GAMEPAD_DPAD_DOWN, VK_PAD_DPAD_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, VK_PAD_DPAD_LEFT },
        { XINPUT_GAMEPAD_DPAD_RIGHT, VK_PAD_DPAD_RIGHT },
        { XINPUT_GAMEPAD_START, VK_PAD_START },
        { XINPUT_GAMEPAD_BACK, VK_PAD_BACK },
        { XINPUT_GAMEPAD_LEFT_THUMB, VK_PAD_LTHUMB_PRESS },
        { XINPUT_GAMEPAD_RIGHT_THUMB, VK_PAD_RTHUMB_PRESS },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PAD_LSHOULDER },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_PAD_RSHOULDER },
        { XINPUT_GAMEPAD_A, VK_PAD_A },
        { XINPUT_GAMEPAD_B, VK_PAD_B },
        { XINPUT_GAMEPAD_X, VK_PAD_X },
        { XINPUT_GAMEPAD_Y, VK_PAD_Y },
        /* note: guide button does not send an event */
    };

    cur = &controller.state.Gamepad;

    /*** buttons ***/
    for (i = 0; i < ARRAY_SIZE(buttons); ++i)
    {
        if ((cur->wButtons & buttons[i].mask) ^ (controller.last_keystroke.wButtons & buttons[i].mask))
        {
            keystroke->VirtualKey = buttons[i].vk;
            keystroke->Unicode = 0; /* unused */
            if (cur->wButtons & buttons[i].mask)
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYDOWN;
                controller.last_keystroke.wButtons |= buttons[i].mask;
            }
            else
            {
                keystroke->Flags = XINPUT_KEYSTROKE_KEYUP;
                controller.last_keystroke.wButtons &= ~buttons[i].mask;
            }
            keystroke->UserIndex = 0;
            keystroke->HidCode = 0;
            ret = ERROR_SUCCESS;
            goto done;
        }
    }

    /*** triggers ***/
    if (trigger_is_on(cur->bLeftTrigger) ^ trigger_is_on(controller.last_keystroke.bLeftTrigger))
    {
        keystroke->VirtualKey = VK_PAD_LTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bLeftTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;
        controller.last_keystroke.bLeftTrigger = cur->bLeftTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    if (trigger_is_on(cur->bRightTrigger) ^ trigger_is_on(controller.last_keystroke.bRightTrigger))
    {
        keystroke->VirtualKey = VK_PAD_RTRIGGER;
        keystroke->Unicode = 0; /* unused */
        keystroke->Flags = trigger_is_on(cur->bRightTrigger) ? XINPUT_KEYSTROKE_KEYDOWN : XINPUT_KEYSTROKE_KEYUP;
        keystroke->UserIndex = 0;
        keystroke->HidCode = 0;
        controller.last_keystroke.bRightTrigger = cur->bRightTrigger;
        ret = ERROR_SUCCESS;
        goto done;
    }

    /*** joysticks ***/
    ret = check_joystick_keystroke(keystroke, &cur->sThumbLX, &cur->sThumbLY,
            &controller.last_keystroke.sThumbLX,
            &controller.last_keystroke.sThumbLY, VK_PAD_LTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

    ret = check_joystick_keystroke(keystroke, &cur->sThumbRX, &cur->sThumbRY,
            &controller.last_keystroke.sThumbRX,
            &controller.last_keystroke.sThumbRY, VK_PAD_RTHUMB_UP);
    if (ret == ERROR_SUCCESS)
        goto done;

done:

    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetKeystroke(DWORD index, DWORD reserved, PXINPUT_KEYSTROKE keystroke)
{    
    TRACE("index %lu, reserved %lu, keystroke %p.\n", index, reserved, keystroke);

    if (index >= XUSER_MAX_COUNT && index != XUSER_INDEX_ANY) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index != XUSER_INDEX_ANY ? index : 0)) return ERROR_DEVICE_NOT_CONNECTED;
  
    return check_for_keystroke(keystroke);
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilities(DWORD index, DWORD flags, XINPUT_CAPABILITIES *capabilities)
{
    XINPUT_CAPABILITIES_EX caps_ex;
    DWORD ret;

    ret = XInputGetCapabilitiesEx(1, index, flags, &caps_ex);

    if (!ret) *capabilities = caps_ex.Capabilities;
    return ret;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetDSoundAudioDeviceGuids(DWORD index, GUID *render_guid, GUID *capture_guid)
{
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetBatteryInformation(DWORD index, BYTE type, XINPUT_BATTERY_INFORMATION* battery)
{
    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;

    return ERROR_NOT_SUPPORTED;
}

DWORD WINAPI DECLSPEC_HOTPATCH XInputGetCapabilitiesEx(DWORD unk, DWORD index, DWORD flags, XINPUT_CAPABILITIES_EX *caps)
{
    TRACE("unk %lu, index %lu, flags %#lx, capabilities %p.\n", unk, index, flags, caps);

    start_read_thread();

    if (index >= XUSER_MAX_COUNT) return ERROR_BAD_ARGUMENTS;
    if (!controller_is_connected(index)) return ERROR_DEVICE_NOT_CONNECTED;
    
    EnterCriticalSection(&controller.crit);

    if (flags & XINPUT_FLAG_GAMEPAD && controller.caps.SubType != XINPUT_DEVSUBTYPE_GAMEPAD) 
        return ERROR_DEVICE_NOT_CONNECTED;
    else
    {
        caps->Capabilities = controller.caps;
        caps->VendorId = 0x045E; // Wireless Xbox 360 Controller
        caps->ProductId = 0x02A1;
    }

    LeaveCriticalSection(&controller.crit);
    return ERROR_SUCCESS;
}
