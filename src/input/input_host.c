/* The PC's pads, keyboard and mouse as Xbox controllers. See input_host.h. */

#define _CRT_SECURE_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#pragma comment(lib, "winmm.lib")
#include <stdio.h>
#include <string.h>

#include "input_host.h"

#ifdef XBOXRECOMP_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

#define SLOTS         4
#define MAX_DEVICES   8
#define POLL_MS       2
#define RUMBLE_REFRESH_MS 100
#define RUMBLE_HOLD_MS    300

typedef struct Device {
    int used;
    int backend;                    /* 1 SDL, 2 XInput */
    int slot;                       /* -1 until assigned */
    char name[64];
#ifdef XBOXRECOMP_HAVE_SDL3
    SDL_Gamepad *gp;
    SDL_JoystickID id;
#endif
    DWORD xi_index;
} Device;

typedef struct Slot {
    int device;                     /* index into dev[], or -1 */
    WORD low, high;                 /* what the title last asked for */
    WORD applied_low, applied_high; /* what the pad was last told */
    ULONGLONG applied_at;
} Slot;

static struct Host {
    int running;
    CRITICAL_SECTION cs;
    HANDLE thread, ready, stop;
    InputHostConfig cfg;
    int kb_slot, kb_merge, players;
    uint8_t key[INPUT_KEY_COUNT];
    ULONGLONG wheel_until[2];
    volatile LONG focused;
    Device dev[MAX_DEVICES];
    Slot slot[SLOTS];
    InputPad out[SLOTS];
    InputPad last[SLOTS];
    DWORD packet[SLOTS];
    int found;                      /* pads opened at start */
    int sdl;                        /* SDL is the pad backend */
} H;

void xbox_HostInputDefaults(InputHostConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->keyboard = INPUT_HOST_KEYBOARD_AUTO;
    cfg->use_sdl = 1;
    cfg->rumble_percent = 100;
    input_padmap_defaults(&cfg->padmap);
}

/* ---- reading a pad into the raw form ------------------------------------- */

static void raw_directions(InputRaw *r)
{
    r->src[INPUT_SRC_LSTICK_UP]    = (int16_t)(r->ly > 0 ? r->ly : 0);
    r->src[INPUT_SRC_LSTICK_DOWN]  = (int16_t)(r->ly < 0 ? -r->ly : 0);
    r->src[INPUT_SRC_LSTICK_LEFT]  = (int16_t)(r->lx < 0 ? -r->lx : 0);
    r->src[INPUT_SRC_LSTICK_RIGHT] = (int16_t)(r->lx > 0 ? r->lx : 0);
    r->src[INPUT_SRC_RSTICK_UP]    = (int16_t)(r->ry > 0 ? r->ry : 0);
    r->src[INPUT_SRC_RSTICK_DOWN]  = (int16_t)(r->ry < 0 ? -r->ry : 0);
    r->src[INPUT_SRC_RSTICK_LEFT]  = (int16_t)(r->rx < 0 ? -r->rx : 0);
    r->src[INPUT_SRC_RSTICK_RIGHT] = (int16_t)(r->rx > 0 ? r->rx : 0);
}

static int16_t clamp_axis(int v)
{
    return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
}

