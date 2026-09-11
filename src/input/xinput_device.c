/**
 * Xbox Input Compatibility Layer
 *
 * Translates the Xbox controller API to a host gamepad backend.
 * Handles the structural differences between the Xbox gamepad
 * (analog buttons as bytes, separate trigger channels) and the host
 * (digital face buttons, trigger axes).
 *
 *   _WIN32 -> Windows XInput
 *   POSIX  -> SDL2 GameController
 */

#include "xinput_xbox.h"
#include <string.h>

/* ======================================================================== */
#if defined(_WIN32)
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_last_packet[dwPort] = xi_state.dwPacketNumber;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = xi_state.dwPacketNumber;
    pState->Gamepad.wButtons = xi_state.Gamepad.wButtons & 0x00FF;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_Y) ? 255 : 0;
    /* Controller S White became the left bumper and Black the right bumper
     * on later Xbox layouts. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        (xi_state.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] = xi_state.Gamepad.bLeftTrigger;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] = xi_state.Gamepad.bRightTrigger;

    pState->Gamepad.sThumbLX = xi_state.Gamepad.sThumbLX;
    pState->Gamepad.sThumbLY = xi_state.Gamepad.sThumbLY;
    pState->Gamepad.sThumbRX = xi_state.Gamepad.sThumbRX;
    pState->Gamepad.sThumbRY = xi_state.Gamepad.sThumbRY;

    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    XINPUT_VIBRATION xi_vib;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    xi_vib.wLeftMotorSpeed = pVibration->wLeftMotorSpeed;
    xi_vib.wRightMotorSpeed = pVibration->wRightMotorSpeed;
    return XInputSetState(dwPort, &xi_vib);
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    XINPUT_CAPABILITIES xi_caps;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;

    result = XInputGetCapabilities(dwPort, dwFlags, &xi_caps);
    if (result != ERROR_SUCCESS) return result;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type = xi_caps.Type;
    pCaps->SubType = xi_caps.SubType;
    pCaps->Flags = xi_caps.Flags;
    return ERROR_SUCCESS;
}

/* ======================================================================== */
#else /* !_WIN32 */
/* ====================  SDL2 GameController backend  ===================== */
/* ======================================================================== */

#include <SDL.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>

static SDL_GameController *g_pads[XBOX_MAX_CONTROLLERS];
static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS];
static DWORD g_packet[XBOX_MAX_CONTROLLERS];
static uint64_t g_last_controller_scan_ms;

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
}

static int controller_is_open(SDL_JoystickID instance)
{
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        SDL_Joystick *joystick;
        if (!g_pads[slot])
            continue;
        joystick = SDL_GameControllerGetJoystick(g_pads[slot]);
        if (joystick && SDL_JoystickInstanceID(joystick) == instance)
            return 1;
    }
    return 0;
}

/* Refresh controller membership at a bounded rate.  The old backend scanned
 * only during startup, which made a controller connected after SDL's initial
 * device snapshot permanently invisible to the title. */
static void refresh_controllers(int force)
{
    uint64_t now = monotonic_ms();
    int joystick_count;

    if (!force && g_last_controller_scan_ms &&
        now - g_last_controller_scan_ms < 250)
        return;
    g_last_controller_scan_ms = now;

    SDL_GameControllerUpdate();

    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot] && !SDL_GameControllerGetAttached(g_pads[slot])) {
            fprintf(stderr, "  [PAD] port %d: %s (disconnected)\n", slot,
                    SDL_GameControllerName(g_pads[slot]));
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    }

    joystick_count = SDL_NumJoysticks();
    for (int i = 0; i < joystick_count; i++) {
        SDL_JoystickID instance;
        int slot;

        if (!SDL_IsGameController(i))
            continue;
        instance = SDL_JoystickGetDeviceInstanceID(i);
        if (instance < 0 || controller_is_open(instance))
            continue;

        for (slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++)
            if (!g_pads[slot])
                break;
        if (slot == XBOX_MAX_CONTROLLERS)
            break;

        g_pads[slot] = SDL_GameControllerOpen(i);
        g_controller_connected[slot] = (g_pads[slot] != NULL);
        fprintf(stderr, "  [PAD] port %d: %s (%s)\n", slot,
                g_pads[slot] ? SDL_GameControllerName(g_pads[slot])
                             : "open failed",
                g_pads[slot] ? "opened" : SDL_GetError());
        fflush(stderr);
    }

    if (force && !g_pads[0]) {
        fprintf(stderr, "  [PAD] no game controller attached (%d joystick(s)"
                " seen); waiting for hotplug\n", joystick_count);
        fflush(stderr);
    }
}

