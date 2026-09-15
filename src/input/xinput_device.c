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
#include <stdio.h>   /* both branches report pad-poll counters */

/* ======================================================================== */
#if defined(_WIN32)
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
/* ====================  XInput backend  ================================== */
/* ======================================================================== */

#include <xinput.h>
#pragma comment(lib, "xinput.lib")

static BOOL  g_controller_connected[XBOX_MAX_CONTROLLERS] = { FALSE };
static DWORD g_last_packet[XBOX_MAX_CONTROLLERS] = { 0 };

double xbox_InputSeconds(void)
{
    static DWORD t0;
    static int have_t0;
    DWORD now = GetTickCount();
    if (!have_t0) { t0 = now; have_t0 = 1; }
    return (double)(now - t0) / 1000.0;
}

void xbox_InputInit(void)
{
    for (DWORD i = 0; i < XBOX_MAX_CONTROLLERS; i++) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        g_controller_connected[i] = (result == ERROR_SUCCESS);
    }
}

/* Pad-poll accounting, mirroring the POSIX branch below field for field.
 *
 * These have to exist on BOTH hosts or the Windows build silently loses the
 * [PAD-POLL] line -- and a missing line reads exactly like polls=0, which is
 * one of the two failure signatures under investigation. A counter that cannot
 * move is worse than no counter, so these are wired to the real XInput path
 * rather than stubbed. */
unsigned long g_pad_polls;
unsigned long g_pad_polls_connected;
unsigned long g_pad_polls_nonneutral;
unsigned long g_pad_polls_disconnected;

/* Anything a human would call "pressed or moved". Same 4096 stick threshold as
 * the POSIX pad_note_state and the USB report path. */
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
}

void xbox_InputPollReport(void)
{
    fprintf(stderr, "  [PAD-POLL] polls=%lu connected=%lu nonneutral=%lu"
            " not_connected=%lu\n",
            g_pad_polls, g_pad_polls_connected, g_pad_polls_nonneutral,
            g_pad_polls_disconnected);
    fflush(stderr);
}

/* DirectInput fallback, for the pads XInput was never going to report.
 *
 * XInput only ever supported Xbox-family controllers. A DualShock 4 is not an
 * XInput device on real Windows either -- it needs DS4Windows or Steam Input to
 * be translated into one -- so XInputGetState answers ERROR_DEVICE_NOT_CONNECTED
 * on every port with the pad sitting right there. Measured in the CrossOver
 * bottle: XInput 1167 on all four ports, while DirectInput8 enumerates
 * "Wireless Controller", type 0x00010215 = DIDEVTYPE_HID | DI8DEVTYPE_GAMEPAD.
 *
 * The POSIX branch below never had this problem because it goes through SDL,
 * which ships a controller database. This is the Windows equivalent: enumerate
 * once, acquire, and present the result as an XINPUT_STATE so every line of the
 * mapping below stays exactly as it was.
 *
 * Only consulted when XInput says nothing is attached, so an Xbox pad keeps the
 * path it always had and this costs a poll that was going to fail anyway. */
static IDirectInput8A  *g_di;
static IDirectInputDevice8A *g_di_pad[XBOX_MAX_CONTROLLERS];
static int              g_di_count;
static int              g_di_tried;