static int read_xinput(DWORD index, InputRaw *r)
{
    XINPUT_STATE s;
    WORD b;
    if (XInputGetState(index, &s) != ERROR_SUCCESS) return 0;
    b = s.Gamepad.wButtons;
    memset(r, 0, sizeof(*r));
#define BTN(mask, source) if (b & (mask)) r->src[source] = 32767
    BTN(XINPUT_GAMEPAD_A, INPUT_SRC_SOUTH);
    BTN(XINPUT_GAMEPAD_B, INPUT_SRC_EAST);
    BTN(XINPUT_GAMEPAD_X, INPUT_SRC_WEST);
    BTN(XINPUT_GAMEPAD_Y, INPUT_SRC_NORTH);
    BTN(XINPUT_GAMEPAD_BACK, INPUT_SRC_BACK);
    BTN(XINPUT_GAMEPAD_START, INPUT_SRC_START);
    BTN(XINPUT_GAMEPAD_LEFT_THUMB, INPUT_SRC_LEFT_STICK_CLICK);
    BTN(XINPUT_GAMEPAD_RIGHT_THUMB, INPUT_SRC_RIGHT_STICK_CLICK);
    BTN(XINPUT_GAMEPAD_LEFT_SHOULDER, INPUT_SRC_LEFT_SHOULDER);
    BTN(XINPUT_GAMEPAD_RIGHT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER);
    BTN(XINPUT_GAMEPAD_DPAD_UP, INPUT_SRC_DPAD_UP);
    BTN(XINPUT_GAMEPAD_DPAD_DOWN, INPUT_SRC_DPAD_DOWN);
    BTN(XINPUT_GAMEPAD_DPAD_LEFT, INPUT_SRC_DPAD_LEFT);
    BTN(XINPUT_GAMEPAD_DPAD_RIGHT, INPUT_SRC_DPAD_RIGHT);
#undef BTN
    r->src[INPUT_SRC_LEFT_TRIGGER]  = (int16_t)(s.Gamepad.bLeftTrigger * 32767 / 255);
    r->src[INPUT_SRC_RIGHT_TRIGGER] = (int16_t)(s.Gamepad.bRightTrigger * 32767 / 255);
    r->lx = clamp_axis(s.Gamepad.sThumbLX);
    r->ly = clamp_axis(s.Gamepad.sThumbLY);
    r->rx = clamp_axis(s.Gamepad.sThumbRX);
    r->ry = clamp_axis(s.Gamepad.sThumbRY);
    raw_directions(r);
    return 1;
}