void xbox_InputInit(void)
{
    /* Without this, SDL discards controller input whenever the window is not
     * focused -- and this binary has no .app bundle, no Info.plist and never
     * calls activateIgnoringOtherApps, so it frequently never becomes the
     * frontmost app at all.  The failure is silent and looks exactly like a
     * dead pad: polls climb, connected tracks them, and nonneutral stays 0.
     * Measured: nonneutral 0 -> 224 over the same interval with this set.
     * Must precede the subsystem init to take effect. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    refresh_controllers(1);
}

/* A pad that is present and pressing things, for bring-up without hardware.
 *
 * With no controller attached this backend reports ERROR_DEVICE_NOT_CONNECTED
 * and there is no keyboard fallback, so a title that waits at a screen for a
 * button waits for ever and looks identical to one that has hung. JSRF sits in
 * a healthy frame loop after its cache stops, which is exactly that shape, and
 * telling the two apart needs a pad that says yes.
 *
 * RECOMP_FAKE_PAD=1 reports port 0 connected and pulses A and START for 150 ms
 * once a second, after a two-second settle. Diagnostic, not input support: it
 * cannot steer anything, and a real keyboard mapping is the actual fix.
 */
static int fake_pad_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("RECOMP_FAKE_PAD") ? 1 : 0;
    return on;
}

/* Whether the synthetic pad should press START on this pulse.
 *
 * START is needed to get past "Press Start" and through the menus, so a pad
 * that never sends it cannot reach the game at all. But START is also the
 * PAUSE button: keep sending it during play and the title sits in covered
 * pause, where most objects do nothing by design -- the pad stops being
 * polled, characters stop animating, and the run looks hung. A whole session
 * was spent diagnosing that as a "pad-poll stall".
 *
 * So START is sent only for an opening window, long enough to boot into the
 * game, and dropped thereafter. A is always sent, which is what advances
 * dialogue.
 *
 *   RECOMP_FAKE_PAD=1        START for the opening window, then A only
 *   RECOMP_FAKE_PAD=<n>      as above with an n-second window
 *   RECOMP_FAKE_PAD=a        never send START
 *   RECOMP_FAKE_PAD=always   always send START (the old behaviour; it pauses)
 */
#define FAKE_PAD_START_WINDOW_DEFAULT 90.0

static int fake_pad_start(double t)
{
    static int mode = -1;           /* 0 never, 1 windowed, 2 always */
    static double window = FAKE_PAD_START_WINDOW_DEFAULT;

    if (mode < 0) {
        const char *v = getenv("RECOMP_FAKE_PAD");
        mode = 1;
        if (v && (v[0] == 'a' || v[0] == 'A') && v[1] != 'l')
            mode = 0;                                   /* "a" / "noStart" */
        else if (v && strstr(v, "always"))
            mode = 2;
        else if (v && v[0] >= '2' && v[0] <= '9') {
            double n = strtod(v, NULL);
            if (n > 0.0) window = n;
        }
        fprintf(stderr, "  [PAD] fake pad: START %s\n",
                mode == 0 ? "never" : mode == 2 ? "always (will pause the game)"
                                                : "for the opening window only");
        fflush(stderr);
    }
    if (mode == 0) return 0;
    if (mode == 2) return 1;
    return t < window;
}

static double fake_pad_seconds(void)
{
    static struct timespec t0;
    static int have_t0;
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!have_t0) { t0 = now; have_t0 = 1; }
    return (double)(now.tv_sec - t0.tv_sec)
         + (double)(now.tv_nsec - t0.tv_nsec) / 1e9;
}

/* Poll accounting for the host pad.
 *
 * "[PAD] port 0: ... (opened)" is printed once at startup and says nothing
 * about a live controller -- it has been logged with the pad unplugged. There
 * has been no instrument that separates the three ways input can fail:
 *
 *   polls=0                  the guest never asks; the fault is guest-side
 *   polls>0 nonneutral=0     the guest asks and SDL reports nothing pressed
 *   polls>0 nonneutral>0     input reaches the runtime and is lost later
 *
 * Counted here because every consumer -- the emulated USB gamepad and the
 * report shim alike -- comes through this one function. Read-only. */