static BOOL CALLBACK di_enum_cb(const DIDEVICEINSTANCEA *inst, void *ctx)
{
    IDirectInputDevice8A *dev = NULL;
    (void)ctx;
    if (g_di_count >= XBOX_MAX_CONTROLLERS)
        return DIENUM_STOP;
    if (FAILED(IDirectInput8_CreateDevice(g_di, &inst->guidInstance, &dev, NULL)))
        return DIENUM_CONTINUE;
    /* Background+nonexclusive: this process often has no foreground window at
     * all (the framebuffer window is optional), and an exclusive acquire would
     * simply fail there. */
    if (FAILED(IDirectInputDevice8_SetDataFormat(dev, &c_dfDIJoystick2)) ||
        FAILED(IDirectInputDevice8_SetCooperativeLevel(dev, NULL,
                   DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
        IDirectInputDevice8_Release(dev);
        return DIENUM_CONTINUE;
    }
    IDirectInputDevice8_Acquire(dev);
    g_di_pad[g_di_count++] = dev;
    fprintf(stderr, "  [PAD] DirectInput port %d: %s (type 0x%08lX)\n",
            g_di_count - 1, inst->tszProductName,
            (unsigned long)inst->dwDevType);
    fflush(stderr);
    return DIENUM_CONTINUE;
}

static void di_init_once(void)
{
    if (g_di_tried)
        return;
    g_di_tried = 1;
    if (FAILED(DirectInput8Create(GetModuleHandleA(NULL), DIRECTINPUT_VERSION,
                                  &IID_IDirectInput8A, (void **)&g_di, NULL))) {
        g_di = NULL;
        return;
    }
    IDirectInput8_EnumDevices(g_di, DI8DEVCLASS_GAMECTRL, di_enum_cb, NULL,
                              DIEDFL_ATTACHEDONLY);
    if (!g_di_count) {
        fprintf(stderr, "  [PAD] DirectInput: no attached game controllers\n");
        fflush(stderr);
    }
}

/* DS4 button order over HID, which is what DirectInput reports here:
 * 0 Square 1 Cross 2 Circle 3 Triangle 4 L1 5 R1 6 L2 7 R2
 * 8 Share 9 Options 10 L3 11 R3 12 PS 13 Touchpad.
 * Cross is the Xbox A, Circle is B, Square is X, Triangle is Y. */
static int di_get_state(DWORD port, XINPUT_STATE *out)
{
    DIJOYSTATE2 js;
    IDirectInputDevice8A *dev;
    static DWORD packet;
    unsigned i;

    di_init_once();
    if (port >= (DWORD)g_di_count || !(dev = g_di_pad[port]))
        return 0;
    if (FAILED(IDirectInputDevice8_Poll(dev))) {
        if (FAILED(IDirectInputDevice8_Acquire(dev)))
            return 0;
        IDirectInputDevice8_Poll(dev);
    }
    if (FAILED(IDirectInputDevice8_GetDeviceState(dev, sizeof js, &js)))
        return 0;

    memset(out, 0, sizeof *out);
    out->dwPacketNumber = ++packet;
    #define DIBTN(n) (js.rgbButtons[(n)] & 0x80)
    if (DIBTN(1))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_A;
    if (DIBTN(2))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_B;
    if (DIBTN(0))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_X;
    if (DIBTN(3))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_Y;
    if (DIBTN(4))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (DIBTN(5))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (DIBTN(8))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_BACK;
    if (DIBTN(9))  out->Gamepad.wButtons |= XINPUT_GAMEPAD_START;
    if (DIBTN(10)) out->Gamepad.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (DIBTN(11)) out->Gamepad.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    out->Gamepad.bLeftTrigger  = DIBTN(6) ? 255 : 0;
    out->Gamepad.bRightTrigger = DIBTN(7) ? 255 : 0;
    #undef DIBTN

    /* The hat switch carries the d-pad. -1 is centred; otherwise hundredths of
     * a degree clockwise from north. */
    for (i = 0; i < 4; i++) {
        DWORD pov = js.rgdwPOV[i];
        if (pov == (DWORD)-1 || LOWORD(pov) == 0xFFFF)
            continue;
        if (pov > 27000 || pov < 9000)   out->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
        if (pov > 0     && pov < 18000)  out->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (pov > 9000  && pov < 27000)  out->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (pov > 18000)                 out->Gamepad.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        break;
    }

    /* DirectInput axes are 0..65535 with c_dfDIJoystick2's default range;
     * XInput wants signed, and Y is inverted between the two. */
    #define AX(v)  ((SHORT)(((LONG)(v) - 32768) > 32767 ? 32767 : ((LONG)(v) - 32768)))
    out->Gamepad.sThumbLX =  AX(js.lX);
    out->Gamepad.sThumbLY = (SHORT)-AX(js.lY);
    out->Gamepad.sThumbRX =  AX(js.lZ);
    out->Gamepad.sThumbRY = (SHORT)-AX(js.lRz);
    #undef AX
    return 1;
}

DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState)
{
    XINPUT_STATE xi_state;
    DWORD result;

    if (dwPort >= XBOX_MAX_CONTROLLERS || !pState)
        return ERROR_DEVICE_NOT_CONNECTED;
    g_pad_polls++;

    result = XInputGetState(dwPort, &xi_state);
    if (result != ERROR_SUCCESS && di_get_state(dwPort, &xi_state))
        result = ERROR_SUCCESS;          /* a pad XInput cannot describe */
    if (result != ERROR_SUCCESS) {
        g_controller_connected[dwPort] = FALSE;
        g_pad_polls_disconnected++;
        return result;
    }

    g_controller_connected[dwPort] = TRUE;
    g_pad_polls_connected++;
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

    pad_note_state(pState);
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
#include <signal.h>      /* release the pad on SIGTERM/SIGINT */
#include <ctype.h>      /* RECOMP_PAD_SCRIPT parses its schedule */
#include <strings.h>    /* strncasecmp, so button names are case-insensitive */

static double fake_pad_seconds(void);
static int  pad_script_on(void);
static void pad_script_apply(XBOX_INPUT_STATE *st);

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

/* Give the pad back, and do it on the way out of a killed run.
 *
 * THE BUG THIS FIXES cost most of a hand-played session. Nothing released the
 * controller when the process died: SDL_GameControllerClose appears only in
 * the hotplug path above, there is no signal handler, and main.c deliberately
 * uses _exit so no atexit handler can run -- for good reasons, see its comment
 * about dumps confusing the run that follows. Every run therefore ended with
 * the HID interface still claimed by a dead process.
 *
 * The next run then opens the device, gets a handle, and receives nothing ever
 * again: polls climb, connected tracks them, nonneutral stays flat. It reads
 * exactly like a broken controller. On a DualShock 4 over USB it compounds --
 * the pad drops back to charge-only and will not send reports until its PS
 * button is pressed. Four reconnections went into that on 14 Sep 2026 before
 * anyone looked here.
 *
 * Deliberately minimal: this releases the pad and nothing else. No dumps, no
 * flushing of other instruments -- those are precisely what main.c avoids at
 * exit, and a handler that did more would be a different kind of problem.
 *
 * SIGKILL cannot be caught, so this only helps when a run is stopped with
 * SIGTERM or SIGINT. play.sh and play_scripted.sh already send TERM first and
 * escalate to -9 only after five seconds; stopping a run by hand should follow
 * the same order rather than reaching straight for kill -9. */
void xbox_InputReleasePads(void)
{
    for (int slot = 0; slot < XBOX_MAX_CONTROLLERS; slot++) {
        if (g_pads[slot]) {
            SDL_GameControllerClose(g_pads[slot]);
            g_pads[slot] = NULL;
            g_controller_connected[slot] = FALSE;
        }
    }
    if (SDL_WasInit(SDL_INIT_GAMECONTROLLER))
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

/* Re-raise with the default disposition so the exit status stays honest: a run
 * killed by SIGTERM still reports as killed by SIGTERM, which is what the
 * watchdogs in play.sh and play_scripted.sh expect to see. The once-guard means
 * a second signal arriving while we are still inside SDL takes the default
 * action immediately rather than deadlocking in here. */
static volatile sig_atomic_t g_release_in_progress;

static void pad_signal_handler(int sig)
{
    if (!g_release_in_progress) {
        g_release_in_progress = 1;
        xbox_InputReleasePads();
    }
    signal(sig, SIG_DFL);
    raise(sig);
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

    /* AFTER SDL is up, and that ordering is the whole point.
     *
     * SDL_InitSubSystem installs signal handlers of its own -- it catches
     * SIGTERM and SIGINT to post a quit event rather than die. Installing ours
     * first, which is what this did at first and what its comment argued for,
     * means SDL simply overwrites them: the handler never runs, the pad is
     * never released, and nothing says so. Caught by input_release_test, whose
     * child exited 42 instead of dying by SIGTERM.
     *
     * The cost of installing after is a window during subsystem init where a
     * signal takes SDL's disposition instead of ours. That window is
     * milliseconds and leaks nothing that was not already leaking; being
     * clobbered for the entire run is the worse failure by far. */
    signal(SIGTERM, pad_signal_handler);
    signal(SIGINT, pad_signal_handler);

    refresh_controllers(1);

    /* Anchor RECOMP_PAD_SCRIPT's t=0 here rather than at the first poll. The
     * guest does not poll until its USB driver is up, and how long that takes
     * varies, so a schedule measured from the first poll is not the same
     * schedule twice. Parsing here also puts any complaint about the schedule
     * at the top of the log rather than minutes into the run. */
    fake_pad_seconds();
    (void)pad_script_on();
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
    /* PRESENCE, DELIBERATELY, and not an oversight of the 15 Sep sweep that
     * converted ten other switches to recomp_switch_on().
     *
     * This switch's VALUE selects the START policy, read below: 1, <n>, `a`
     * and `always` are all meaningful and all mean "the fake pad is on".
     * recomp_switch_on() would keep every one of those working, but a future
     * atoi() conversion would silently turn `RECOMP_FAKE_PAD=a` off, which is
     * the spelling that presses A without pausing the game. Presence is the
     * right test here; the value question is answered separately. */
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

double xbox_InputSeconds(void) { return fake_pad_seconds(); }

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

/* RECOMP_PAD_SCRIPT -- a timed list of presses, so an unattended run can steer.
 *
 * The pulse above cannot reach a new game. It presses the same two buttons on
 * a blind one-per-second beat, which walks the title as far as live=133 and
 * 1342 file opens and stops there; New Game's live=56 / 1408 has only ever
 * been reached by a person holding the pad. That made every measurement past
 * the title screen cost a human playthrough, and section 4 of the 12 Sep
 * handover is what happens when a measurement is expensive enough to tempt
 * you into inferring it instead.
 *
 * A schedule is the whole difference: menus need DOWN and A at particular
 * moments, not A forever, and a confirm whose default is "no" is invisible to
 * a masher. So:
 *
 *   RECOMP_PAD_SCRIPT="2:START 6:A 9:DOWN 10:A"        inline
 *   RECOMP_PAD_SCRIPT=@diagnostics/.../new_game.pad    one event per line
 *
 * Each event is  <t>:<BUTTONS>[:<hold>]  -- t seconds after xbox_InputInit,
 * BUTTONS one or more names joined by '+', hold in seconds (default 0.20,
 * which is 12 frames at 60 Hz). Events may overlap; they are OR'd together.
 *
 * Names: START BACK LTHUMB RTHUMB UP DOWN LEFT RIGHT
 *        A B X Y WHITE BLACK LT RT
 *        LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT   (stick deflections)
 *
 * Unlike RECOMP_FAKE_PAD this MERGES rather than replaces: with a controller
 * attached the live report is read first and the scripted presses are OR'd
 * into it, so a person can take over a run the script has driven into place.
 * With no controller attached port 0 still reports connected, which is what
 * makes an unattended run possible at all.
 */
#define PAD_SCRIPT_MAX      256
#define PAD_SCRIPT_HOLD     0.20

struct pad_script_ev {
    double t0, t1;
    WORD   buttons;
    BYTE   analog[8];
    SHORT  lx, ly, rx, ry;
    int    fired;
};

static struct pad_script_ev g_pad_script[PAD_SCRIPT_MAX];
static int g_pad_script_n = -1;

static const struct { const char *name; WORD bit; } pad_script_bits[] = {
    { "START",  XBOX_GAMEPAD_START      }, { "BACK",   XBOX_GAMEPAD_BACK       },
    { "LTHUMB", XBOX_GAMEPAD_LEFT_THUMB }, { "RTHUMB", XBOX_GAMEPAD_RIGHT_THUMB},
    { "UP",     XBOX_GAMEPAD_DPAD_UP    }, { "DOWN",   XBOX_GAMEPAD_DPAD_DOWN  },
    { "LEFT",   XBOX_GAMEPAD_DPAD_LEFT  }, { "RIGHT",  XBOX_GAMEPAD_DPAD_RIGHT },
};
static const struct { const char *name; int idx; } pad_script_analog[] = {
    { "A", XBOX_BUTTON_A }, { "B", XBOX_BUTTON_B },
    { "X", XBOX_BUTTON_X }, { "Y", XBOX_BUTTON_Y },
    { "WHITE", XBOX_BUTTON_WHITE }, { "BLACK", XBOX_BUTTON_BLACK },
    { "LT", XBOX_BUTTON_LTRIGGER }, { "RT", XBOX_BUTTON_RTRIGGER },
};
/* Full deflection, not the 4096 the report path uses to reject jitter: a menu
 * that reads the stick as a d-pad wants an unambiguous push. */
#define PAD_SCRIPT_STICK 30000
static const struct { const char *name; int axis; int sign; } pad_script_stick[] = {
    { "LLEFT", 0, -1 }, { "LRIGHT", 0, +1 }, { "LDOWN", 1, -1 }, { "LUP", 1, +1 },
    { "RLEFT", 2, -1 }, { "RRIGHT", 2, +1 }, { "RDOWN", 3, -1 }, { "RUP", 3, +1 },
};

/* One "NAME" or "NAME+NAME+..." into an event. Returns 0 on an unknown name,
 * which is reported rather than ignored -- a typo in a schedule is otherwise a
 * silent hour of watching the title not do the thing you asked for. */
static int pad_script_buttons(const char *s, size_t len, struct pad_script_ev *ev)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i, n;
        unsigned k;
        int hit = 0;
        while (j < len && s[j] != '+') j++;
        n = j - i;
        for (k = 0; !hit && k < sizeof pad_script_bits / sizeof pad_script_bits[0]; k++)
            if (strlen(pad_script_bits[k].name) == n
                && !strncasecmp(s + i, pad_script_bits[k].name, n)) {
                ev->buttons |= pad_script_bits[k].bit; hit = 1;
            }
        for (k = 0; !hit && k < sizeof pad_script_stick / sizeof pad_script_stick[0]; k++)
            if (strlen(pad_script_stick[k].name) == n
                && !strncasecmp(s + i, pad_script_stick[k].name, n)) {
                SHORT v = (SHORT)(pad_script_stick[k].sign * PAD_SCRIPT_STICK);
                switch (pad_script_stick[k].axis) {
                case 0: ev->lx = v; break;  case 1: ev->ly = v; break;
                case 2: ev->rx = v; break;  default: ev->ry = v; break;
                }
                hit = 1;
            }
        for (k = 0; !hit && k < sizeof pad_script_analog / sizeof pad_script_analog[0]; k++)
            if (strlen(pad_script_analog[k].name) == n
                && !strncasecmp(s + i, pad_script_analog[k].name, n)) {
                ev->analog[pad_script_analog[k].idx] = 255; hit = 1;
            }
        if (!hit) {
            fprintf(stderr, "  [PAD-SCRIPT] unknown button '%.*s'\n", (int)n, s + i);
            return 0;
        }
        i = j + 1;
    }
    return 1;
}

/* Whitespace-, comma- and semicolon-separated events; '#' to end of line is a
 * comment, so an inline value and a file are the same grammar. */
static void pad_script_parse(const char *text)
{
    const char *p = text;
    int bad = 0;
    g_pad_script_n = 0;
    while (*p) {
        const char *tok, *colon, *end;
        struct pad_script_ev ev;
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
        if (*p == '#') { while (*p && *p != '\n') p++; continue; }
        if (!*p) break;
        tok = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ';' && *p != '#') p++;
        end = p;
        colon = memchr(tok, ':', (size_t)(end - tok));
        if (!colon) {
            fprintf(stderr, "  [PAD-SCRIPT] ignoring '%.*s': expected <t>:<BUTTONS>\n",
                    (int)(end - tok), tok);
            bad++;
            continue;
        }
        memset(&ev, 0, sizeof ev);
        ev.t0 = strtod(tok, NULL);
        {
            const char *hold = memchr(colon + 1, ':', (size_t)(end - colon - 1));
            const char *blast = hold ? hold : end;
            double h = hold ? strtod(hold + 1, NULL) : PAD_SCRIPT_HOLD;
            if (h <= 0.0) h = PAD_SCRIPT_HOLD;
            ev.t1 = ev.t0 + h;
            if (!pad_script_buttons(colon + 1, (size_t)(blast - colon - 1), &ev)) {
                bad++;
                continue;
            }
        }
        if (g_pad_script_n < PAD_SCRIPT_MAX)
            g_pad_script[g_pad_script_n++] = ev;
        else
            bad++;
    }
    fprintf(stderr, "  [PAD-SCRIPT] %d event(s) loaded%s; t=0 is input init\n",
            g_pad_script_n, bad ? " (some rejected -- see above)" : "");
    fflush(stderr);
}

static int pad_script_on(void)
{
    if (g_pad_script_n < 0) {
        const char *v = getenv("RECOMP_PAD_SCRIPT");
        g_pad_script_n = 0;
        if (v && *v == '@') {
            FILE *f = fopen(v + 1, "rb");
            if (!f) {
                fprintf(stderr, "  [PAD-SCRIPT] cannot open %s\n", v + 1);
                fflush(stderr);
            } else {
                char *buf; long sz;
                fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
                buf = (char *)malloc((size_t)sz + 1);
                if (buf && sz >= 0 && fread(buf, 1, (size_t)sz, f) == (size_t)sz) {
                    buf[sz] = 0;
                    pad_script_parse(buf);
                }
                free(buf);
                fclose(f);
            }
        } else if (v && *v) {
            pad_script_parse(v);
        }
    }
    return g_pad_script_n > 0;
}

/* OR the schedule into whatever the caller has already filled in. A scripted
 * stick deflection yields to a live one, so taking over a run mid-flight does
 * not fight the script for the camera. */
static void pad_script_apply(XBOX_INPUT_STATE *st)
{
    double t;
    int i;

    if (!pad_script_on()) return;
    t = fake_pad_seconds();
    for (i = 0; i < g_pad_script_n; i++) {
        struct pad_script_ev *ev = &g_pad_script[i];
        int j;
        if (t < ev->t0 || t >= ev->t1) continue;
        if (!ev->fired) {
            ev->fired = 1;
            fprintf(stderr, "  [PAD-SCRIPT] t=%7.2f fire #%d buttons=%04X"
                    " a=%u b=%u lx=%d ly=%d poll=%lu\n",
                    t, i, (unsigned)ev->buttons, (unsigned)ev->analog[XBOX_BUTTON_A],
                    (unsigned)ev->analog[XBOX_BUTTON_B], (int)ev->lx, (int)ev->ly,
                    g_pad_polls);
            fflush(stderr);
        }
        st->Gamepad.wButtons |= ev->buttons;
        for (j = 0; j < 8; j++)
            if (ev->analog[j] > st->Gamepad.bAnalogButtons[j])
                st->Gamepad.bAnalogButtons[j] = ev->analog[j];
        if (ev->lx && st->Gamepad.sThumbLX > -4096 && st->Gamepad.sThumbLX < 4096)
            st->Gamepad.sThumbLX = ev->lx;
        if (ev->ly && st->Gamepad.sThumbLY > -4096 && st->Gamepad.sThumbLY < 4096)
            st->Gamepad.sThumbLY = ev->ly;
        if (ev->rx && st->Gamepad.sThumbRX > -4096 && st->Gamepad.sThumbRX < 4096)
            st->Gamepad.sThumbRX = ev->rx;
        if (ev->ry && st->Gamepad.sThumbRY > -4096 && st->Gamepad.sThumbRY < 4096)
            st->Gamepad.sThumbRY = ev->ry;
    }
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

    /* The triggers are in the signature above but were not in this line, so a
     * pull could change the state and leave the log looking idle -- which is
     * exactly the question asked when the tutorial stopped at "pull the Right
     * Trigger". Print them; they are analog buttons on this pad, not axes. */
    fprintf(stderr, "  [PAD-TRACE] t=%7.2f %s buttons=%04X a=%3u b=%3u"
            " x=%3u y=%3u lt=%3u rt=%3u lx=%6d ly=%6d poll=%lu\n",
            fake_pad_seconds(), active ? "PRESSED " : "released",
            (unsigned)st->Gamepad.wButtons,
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_A],
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_B],
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_X],
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_Y],
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER],
            (unsigned)st->Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER],
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
        if (pad_script_on()) {
            /* A schedule is strictly better than the pulse -- it can press
             * DOWN, and it stops pressing START -- so it wins outright. */
            pad_script_apply(pState);
            g_pad_polls_connected++;
            pad_note_state(pState);
            return ERROR_SUCCESS;
        }
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
        /* With a schedule loaded, port 0 is a pad whether or not hardware is
         * attached -- otherwise an unattended run reports "not connected" and
         * the title waits for a controller that is never going to arrive. */
        if (dwPort == 0 && pad_script_on()) {
            memset(pState, 0, sizeof(XBOX_INPUT_STATE));
            pState->dwPacketNumber = ++g_packet[0];
            pad_script_apply(pState);
            g_controller_connected[0] = TRUE;
            g_pad_polls_connected++;
            pad_note_state(pState);
            return ERROR_SUCCESS;
        }
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

    /* MERGE, not replace: RECOMP_FAKE_PAD replaces the real pad and so
     * silently eats every press a person makes, which cost an afternoon. A
     * schedule that OR's itself into the live report lets a person take over
     * a run the script has driven into place. */
    if (dwPort == 0) pad_script_apply(pState);

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
    if (dwPort == 0 && (fake_pad_on() || pad_script_on())) return TRUE;
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