#ifdef XBOXRECOMP_HAVE_SDL3
static void read_sdl(SDL_Gamepad *gp, InputRaw *r)
{
    static const struct { SDL_GamepadButton button; int src; } buttons[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, INPUT_SRC_SOUTH }, { SDL_GAMEPAD_BUTTON_EAST, INPUT_SRC_EAST },
        { SDL_GAMEPAD_BUTTON_WEST, INPUT_SRC_WEST }, { SDL_GAMEPAD_BUTTON_NORTH, INPUT_SRC_NORTH },
        { SDL_GAMEPAD_BUTTON_BACK, INPUT_SRC_BACK }, { SDL_GAMEPAD_BUTTON_GUIDE, INPUT_SRC_GUIDE },
        { SDL_GAMEPAD_BUTTON_START, INPUT_SRC_START },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, INPUT_SRC_LEFT_STICK_CLICK },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, INPUT_SRC_RIGHT_STICK_CLICK },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, INPUT_SRC_LEFT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, INPUT_SRC_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, INPUT_SRC_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, INPUT_SRC_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, INPUT_SRC_DPAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_MISC1, INPUT_SRC_MISC1 }, { SDL_GAMEPAD_BUTTON_TOUCHPAD, INPUT_SRC_TOUCHPAD },
        { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, INPUT_SRC_PADDLE1 }, { SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, INPUT_SRC_PADDLE2 },
        { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, INPUT_SRC_PADDLE3 }, { SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, INPUT_SRC_PADDLE4 },
    };
    size_t i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (SDL_GetGamepadButton(gp, buttons[i].button)) r->src[buttons[i].src] = 32767;
    {
        int lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        r->src[INPUT_SRC_LEFT_TRIGGER]  = (int16_t)(lt < 0 ? 0 : lt);
        r->src[INPUT_SRC_RIGHT_TRIGGER] = (int16_t)(rt < 0 ? 0 : rt);
    }
    /* SDL's Y axes point down; the Xbox's point up. */
    r->lx = clamp_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
    r->ly = clamp_axis(-1 - SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
    r->rx = clamp_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    r->ry = clamp_axis(-1 - SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    raw_directions(r);
}
#endif

/* ---- slots and devices ---------------------------------------------------- */

static void log_line(const char *fmt, const char *a, int b)
{
    fprintf(stderr, fmt, a, b);
    fflush(stderr);
}

/* Give a new device the lowest slot the keyboard does not occupy. */
static void assign_device(int d)
{
    int s;
    for (s = 0; s < SLOTS; s++) {
        if (s == H.kb_slot || H.slot[s].device >= 0) continue;
        H.slot[s].device = d;
        H.dev[d].slot = s;
        log_line("[INPUT] %s is player %d\n", H.dev[d].name, s + 1);
        return;
    }
    log_line("[INPUT] %s ignored: all four players have a device\n", H.dev[d].name, 0);
}

static int new_device(int backend, const char *name)
{
    int d;
    for (d = 0; d < MAX_DEVICES; d++) {
        if (H.dev[d].used) continue;
        memset(&H.dev[d], 0, sizeof(H.dev[d]));
        H.dev[d].used = 1;
        H.dev[d].backend = backend;
        H.dev[d].slot = -1;
        snprintf(H.dev[d].name, sizeof(H.dev[d].name), "%s", name ? name : "pad");
        return d;
    }
    return -1;
}

static void free_device(int d)
{
    if (H.dev[d].slot >= 0) {
        log_line("[INPUT] %s removed from player %d\n", H.dev[d].name, H.dev[d].slot + 1);
        H.slot[H.dev[d].slot].device = -1;
    }
#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.dev[d].gp) SDL_CloseGamepad(H.dev[d].gp);
#endif
    memset(&H.dev[d], 0, sizeof(H.dev[d]));
}

static void plan_slots(int pads)
{
    int k = H.cfg.keyboard, n;
    H.kb_slot = -1;
    H.kb_merge = 0;
    if (k == INPUT_HOST_KEYBOARD_MERGE) {
        H.kb_merge = 1;
    } else if (k == INPUT_HOST_KEYBOARD_AUTO) {
        if (pads >= SLOTS) H.kb_merge = 1;
        else H.kb_slot = pads;
    } else if (k >= INPUT_HOST_KEYBOARD_SLOT1 && k <= INPUT_HOST_KEYBOARD_SLOT4) {
        H.kb_slot = k - INPUT_HOST_KEYBOARD_SLOT1;
    }
    n = pads + (H.kb_slot >= 0 ? 1 : 0);
    if (H.kb_slot >= 0 && H.kb_slot + 1 > n) n = H.kb_slot + 1;
    if (n < H.cfg.players) n = H.cfg.players;
    if (n < 1) n = 1;
    if (n > SLOTS) n = SLOTS;
    H.players = n;
}

#ifdef XBOXRECOMP_HAVE_SDL3
static void open_sdl_gamepad(SDL_JoystickID id)
{
    SDL_Gamepad *gp;
    int d, i;
    for (i = 0; i < MAX_DEVICES; i++)
        if (H.dev[i].used && H.dev[i].backend == 1 && H.dev[i].id == id) return;
    gp = SDL_OpenGamepad(id);
    if (!gp) {
        log_line("[INPUT] could not open pad: %s\n", SDL_GetError(), 0);
        return;
    }
    d = new_device(1, SDL_GetGamepadName(gp));
    if (d < 0) { SDL_CloseGamepad(gp); return; }
    H.dev[d].gp = gp;
    H.dev[d].id = id;
    EnterCriticalSection(&H.cs);
    assign_device(d);
    LeaveCriticalSection(&H.cs);
}
#endif

/* ---- the reader thread ----------------------------------------------------- */

static void apply_rumble(int s, int focused)
{
    Slot *sl = &H.slot[s];
    Device *dv;
    WORD low, high;
    ULONGLONG now = GetTickCount64();
    if (sl->device < 0) return;
    dv = &H.dev[sl->device];
    low = high = 0;
    if (focused && H.cfg.rumble_percent > 0) {
        low = (WORD)((unsigned)sl->low * (unsigned)H.cfg.rumble_percent / 100u);
        high = (WORD)((unsigned)sl->high * (unsigned)H.cfg.rumble_percent / 100u);
    }
    if (low == sl->applied_low && high == sl->applied_high &&
        (!(low | high) || now - sl->applied_at < RUMBLE_REFRESH_MS))
        return;
    sl->applied_low = low;
    sl->applied_high = high;
    sl->applied_at = now;
#ifdef XBOXRECOMP_HAVE_SDL3
    if (dv->backend == 1 && dv->gp) {
        SDL_RumbleGamepad(dv->gp, low, high, (low | high) ? RUMBLE_HOLD_MS : 0);
        return;
    }
#endif
    if (dv->backend == 2) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = low;
        v.wRightMotorSpeed = high;
        XInputSetState(dv->xi_index, &v);
    }
}