unsigned long g_pad_polls;
unsigned long g_pad_polls_connected;
unsigned long g_pad_polls_nonneutral;
unsigned long g_pad_polls_disconnected;

/* Thread suspend/resume accounting lives in the platform layer; the pad report
 * is where it is wanted, because the question it answers is whether a lost
 * wakeup coincides with the report in which polls stop climbing. */
void w32_thread_trace_report(void);

void xbox_InputPollReport(void)
{
    fprintf(stderr, "  [PAD-POLL] polls=%lu connected=%lu nonneutral=%lu"
            " not_connected=%lu\n",
            g_pad_polls, g_pad_polls_connected, g_pad_polls_nonneutral,
            g_pad_polls_disconnected);
    fflush(stderr);
    w32_thread_trace_report();
}

/* RECOMP_PAD_TRACE -- one line each time the host pad state changes.
 *
 * nonneutral counts polls, so a flat counter says "SDL reported nothing
 * pressed" and cannot be told apart from nobody pressing anything. That
 * difference has so far been settled by asking a person to press buttons to a
 * schedule and comparing -- which is a measurement with a human in the loop,
 * and it does not belong in this tree. An edge-triggered line settles it from
 * the log alone: press a button, and either a line appears or the host read is
 * dead. Only transitions print, so a mashed pad costs a few lines a second.
 *
 * Read-only, opt-in, and it observes the state the guest is about to be given
 * -- not SDL's raw view -- so a line here means the value reached our side of
 * the pad path intact. */
static void pad_trace(const XBOX_INPUT_STATE *st, int active)
{
    static int on = -1;
    static unsigned long long prev = ~0ull;
    unsigned long long sig;

    if (on < 0) on = getenv("RECOMP_PAD_TRACE") ? 1 : 0;
    if (!on) return;

    /* Buttons exactly; axes quantised, so resting jitter is not a transition. */
    sig = (unsigned long long)st->Gamepad.wButtons;
    for (int i = 0; i < 8; i++)
        sig = sig * 3u + (st->Gamepad.bAnalogButtons[i] > 32);
    sig = sig * 7u + (unsigned)(st->Gamepad.sThumbLX / 8192 + 4);
    sig = sig * 7u + (unsigned)(st->Gamepad.sThumbLY / 8192 + 4);
    sig = sig * 7u + (unsigned)(st->Gamepad.sThumbRX / 8192 + 4);
    sig = sig * 7u + (unsigned)(st->Gamepad.sThumbRY / 8192 + 4);
    if (sig == prev) return;
    prev = sig;

    fprintf(stderr, "  [PAD-TRACE] t=%7.2f %s buttons=%04X a=%3u b=%3u"
            " lx=%6d ly=%6d poll=%lu\n",
            fake_pad_seconds(), active ? "PRESSED " : "released",
            (unsigned)st->Gamepad.wButtons,
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_A],
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_B],
            (int)st->Gamepad.sThumbLX, (int)st->Gamepad.sThumbLY,
            g_pad_polls);
    fflush(stderr);
}

/* Anything a human would call "pressed or moved". The stick threshold is the
 * same 4096 the USB report path uses to reject resting jitter. */