static void to_state(const InputPad *p, XBOX_INPUT_STATE *out, DWORD packet)
{
    memset(out, 0, sizeof(*out));
    out->dwPacketNumber = packet;
    out->Gamepad.wButtons = (WORD)(p->buttons & 0xFF);
    memcpy(out->Gamepad.bAnalogButtons, p->analog, 8);
    out->Gamepad.sThumbLX = p->lx;
    out->Gamepad.sThumbLY = p->ly;
    out->Gamepad.sThumbRX = p->rx;
    out->Gamepad.sThumbRY = p->ry;
}

static void poll_once(void)
{
    InputPad next[SLOTS];
    uint8_t keys[INPUT_KEY_COUNT];
    InputHostConfig cfg;
    ULONGLONG now = GetTickCount64();
    int focused = InterlockedCompareExchange(&H.focused, 0, 0);
    int s, live;

    EnterCriticalSection(&H.cs);
    cfg = H.cfg;
    memcpy(keys, H.key, sizeof(keys));
    LeaveCriticalSection(&H.cs);
    live = focused || cfg.ignore_focus;
    keys[INPUT_KEY_WHEEL_UP] = now < H.wheel_until[0];
    keys[INPUT_KEY_WHEEL_DOWN] = now < H.wheel_until[1];

    memset(next, 0, sizeof(next));
    for (s = 0; s < SLOTS; s++) {
        InputRaw raw;
        Device *dv;
        int have = 0;
        if (H.slot[s].device < 0) continue;
        dv = &H.dev[H.slot[s].device];
#ifdef XBOXRECOMP_HAVE_SDL3
        if (dv->backend == 1 && dv->gp) { read_sdl(dv->gp, &raw); have = 1; }
#endif
        if (dv->backend == 2) have = read_xinput(dv->xi_index, &raw);
        if (have && live) input_pad_map(&cfg.padmap, &raw, &next[s]);
    }
    if (live) {
        InputPad kp;
        if (H.kb_slot >= 0 || H.kb_merge) {
            input_keys_resolve(&cfg.keys, keys, &kp);
            if (H.kb_slot >= 0) next[H.kb_slot] = kp;
            else input_pad_merge(&next[0], &kp);
        }
    }

    EnterCriticalSection(&H.cs);
    for (s = 0; s < SLOTS; s++) {
        if (memcmp(&next[s], &H.last[s], sizeof(InputPad)) != 0) {
            H.last[s] = next[s];
            H.packet[s]++;
        }
        H.out[s] = next[s];
    }
    LeaveCriticalSection(&H.cs);

    for (s = 0; s < SLOTS; s++) apply_rumble(s, live);
}

static void hotplug_once(void)
{
    static ULONGLONG xi_scan;
    ULONGLONG now = GetTickCount64();
    int d;
#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.sdl) {
        SDL_Event e;
        SDL_UpdateGamepads();
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                open_sdl_gamepad(e.gdevice.which);
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                EnterCriticalSection(&H.cs);
                for (d = 0; d < MAX_DEVICES; d++)
                    if (H.dev[d].used && H.dev[d].backend == 1 && H.dev[d].id == e.gdevice.which)
                        free_device(d);
                LeaveCriticalSection(&H.cs);
            }
        }
        return;
    }
#endif
    if (now - xi_scan < 1000) return;
    xi_scan = now;
    for (d = 0; d < XUSER_MAX_COUNT; d++) {
        InputRaw raw;
        int i, known = -1;
        for (i = 0; i < MAX_DEVICES; i++)
            if (H.dev[i].used && H.dev[i].backend == 2 && H.dev[i].xi_index == (DWORD)d) known = i;
        if (read_xinput((DWORD)d, &raw)) {
            if (known < 0) {
                char name[32];
                snprintf(name, sizeof(name), "XInput pad %d", d + 1);
                known = new_device(2, name);
                if (known >= 0) {
                    H.dev[known].xi_index = (DWORD)d;
                    EnterCriticalSection(&H.cs);
                    assign_device(known);
                    LeaveCriticalSection(&H.cs);
                }
            }
        } else if (known >= 0) {
            EnterCriticalSection(&H.cs);
            free_device(known);
            LeaveCriticalSection(&H.cs);
        }
    }
}

static DWORD WINAPI reader_thread(LPVOID unused)
{
    int d, i;
    (void)unused;
    timeBeginPeriod(1);

#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.cfg.use_sdl) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_Init(SDL_INIT_GAMEPAD)) {
            H.sdl = 1;
        } else {
            log_line("[INPUT] SDL gamepads unavailable (%s); using XInput\n", SDL_GetError(), 0);
        }
    }
    if (H.sdl) {
        /* Pads can take a moment to appear; wait until the count holds still. */
        int stable = 0, last = -1, waited;
        SDL_JoystickID *ids = NULL;
        int count = 0;
        for (waited = 0; waited < 400 && stable < 100; waited += 25) {
            SDL_UpdateGamepads();
            ids = SDL_GetGamepads(&count);
            SDL_free(ids);
            stable = count == last ? stable + 25 : 0;
            last = count;
            if (count > 0 && stable >= 100) break;
            SDL_Delay(25);
        }
        ids = SDL_GetGamepads(&count);
        for (i = 0; ids && i < count && i < MAX_DEVICES; i++) {
            SDL_Gamepad *gp = SDL_OpenGamepad(ids[i]);
            if (!gp) continue;
            d = new_device(1, SDL_GetGamepadName(gp));
            if (d < 0) { SDL_CloseGamepad(gp); continue; }
            H.dev[d].gp = gp;
            H.dev[d].id = ids[i];
            H.found++;
        }
        SDL_free(ids);
    }
#endif
    if (!H.sdl) {
        for (i = 0; i < XUSER_MAX_COUNT; i++) {
            InputRaw raw;
            if (read_xinput((DWORD)i, &raw)) {
                char name[32];
                snprintf(name, sizeof(name), "XInput pad %d", i + 1);
                d = new_device(2, name);
                if (d >= 0) { H.dev[d].xi_index = (DWORD)i; H.found++; }
            }
        }
    }

    EnterCriticalSection(&H.cs);
    plan_slots(H.found > SLOTS ? SLOTS : H.found);
    for (d = 0; d < MAX_DEVICES; d++)
        if (H.dev[d].used) assign_device(d);
    LeaveCriticalSection(&H.cs);
    fprintf(stderr, "[INPUT] %d pad(s) found (%s); keyboard %s; %d player(s)\n", H.found,
            H.sdl ? "SDL" : "XInput",
            H.kb_slot >= 0 ? "its own player" : H.kb_merge ? "merged into player 1" : "off",
            H.players);
    if (H.kb_slot >= 0) fprintf(stderr, "[INPUT] keyboard and mouse are player %d\n", H.kb_slot + 1);
    fflush(stderr);
    SetEvent(H.ready);

    while (WaitForSingleObject(H.stop, POLL_MS) == WAIT_TIMEOUT) {
        hotplug_once();
        poll_once();
    }

    /* Stop the motors before the devices go. */
    EnterCriticalSection(&H.cs);
    for (i = 0; i < SLOTS; i++) { H.slot[i].low = H.slot[i].high = 0; }
    LeaveCriticalSection(&H.cs);
    for (i = 0; i < SLOTS; i++) apply_rumble(i, 0);
    EnterCriticalSection(&H.cs);
    for (d = 0; d < MAX_DEVICES; d++)
        if (H.dev[d].used) free_device(d);
    LeaveCriticalSection(&H.cs);
#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.sdl) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
    timeEndPeriod(1);
    return 0;
}

/* ---- the public interface ---------------------------------------------------- */