static void pad_note_state(const XBOX_INPUT_STATE *st)
{
    int active = st->Gamepad.wButtons != 0;
    for (int i = 0; !active && i < 8; i++)
        active = st->Gamepad.bAnalogButtons[i] > 32;
    if (!active) {
        active = st->Gamepad.sThumbLX > 4096 || st->Gamepad.sThumbLX < -4096
              || st->Gamepad.sThumbLY > 4096 || st->Gamepad.sThumbLY < -4096
              || st->Gamepad.sThumbRX > 4096 || st->Gamepad.sThumbRX < -4096
              || st->Gamepad.sThumbRY > 4096 || st->Gamepad.sThumbRY < -4096;
    }
    if (active)
        g_pad_polls_nonneutral++;
    pad_trace(st, active);
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    g_pad_polls++;

    if (fake_pad_on()) {
        if (dwPort != 0)
            return ERROR_DEVICE_NOT_CONNECTED;
        memset(pState, 0, sizeof(*pState));
        pState->dwPacketNumber = ++g_packet[0];
        {
            double t = fake_pad_seconds();
            if (t > 2.0 && (t - (double)(long)t) < 0.15) {
                /* START opens the pause menu, and a synthetic START once a
                 * second leaves the title sitting in covered pause with most
                 * objects doing nothing -- which reads exactly like a hung
                 * game or a dead controller, and was misdiagnosed as a
                 * "pad-poll stall" for a whole session. Any run that must keep
                 * running needs A without START, so the button set is
                 * selectable: RECOMP_FAKE_PAD=a presses A only. */
                if (fake_pad_start(t))
                    pState->Gamepad.wButtons |= XBOX_GAMEPAD_START;
                pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] = 255;
            }
        }
        g_pad_polls_connected++;
        pad_note_state(pState);
        return ERROR_SUCCESS;
    }

    refresh_controllers(0);

    SDL_GameController *c = g_pads[dwPort];
    if (!c || !SDL_GameControllerGetAttached(c)) {
        g_controller_connected[dwPort] = FALSE;
        g_pad_polls_disconnected++;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    SDL_GameControllerUpdate();
    g_controller_connected[dwPort] = TRUE;
    g_pad_polls_connected++;

    memset(pState, 0, sizeof(XBOX_INPUT_STATE));
    pState->dwPacketNumber = ++g_packet[dwPort];

    WORD btn = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    btn |= XBOX_GAMEPAD_DPAD_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  btn |= XBOX_GAMEPAD_DPAD_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  btn |= XBOX_GAMEPAD_DPAD_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) btn |= XBOX_GAMEPAD_DPAD_RIGHT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))      btn |= XBOX_GAMEPAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))       btn |= XBOX_GAMEPAD_BACK;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSTICK))  btn |= XBOX_GAMEPAD_LEFT_THUMB;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSTICK)) btn |= XBOX_GAMEPAD_RIGHT_THUMB;
    pState->Gamepad.wButtons = btn;

    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_A] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_B] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_X] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_X) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_Y] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_Y) ? 255 : 0;
    /* Controller S White became the left bumper and Black the right bumper
     * on later Xbox layouts. */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ? 255 : 0;
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] =
        SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) ? 255 : 0;

    /* SDL trigger axes are 0..32767 -> Xbox analog button 0..255 */
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] =
        (BYTE)(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    /* SDL Y axis points down; the Xbox Y axis points up -- invert.
     * Use (-1 - v) so v = -32768 does not overflow SHORT. */
    pState->Gamepad.sThumbLX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    pState->Gamepad.sThumbLY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    pState->Gamepad.sThumbRX = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    pState->Gamepad.sThumbRY =
        (SHORT)(-1 - SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY));

    pad_note_state(pState);
    return ERROR_SUCCESS;
}

DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pVibration)
        return ERROR_DEVICE_NOT_CONNECTED;

    SDL_GameController *c = g_pads[dwPort];
    if (!c) return ERROR_DEVICE_NOT_CONNECTED;

    /* SDL rumble needs a duration; refresh for ~1s on each call (the game
     * polls vibration continuously). */
    SDL_GameControllerRumble(c, pVibration->wLeftMotorSpeed,
                             pVibration->wRightMotorSpeed, 1000);
    return ERROR_SUCCESS;
}

BOOL xbox_InputIsConnected(DWORD dwPort)
{
    if (dwPort >= XBOX_MAX_CONTROLLERS) return FALSE;
    refresh_controllers(0);
    return g_controller_connected[dwPort];
}

DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps)
{
    (void)dwFlags;
    if (dwPort >= XBOX_MAX_CONTROLLERS || !pCaps)
        return ERROR_DEVICE_NOT_CONNECTED;
    refresh_controllers(0);
    if (!g_pads[dwPort])
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(XBOX_INPUT_CAPABILITIES));
    pCaps->Type    = 1;   /* XINPUT_DEVTYPE_GAMEPAD */
    pCaps->SubType = 1;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
    pCaps->Flags   = 0;
    return ERROR_SUCCESS;
}

#endif /* _WIN32 */