int xbox_HostInputStart(const InputHostConfig *cfg)
{
    static int cs_ready;
    int i;
    if (H.running) return H.found;
    if (!cs_ready) { InitializeCriticalSection(&H.cs); cs_ready = 1; }
    EnterCriticalSection(&H.cs);
    memset(H.dev, 0, sizeof(H.dev));
    memset(H.slot, 0, sizeof(H.slot));
    memset(H.out, 0, sizeof(H.out));
    memset(H.last, 0, sizeof(H.last));
    memset(H.packet, 0, sizeof(H.packet));
    memset(H.key, 0, sizeof(H.key));
    H.wheel_until[0] = H.wheel_until[1] = 0;
    for (i = 0; i < SLOTS; i++) H.slot[i].device = -1;
    H.found = 0;
    H.sdl = 0;
    H.cfg = *cfg;
    LeaveCriticalSection(&H.cs);
    InterlockedExchange(&H.focused, 1);
    H.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    H.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    H.thread = H.ready && H.stop ? CreateThread(NULL, 0, reader_thread, NULL, 0, NULL) : NULL;
    if (!H.thread) return 0;
    H.running = 1;
    WaitForSingleObject(H.ready, 10000);
    return H.found;
}

void xbox_HostInputStop(void)
{
    if (!H.running) return;
    H.running = 0;
    SetEvent(H.stop);
    WaitForSingleObject(H.thread, 5000);
    CloseHandle(H.thread);
    CloseHandle(H.ready);
    CloseHandle(H.stop);
    H.thread = H.ready = H.stop = NULL;
}

int xbox_HostInputActive(void) { return H.running; }

void xbox_HostInputConfigure(const InputHostConfig *cfg)
{
    if (!H.running) return;
    EnterCriticalSection(&H.cs);
    /* The layout fixed at start stays. */
    {
        int kb = H.cfg.keyboard, players = H.cfg.players;
        H.cfg = *cfg;
        H.cfg.keyboard = kb;
        H.cfg.players = players;
    }
    LeaveCriticalSection(&H.cs);
}

int xbox_HostInputPlayerCount(void) { return H.players < 1 ? 1 : H.players; }
int xbox_HostInputKeyboardSlot(void) { return H.kb_slot; }

void xbox_HostInputKey(int code, int down)
{
    if (code >= 0 && code < INPUT_KEY_COUNT) H.key[code] = down ? 1 : 0;
}

void xbox_HostInputWheel(int notches)
{
    if (notches > 0) H.wheel_until[0] = GetTickCount64() + 120;
    if (notches < 0) H.wheel_until[1] = GetTickCount64() + 120;
}

void xbox_HostInputFocus(int focused)
{
    InterlockedExchange(&H.focused, focused ? 1 : 0);
    if (!focused) memset(H.key, 0, sizeof(H.key));
}

DWORD xbox_HostInputGetState(int slot, XBOX_INPUT_STATE *state)
{
    int present;
    if (!H.running || slot < 0 || slot >= SLOTS || !state) return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&H.cs);
    present = H.slot[slot].device >= 0 || slot == H.kb_slot || (H.kb_merge && slot == 0);
    if (present) to_state(&H.out[slot], state, H.packet[slot]);
    LeaveCriticalSection(&H.cs);
    return present ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD xbox_HostInputRumble(int slot, WORD low, WORD high)
{
    if (!H.running || slot < 0 || slot >= SLOTS) return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&H.cs);
    H.slot[slot].low = low;
    H.slot[slot].high = high;
    LeaveCriticalSection(&H.cs);
    return ERROR_SUCCESS;
}

int xbox_HostInputSlotInfo(int slot, char *name, int size)
{
    int what = 0;
    if (!H.running || slot < 0 || slot >= SLOTS) return 0;
    EnterCriticalSection(&H.cs);
    if (H.slot[slot].device >= 0) what |= 1;
    if (slot == H.kb_slot || (H.kb_merge && slot == 0)) what |= 2;
    if (name && size > 0) {
        if (what == 1) snprintf(name, (size_t)size, "%s", H.dev[H.slot[slot].device].name);
        else if (what == 2) snprintf(name, (size_t)size, "Keyboard and mouse");
        else if (what == 3) snprintf(name, (size_t)size, "%s + keyboard", H.dev[H.slot[slot].device].name);
        else name[0] = 0;
    }
    LeaveCriticalSection(&H.cs);
    return what;
}
