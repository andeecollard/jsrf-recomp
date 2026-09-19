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

/* ─────────────────────────────────────────────────────────────────────────
 * THE FRAME THE GUEST COUNTED.
 *
 * Everything keyed to "a frame" below is keyed to this, and the choice is the
 * whole point: two of the three counters that were available are wall clocks
 * wearing a frame's clothes. Measured, from one player session's four 5 s
 * reports (~/Library/Application Support/JSRF/last-run.log, 19 Sep 2026):
 *
 *   pad polls       592, then +604, +618, +617 per report -- FLAT. It is the
 *                   guest's USB interrupt schedule, i.e. a 120 Hz timer.
 *   xbox_InputSeconds()  the wall clock outright. This is what the existing
 *                   RECOMP_PAD_SCRIPT schedule keys on, and it is why the
 *                   19 Sep runs reached gameplay at 64-86 fps and parked at
 *                   the title screen in every run above ~117 fps.
 *   FLIP_STALL      1009, then +885, +658, +120 per report over the same
 *                   four reports -- it moved by a factor of seven while the
 *                   clock did not. That is the title's own frame.
 *
 * FLIP_STALL is a method the TITLE writes into its push buffer, so the count
 * is the guest's and not ours. This counter is therefore advanced by whoever
 * consumes that flip -- in this tree diagnostics/jsrf_first_fault/main.c, at
 * pgraph_d3d11_take_frame() -- and by nothing else. It is deliberately not
 * derived from a host timer, a vblank or a present: a replay keyed to any of
 * those replays at the host's speed rather than the guest's, which is the
 * bug this exists to remove.
 *
 * One writer (the push-buffer thread), several readers (the pad poll path,
 * which runs on the USB thread and on RECOMP_PAD_INJECT's thread). An aligned
 * word load cannot tear on either supported host, and a reader that is one
 * frame behind simply repeats a frame's input, so no lock is taken here.
 */
static volatile unsigned long g_pad_guest_frame;

void xbox_InputFrameAdvance(void) { g_pad_guest_frame++; }
unsigned long xbox_InputFrame(void) { return g_pad_guest_frame; }

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
#include <stdarg.h>     /* the recorder formats one line per run */
#include <errno.h>      /* a failed recording says WHY it failed */
#include <pthread.h>    /* the pad is polled from more than one thread */
#include <unistd.h>     /* getpid, for the default recording name */
#include <sys/stat.h>   /* mkdir, for the support directory */
#include "../recomp_switch.h"

static double fake_pad_seconds(void);
static int  pad_script_on(void);
static void pad_script_apply(XBOX_INPUT_STATE *st);
static void pad_record_sample(const XBOX_INPUT_STATE *st);

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
    /* And the recording. play.sh and play_scripted.sh both send SIGTERM
     * before escalating, so this is the path a normal unattended run takes
     * out -- without it the last run of input, which is the interesting one
     * if the session was stopped because something went wrong, is still
     * sitting in stdio's buffer. */
    xbox_PadRecordFlush();
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
    xbox_PadRecordReport();
    fflush(stderr);
    w32_thread_trace_report();
}

/* RECOMP_PAD_SCRIPT and RECOMP_PAD_RECORD -- one format, both directions.
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
 * Each event is  <t>:<BUTTONS>[:<hold>]  -- BUTTONS one or more names joined
 * by '+'. Events may overlap; they are OR'd together.
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
 *
 * ── WHY <t> MAY NOW BE A FRAME ────────────────────────────────────────────
 *
 * <t> as SECONDS is why the harness is unreliable. new_game.pad's own header
 * says the logos vary by ten seconds; on 19 Sep 2026 the same schedule
 * reached gameplay at 64-86 fps and parked at the title screen in every run
 * above ~117 fps, because the presses had all been spent before the menu
 * arrived. A press timed against the wall cannot be deterministic in a title
 * whose boot time depends on how fast the host is that afternoon.
 *
 * The title is frame-locked, so the fix is to key on the frame instead:
 *
 *   f120:A            frame 120, held PAD_SCRIPT_HOLD_FRAMES frames
 *   f120:A=255:6      frame 120, held 6 frames, A at exactly 255
 *   f120:LX=-12000    frame 120, left stick X at exactly -12000
 *
 * A token may carry "=<value>" so a recording can state the analog value it
 * saw rather than approximating it with a name: bare A is still 255 and bare
 * LLEFT is still full deflection, which is what a hand-written schedule
 * wants, but LX/LY/RX/RY REQUIRE a value and BTN=<hex> sets the digital word
 * outright. Time-keyed and frame-keyed events may be mixed in one file; they
 * are independent.
 *
 * ── AND WHY A RECORDING IS THE SAME FILE ──────────────────────────────────
 *
 * RECOMP_PAD_RECORD=1 writes the live pad out in exactly the grammar above,
 * frame-keyed, so a recording IS a pad script and replaying one is
 * RECOMP_PAD_SCRIPT=@<the file>. There is deliberately no second mechanism:
 * one parser, one apply hook, one set of button names, and a recording can be
 * opened in an editor and trimmed like any other schedule.
 *
 * A recording announces itself with a "#!padrec" directive, and that changes
 * two things, both of which a replay needs and a hand-written schedule must
 * not get:
 *
 *   REPLACE, NOT MERGE. The events are the whole pad state for the frames
 *   they cover, and frames no event covers are neutral. Merging would OR a
 *   live controller into the replay and quietly change it.
 *
 *   A HEADER THAT CAN REFUSE. #!build and #!gen carry the binary and the
 *   generated tree the recording was taken against; a mismatch is refused
 *   outright, because replaying a session into different code produces a
 *   result that looks like a measurement and is not one. #!switches carries a
 *   hash of the RECOMP_* environment, which is reported rather than refused:
 *   29 switches routinely differ between a scripted run and the player's
 *   bundle (19 Sep handover, item 0.4), so refusing on it would refuse every
 *   replay anybody actually wants.
 *
 * ── TELLING A DIVERGED REPLAY FROM A DRIFTING ONE ─────────────────────────
 *
 * The recording also carries #!ck checkpoints: a rolling hash of the pad
 * state, folded one RUN at a time (a run is a maximal span of frames over
 * which the state did not change), emitted every PAD_REC_CK_RUNS runs. The
 * replay folds the same runs out of the event list and compares. That
 * catches every way the input stream itself can go wrong -- a truncated
 * file, an event dropped by the parser, a frame counter that is not
 * advancing, a hand edit -- and it deliberately does not claim more than
 * that: nothing in the input layer can tell you the GAME diverged, only that
 * the input it was given did.
 */
#define PAD_SCRIPT_MAX          256       /* hand-written schedules only; a
                                           * recording grows without limit */
#define PAD_SCRIPT_HOLD         0.20
#define PAD_SCRIPT_HOLD_FRAMES  12u       /* 0.20 s at 60 Hz, the same beat */
#define PAD_REC_VERSION         1
#define PAD_REC_CK_RUNS         256u
#define PAD_REC_FLUSH_RUNS      32u

/* Full deflection, not the 4096 the report path uses to reject jitter: a menu
 * that reads the stick as a d-pad wants an unambiguous push. */
#define PAD_SCRIPT_STICK 30000

struct pad_state {
    WORD  buttons;
    BYTE  analog[8];
    SHORT lx, ly, rx, ry;
};

struct pad_script_ev {
    int              frame_keyed;
    double           t0, t1;          /* seconds, when !frame_keyed */
    unsigned long    f0, f1;          /* frames,  when  frame_keyed */
    struct pad_state s;
    int              fired;
};

/* A maximal span of frames over which the pad did not change. The replay
 * applies runs and the checkpoint hash folds runs, so both sides agree
 * without either having to guess how often the other was polled. */
struct pad_run {
    unsigned long    f0, f1;
    struct pad_state s;
};

static struct pad_script_ev *g_pad_script;
static int g_pad_script_n = -1;          /* -1: not parsed yet */
static int g_pad_script_cap;

/* Recording header, as read out of a file being replayed. */
static int                g_padrec_seen;
static int                g_padrec_version;
static char               g_padrec_build[96];
static char               g_padrec_gen[96];
static unsigned long long g_padrec_switches;
static unsigned long      g_padrec_frames;   /* 0: no end marker (a killed run) */
static int                g_padrec_refused;

/* Who this binary is, for the header. main.c stamps it from the same CMake
 * definitions that produce the [GEN] and [BUILD] banner lines; left
 * "unstamped" a recording still replays, and says so. */
static char g_pad_build_id[96] = "unstamped";
static char g_pad_gen_id[96]   = "unstamped";

void xbox_PadRecordSetIdentity(const char *build, const char *gen)
{
    if (build && *build) snprintf(g_pad_build_id, sizeof g_pad_build_id, "%s", build);
    if (gen   && *gen)   snprintf(g_pad_gen_id,   sizeof g_pad_gen_id,   "%s", gen);
}

/* ── FNV-1a, over a canonical encoding ───────────────────────────────────
 *
 * Explicit bytes rather than hashing the struct: struct padding is
 * indeterminate, so hashing it directly would compare a recorder's padding
 * against a replayer's and could differ between two builds of the same file.
 */
#define PAD_FNV_OFFSET 1469598103934665603ULL
#define PAD_FNV_PRIME  1099511628211ULL

static unsigned long long pad_fnv(unsigned long long h, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= PAD_FNV_PRIME; }
    return h;
}

static unsigned long long pad_fold_run(unsigned long long h,
                                       const struct pad_state *s,
                                       unsigned long len)
{
    unsigned char buf[22];
    int i;
    buf[0] = (unsigned char)(s->buttons & 0xFFu);
    buf[1] = (unsigned char)(s->buttons >> 8);
    for (i = 0; i < 8; i++) buf[2 + i] = s->analog[i];
    buf[10] = (unsigned char)((unsigned)s->lx & 0xFFu);
    buf[11] = (unsigned char)(((unsigned)s->lx >> 8) & 0xFFu);
    buf[12] = (unsigned char)((unsigned)s->ly & 0xFFu);
    buf[13] = (unsigned char)(((unsigned)s->ly >> 8) & 0xFFu);
    buf[14] = (unsigned char)((unsigned)s->rx & 0xFFu);
    buf[15] = (unsigned char)(((unsigned)s->rx >> 8) & 0xFFu);
    buf[16] = (unsigned char)((unsigned)s->ry & 0xFFu);
    buf[17] = (unsigned char)(((unsigned)s->ry >> 8) & 0xFFu);
    buf[18] = (unsigned char)(len & 0xFFu);
    buf[19] = (unsigned char)((len >> 8) & 0xFFu);
    buf[20] = (unsigned char)((len >> 16) & 0xFFu);
    buf[21] = (unsigned char)((len >> 24) & 0xFFu);
    return pad_fnv(h, buf, sizeof buf);
}

static int pad_state_eq(const struct pad_state *a, const struct pad_state *b)
{
    return a->buttons == b->buttons && a->lx == b->lx && a->ly == b->ly
        && a->rx == b->rx && a->ry == b->ry
        && !memcmp(a->analog, b->analog, sizeof a->analog);
}

static int pad_state_neutral(const struct pad_state *s)
{
    static const struct pad_state zero;
    return pad_state_eq(s, &zero);
}

static void pad_state_from_report(struct pad_state *s, const XBOX_INPUT_STATE *st)
{
    s->buttons = st->Gamepad.wButtons;
    memcpy(s->analog, st->Gamepad.bAnalogButtons, sizeof s->analog);
    s->lx = st->Gamepad.sThumbLX; s->ly = st->Gamepad.sThumbLY;
    s->rx = st->Gamepad.sThumbRX; s->ry = st->Gamepad.sThumbRY;
}

static void pad_state_to_report(const struct pad_state *s, XBOX_INPUT_STATE *st)
{
    st->Gamepad.wButtons = s->buttons;
    memcpy(st->Gamepad.bAnalogButtons, s->analog, sizeof s->analog);
    st->Gamepad.sThumbLX = s->lx; st->Gamepad.sThumbLY = s->ly;
    st->Gamepad.sThumbRX = s->rx; st->Gamepad.sThumbRY = s->ry;
}

/* ── the RECOMP_* environment, hashed ────────────────────────────────────
 *
 * Sorted, so two runs that set the same switches in a different order agree.
 * RECOMP_PAD_* and RECOMP_FAKE_PAD are EXCLUDED by construction: the pad
 * switches are exactly the ones that must differ between a recording run
 * (RECOMP_PAD_RECORD=1) and its replay (RECOMP_PAD_SCRIPT=@file), so
 * including them would make every recording mismatch its own replay.
 */
#if defined(__APPLE__)
#include <crt_externs.h>
#define PAD_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define PAD_ENVIRON environ
#endif

static int pad_env_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static unsigned long long pad_switch_hash(void)
{
    static unsigned long long cached;
    static int have;
    const char *pick[512];
    int n = 0, i;
    char **e;
    if (have) return cached;
    have = 1;
    for (e = PAD_ENVIRON; e && *e && n < (int)(sizeof pick / sizeof pick[0]); e++) {
        if (strncmp(*e, "RECOMP_", 7) != 0) continue;
        if (strncmp(*e, "RECOMP_PAD_", 11) == 0) continue;
        if (strncmp(*e, "RECOMP_FAKE_PAD", 15) == 0) continue;
        pick[n++] = *e;
    }
    qsort((void *)pick, (size_t)n, sizeof pick[0], pad_env_cmp);
    cached = PAD_FNV_OFFSET;
    for (i = 0; i < n; i++)
        cached = pad_fnv(cached, pick[i], strlen(pick[i]) + 1);
    return cached;
}

/* ── parsing ─────────────────────────────────────────────────────────────*/

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
static const struct { const char *name; int axis; int sign; } pad_script_stick[] = {
    { "LLEFT", 0, -1 }, { "LRIGHT", 0, +1 }, { "LDOWN", 1, -1 }, { "LUP", 1, +1 },
    { "RLEFT", 2, -1 }, { "RRIGHT", 2, +1 }, { "RDOWN", 3, -1 }, { "RUP", 3, +1 },
};
/* The raw axes, which a recording uses because a name cannot say -12000. */
static const struct { const char *name; int axis; } pad_script_axis[] = {
    { "LX", 0 }, { "LY", 1 }, { "RX", 2 }, { "RY", 3 },
};

static void pad_axis_set(struct pad_state *s, int axis, SHORT v)
{
    switch (axis) {
    case 0: s->lx = v; break;
    case 1: s->ly = v; break;
    case 2: s->rx = v; break;
    default: s->ry = v; break;
    }
}

static int pad_name_is(const char *s, size_t n, const char *name)
{
    return strlen(name) == n && !strncasecmp(s, name, n);
}

/* One "NAME" or "NAME+NAME=VALUE+..." into an event. Returns 0 on an unknown
 * name, which is reported rather than ignored -- a typo in a schedule is
 * otherwise a silent hour of watching the title not do the thing you asked
 * for. */
static int pad_script_buttons(const char *s, size_t len, struct pad_state *out)
{
    size_t i = 0;
    while (i < len) {
        size_t j = i, n, nlen;
        const char *tok, *eq;
        int have_val = 0;
        unsigned k;
        int hit = 0;
        while (j < len && s[j] != '+') j++;
        n = j - i;
        tok = s + i;
        eq = (const char *)memchr(tok, '=', n);
        nlen = eq ? (size_t)(eq - tok) : n;
        have_val = eq != NULL;

        for (k = 0; !hit && k < sizeof pad_script_bits / sizeof pad_script_bits[0]; k++)
            if (pad_name_is(tok, nlen, pad_script_bits[k].name)) {
                out->buttons |= pad_script_bits[k].bit; hit = 1;
            }
        if (!hit && pad_name_is(tok, nlen, "BTN")) {
            if (!have_val) {
                fprintf(stderr, "  [PAD-SCRIPT] BTN needs a value, as BTN=0x0010\n");
                return 0;
            }
            out->buttons |= (WORD)strtoul(eq + 1, NULL, 0);
            hit = 1;
        }
        for (k = 0; !hit && k < sizeof pad_script_axis / sizeof pad_script_axis[0]; k++)
            if (pad_name_is(tok, nlen, pad_script_axis[k].name)) {
                long v;
                if (!have_val) {
                    fprintf(stderr, "  [PAD-SCRIPT] %.*s needs a value, as %.*s=-12000"
                            " (the named deflections LLEFT/LRIGHT/... do not)\n",
                            (int)nlen, tok, (int)nlen, tok);
                    return 0;
                }
                v = strtol(eq + 1, NULL, 10);
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                pad_axis_set(out, pad_script_axis[k].axis, (SHORT)v);
                hit = 1;
            }
        for (k = 0; !hit && k < sizeof pad_script_stick / sizeof pad_script_stick[0]; k++)
            if (pad_name_is(tok, nlen, pad_script_stick[k].name)) {
                pad_axis_set(out, pad_script_stick[k].axis,
                             (SHORT)(pad_script_stick[k].sign * PAD_SCRIPT_STICK));
                hit = 1;
            }
        for (k = 0; !hit && k < sizeof pad_script_analog / sizeof pad_script_analog[0]; k++)
            if (pad_name_is(tok, nlen, pad_script_analog[k].name)) {
                long v = have_val ? strtol(eq + 1, NULL, 10) : 255;
                if (v > 255) v = 255;
                if (v < 0)   v = 0;
                out->analog[pad_script_analog[k].idx] = (BYTE)v;
                hit = 1;
            }
        if (!hit) {
            fprintf(stderr, "  [PAD-SCRIPT] unknown button '%.*s'\n", (int)nlen, tok);
            return 0;
        }
        i = j + 1;
    }
    return 1;
}

/* ── the "#!" directives a recording carries ─────────────────────────────*/

struct pad_ck { unsigned long run, frame, anchor; unsigned long long hash; };
static struct pad_ck *g_padrec_ck;
static int g_padrec_ck_n, g_padrec_ck_cap;

/* Marks: "the text was wrong HERE", as a guest frame and a label. One table
 * for both directions -- a `#!mark` read from a recording and a mark made
 * live while recording land in the same place -- so a probe asking "am I
 * near a symptom" gets one answer whichever way the session is being run.
 * Fixed-size: a session with more than 64 marks is a session that should
 * have been several. */
#define PAD_MARK_MAX 64
#define PAD_MARK_LABEL 48
struct pad_mark { unsigned long frame; char label[PAD_MARK_LABEL]; };
static struct pad_mark g_pad_marks[PAD_MARK_MAX];
static int g_pad_marks_n;
static int g_pad_mark_echoed;            /* replay: marks already announced */

static void pad_mark_push(unsigned long frame, const char *label, size_t n)
{
    struct pad_mark *m;
    if (g_pad_marks_n >= PAD_MARK_MAX) return;
    m = &g_pad_marks[g_pad_marks_n++];
    m->frame = frame;
    if (n >= sizeof m->label) n = sizeof m->label - 1;
    memcpy(m->label, label, n);
    m->label[n] = 0;
}

static void pad_ck_push(unsigned long run, unsigned long frame,
                        unsigned long long h, unsigned long anchor)
{
    if (g_padrec_ck_n >= g_padrec_ck_cap) {
        int cap = g_padrec_ck_cap ? g_padrec_ck_cap * 2 : 64;
        struct pad_ck *p = (struct pad_ck *)realloc(g_padrec_ck, (size_t)cap * sizeof *p);
        if (!p) return;
        g_padrec_ck = p; g_padrec_ck_cap = cap;
    }
    g_padrec_ck[g_padrec_ck_n].run = run;
    g_padrec_ck[g_padrec_ck_n].frame = frame;
    g_padrec_ck[g_padrec_ck_n].hash = h;
    g_padrec_ck[g_padrec_ck_n].anchor = anchor;
    g_padrec_ck_n++;
}

static void pad_copy_word(char *dst, size_t dstn, const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && i + 1 < dstn && !isspace((unsigned char)s[i])) { dst[i] = s[i]; i++; }
    dst[i] = 0;
}

/* `s` is the whole comment, "#!..." included, without its newline. */
static void pad_script_directive(const char *s, size_t len)
{
    const char *p = s + 2, *end = s + len, *w;
    size_t wn;
    while (p < end && isspace((unsigned char)*p)) p++;
    w = p;
    while (p < end && !isspace((unsigned char)*p)) p++;
    wn = (size_t)(p - w);
    while (p < end && isspace((unsigned char)*p)) p++;

    if (pad_name_is(w, wn, "padrec")) {
        g_padrec_seen = 1;
        g_padrec_version = (int)strtol(p, NULL, 10);
    } else if (pad_name_is(w, wn, "build")) {
        pad_copy_word(g_padrec_build, sizeof g_padrec_build, p, (size_t)(end - p));
    } else if (pad_name_is(w, wn, "gen")) {
        pad_copy_word(g_padrec_gen, sizeof g_padrec_gen, p, (size_t)(end - p));
    } else if (pad_name_is(w, wn, "switches")) {
        g_padrec_switches = strtoull(p, NULL, 16);
    } else if (pad_name_is(w, wn, "frames")) {
        g_padrec_frames = strtoul(p, NULL, 10);
    } else if (pad_name_is(w, wn, "ck")) {
        char *q = NULL;
        unsigned long run = strtoul(p, &q, 10);
        unsigned long fr  = q ? strtoul(q, &q, 10) : 0;
        unsigned long long h = q ? strtoull(q, &q, 16) : 0;
        unsigned long an = q ? strtoul(q, NULL, 16) : 0;
        pad_ck_push(run, fr, h, an);
    } else if (pad_name_is(w, wn, "mark")) {
        char *q = NULL;
        unsigned long fr = strtoul(p, &q, 10);
        const char *lab = q ? q : end;
        while (lab < end && isspace((unsigned char)*lab)) lab++;
        pad_mark_push(fr, lab, (size_t)(end - lab));
    } else {
        fprintf(stderr, "  [PAD-SCRIPT] unknown directive '%.*s'\n", (int)wn, w);
    }
}

static int pad_script_push(const struct pad_script_ev *ev)
{
    if (!g_padrec_seen && g_pad_script_n >= PAD_SCRIPT_MAX)
        return 0;                    /* hand-written schedules keep the cap */
    if (g_pad_script_n >= g_pad_script_cap) {
        int cap = g_pad_script_cap ? g_pad_script_cap * 2 : 256;
        struct pad_script_ev *p = (struct pad_script_ev *)
            realloc(g_pad_script, (size_t)cap * sizeof *p);
        if (!p) return 0;
        g_pad_script = p; g_pad_script_cap = cap;
    }
    g_pad_script[g_pad_script_n++] = *ev;
    return 1;
}

/* Whitespace-, comma- and semicolon-separated events; '#' to end of line is a
 * comment, so an inline value and a file are the same grammar. A comment
 * beginning "#!" is a directive and is read rather than skipped. */
static void pad_script_parse(const char *text)
{
    const char *p = text;
    int bad = 0;
    g_pad_script_n = 0;
    while (*p) {
        const char *tok, *colon, *end;
        struct pad_script_ev ev;
        while (*p && (isspace((unsigned char)*p) || *p == ',' || *p == ';')) p++;
        if (*p == '#') {
            const char *c = p;
            while (*p && *p != '\n') p++;
            if (p - c > 2 && c[1] == '!')
                pad_script_directive(c, (size_t)(p - c));
            continue;
        }
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
        if (tok[0] == 'f' || tok[0] == 'F') {
            ev.frame_keyed = 1;
            ev.f0 = strtoul(tok + 1, NULL, 10);
        } else {
            ev.t0 = strtod(tok, NULL);
        }
        {
            const char *hold = memchr(colon + 1, ':', (size_t)(end - colon - 1));
            const char *blast = hold ? hold : end;
            if (ev.frame_keyed) {
                unsigned long h = hold ? strtoul(hold + 1, NULL, 10)
                                       : PAD_SCRIPT_HOLD_FRAMES;
                if (h == 0) h = 1;         /* an event must cover a frame */
                ev.f1 = ev.f0 + h;
            } else {
                double h = hold ? strtod(hold + 1, NULL) : PAD_SCRIPT_HOLD;
                if (h <= 0.0) h = PAD_SCRIPT_HOLD;
                ev.t1 = ev.t0 + h;
            }
            if (!pad_script_buttons(colon + 1, (size_t)(blast - colon - 1), &ev.s)) {
                bad++;
                continue;
            }
        }
        if (!pad_script_push(&ev))
            bad++;
    }
    fprintf(stderr, "  [PAD-SCRIPT] %d event(s) loaded%s; t=0 is input init,"
            " f=0 is the guest's first frame\n",
            g_pad_script_n, bad ? " (some rejected -- see above)" : "");
    fflush(stderr);
}

/* ── the replay side: runs, and the checkpoints that police them ─────────*/

static struct pad_run *g_pad_runs;
static int g_pad_runs_n, g_pad_runs_cap;
static int g_pad_run_cursor;        /* run containing the current frame */
static int g_pad_run_folded;        /* runs folded into the hash so far */
static int g_pad_ck_next;
static int g_pad_ck_ok, g_pad_ck_bad;
static int g_pad_anchor_bad;

/* ── THE HALF A FRAME KEY DOES NOT FIX ───────────────────────────────────
 *
 * Frame 0 is the guest's first flip, which is a well-defined moment, and
 * from there a frame-keyed replay is immune to how fast the host is. It is
 * NOT immune to the title spending a different NUMBER of frames getting
 * somewhere: the boot path does file I/O and texture loads that are not
 * frame-locked, so if a disc read is slower today the logos can occupy more
 * frames and frame 4000 is a different moment than it was. The existing
 * wall-clock schedule brute-forces the menu with eight START/START/A groups
 * for exactly this reason -- it cannot tell which state the title is in.
 *
 * A frame key fixes the timing half of that and not the state half. So the
 * recording also carries an ANCHOR: one word of guest-visible state, sampled
 * by a callback the host installs, written beside each checkpoint and
 * compared on replay. It cannot correct a misalignment; it makes one VISIBLE,
 * which is the difference between a replay you can trust and a replay that
 * quietly drifted into a different scene.
 *
 * The contract with the caller is deliberately thin, so the input layer
 * carries no knowledge of any particular title: the low 16 bits are a SLOW
 * counter and are compared with a tolerance; everything above them is
 * identity and is compared exactly. In this tree main.c fills it with JSRF's
 * own CSaveData -- chapter, mission and playtime in seconds -- which is the
 * one thing that tells a run parked in the attract loop apart from a run
 * that reached gameplay.
 */
#define PAD_ANCHOR_SLACK 2u

static unsigned long (*g_pad_anchor_fn)(void);

void xbox_PadRecordSetAnchorFn(unsigned long (*fn)(void))
{
    g_pad_anchor_fn = fn;
}

static unsigned long pad_anchor_now(void)
{
    return g_pad_anchor_fn ? g_pad_anchor_fn() : 0ul;
}

static int pad_anchor_agrees(unsigned long a, unsigned long b)
{
    unsigned long la = a & 0xFFFFul, lb = b & 0xFFFFul;
    if ((a >> 16) != (b >> 16)) return 0;
    return (la > lb ? la - lb : lb - la) <= PAD_ANCHOR_SLACK;
}
static unsigned long long g_pad_replay_hash = PAD_FNV_OFFSET;
static unsigned long g_pad_replay_frame;
static int g_pad_replay_past_end;
static pthread_mutex_t g_pad_rec_lock = PTHREAD_MUTEX_INITIALIZER;

static void pad_run_push(unsigned long f0, unsigned long f1, const struct pad_state *s)
{
    if (f1 <= f0) return;
    if (g_pad_runs_n >= g_pad_runs_cap) {
        int cap = g_pad_runs_cap ? g_pad_runs_cap * 2 : 512;
        struct pad_run *p = (struct pad_run *)realloc(g_pad_runs, (size_t)cap * sizeof *p);
        if (!p) return;
        g_pad_runs = p; g_pad_runs_cap = cap;
    }
    g_pad_runs[g_pad_runs_n].f0 = f0;
    g_pad_runs[g_pad_runs_n].f1 = f1;
    g_pad_runs[g_pad_runs_n].s  = *s;
    g_pad_runs_n++;
}

/* The canonical decomposition: every frame from 0 to the end belongs to
 * exactly one run, neutral where no event covers it. The recorder builds the
 * same sequence as it goes, which is what lets the two hashes be compared. */
static void pad_runs_build(void)
{
    static const struct pad_state neutral;
    unsigned long pos = 0, end;
    int i, overlap = 0;

    for (i = 0; i < g_pad_script_n; i++) {
        struct pad_script_ev *e = &g_pad_script[i];
        if (!e->frame_keyed) continue;
        if (e->f0 < pos) { overlap++; continue; }
        if (e->f0 > pos) pad_run_push(pos, e->f0, &neutral);
        pad_run_push(e->f0, e->f1, &e->s);
        pos = e->f1;
    }
    end = g_padrec_frames > pos ? g_padrec_frames : pos;
    if (end > pos) pad_run_push(pos, end, &neutral);
    if (overlap)
        fprintf(stderr, "  [PAD-REPLAY] %d event(s) overlapped an earlier one and"
                " were DROPPED -- a recording never overlaps, so this file has"
                " been edited or is not a recording\n", overlap);
}

static void pad_replay_check_ck(unsigned long run, unsigned long frame,
                                unsigned long long h)
{
    unsigned long anchor;
    while (g_pad_ck_next < g_padrec_ck_n && g_padrec_ck[g_pad_ck_next].run < run) {
        /* A checkpoint the replay walked straight past: its run ordinal never
         * came up, so the two run decompositions are already different. */
        g_pad_ck_bad++;
        fprintf(stderr, "  [PAD-REPLAY] DIVERGED: the recording has a checkpoint"
                " at run %lu and this replay went from a lower run straight to"
                " %lu\n", g_padrec_ck[g_pad_ck_next].run, run);
        g_pad_ck_next++;
    }
    if (g_pad_ck_next >= g_padrec_ck_n) return;
    if (g_padrec_ck[g_pad_ck_next].run != run) return;
    anchor = pad_anchor_now();
    if (!pad_anchor_agrees(anchor, g_padrec_ck[g_pad_ck_next].anchor)) {
        /* SEPARATE FROM THE HASH, because it means something different. A
         * hash mismatch says the input being fed in is not the input that
         * was recorded. This says the input IS right and the GAME is
         * somewhere else -- which is what happens when the boot path takes a
         * different number of frames, and it is the failure a frame key
         * cannot prevent. Not fatal; loud. */
        g_pad_anchor_bad++;
        if (g_pad_anchor_bad <= 8)
            fprintf(stderr, "  [PAD-REPLAY] STATE MISALIGNED at frame %lu:"
                    " the recording was at guest state %08lX here, this run is"
                    " at %08lX. The input is being replayed correctly but the"
                    " title is not in the same place, so frame %lu is not the"
                    " same moment it was.\n",
                    frame, g_padrec_ck[g_pad_ck_next].anchor, anchor, frame);
    }
    if (g_padrec_ck[g_pad_ck_next].hash == h &&
        g_padrec_ck[g_pad_ck_next].frame == frame) {
        g_pad_ck_ok++;
    } else {
        g_pad_ck_bad++;
        fprintf(stderr, "  [PAD-REPLAY] DIVERGED at run %lu: recording says"
                " frame %lu hash %016llx, this replay has frame %lu hash %016llx."
                " The input being fed to the guest is NOT the input that was"
                " recorded; stop trusting this run.\n",
                run, g_padrec_ck[g_pad_ck_next].frame,
                (unsigned long long)g_padrec_ck[g_pad_ck_next].hash, frame, h);
        fflush(stderr);
    }
    g_pad_ck_next++;
}

/* Replace the whole pad state from the run covering this frame. */
static void pad_replay_apply(XBOX_INPUT_STATE *st, unsigned long f)
{
    pthread_mutex_lock(&g_pad_rec_lock);
    if (f < g_pad_replay_frame) {
        /* The counter cannot go backwards; if it has, something reset it and
         * every frame key below is meaningless. */
        pthread_mutex_unlock(&g_pad_rec_lock);
        return;
    }
    g_pad_replay_frame = f;
    /* A mark is announced once, when the replay reaches its frame, so the
     * log of every replay of this session carries the player's "here" at
     * the same guest frame the player pressed it. Marks are kept in file
     * order, which is frame order for a recording. */
    while (g_pad_mark_echoed < g_pad_marks_n
           && g_pad_marks[g_pad_mark_echoed].frame <= f) {
        const struct pad_mark *m = &g_pad_marks[g_pad_mark_echoed++];
        fprintf(stderr, "  [PAD-MARK] replay reached mark #%d: frame=%lu"
                " \"%s\" (now t=%.1fs)\n", g_pad_mark_echoed, m->frame,
                m->label, xbox_InputSeconds());
        fflush(stderr);
    }
    while (g_pad_run_cursor < g_pad_runs_n && g_pad_runs[g_pad_run_cursor].f1 <= f)
        g_pad_run_cursor++;
    while (g_pad_run_folded < g_pad_run_cursor) {
        const struct pad_run *r = &g_pad_runs[g_pad_run_folded];
        g_pad_replay_hash = pad_fold_run(g_pad_replay_hash, &r->s, r->f1 - r->f0);
        g_pad_run_folded++;
        pad_replay_check_ck((unsigned long)g_pad_run_folded, r->f1, g_pad_replay_hash);
    }
    if (g_pad_run_cursor < g_pad_runs_n) {
        const struct pad_run *r = &g_pad_runs[g_pad_run_cursor];
        /* REPLACE, not merge: a replay a resting stick can alter is not a
         * replay. Frames before the first run are neutral for the same
         * reason -- the recording says nothing was pressed there. */
        memset(&st->Gamepad, 0, sizeof st->Gamepad);
        if (f >= r->f0) pad_state_to_report(&r->s, st);
    } else if (!g_pad_replay_past_end) {
        /* PAST THE END: HAND THE PAD BACK, rather than pinning it neutral.
         * The recording has nothing left to say, and a controller that stops
         * working when a replay runs out is the kind of thing that gets
         * diagnosed as a dead pad -- this tree has already spent a session
         * on exactly that. The caller's own read stands from here. */
        g_pad_replay_past_end = 1;
        fprintf(stderr, "  [PAD-REPLAY] frame %lu is past the end of the"
                " recording (%d run(s)); the replay is over and the pad is"
                " whatever the controller says from here.\n",
                f, g_pad_runs_n);
        fflush(stderr);
    }
    pthread_mutex_unlock(&g_pad_rec_lock);
}

/* ── loading, and the refusal ────────────────────────────────────────────*/

/* Cached at file scope rather than inside the function, so that
 * xbox_PadScriptReset() can clear it: a test that loads one file twice with
 * the switch set differently has to be able to, and a function-local static
 * cannot be reached. Read once per load, for the reason every switch here is
 * read once -- getenv takes a lock and walks the environment linearly. */
static int g_pad_replay_force = -1;

static int pad_replay_force(void)
{
    if (g_pad_replay_force < 0)
        g_pad_replay_force = recomp_switch_on("RECOMP_PAD_REPLAY_FORCE");
    return g_pad_replay_force;
}

static void pad_script_reset_state(void)
{
    free(g_pad_script);  g_pad_script = NULL;
    free(g_pad_runs);    g_pad_runs = NULL;
    free(g_padrec_ck);   g_padrec_ck = NULL;
    g_pad_script_n = -1; g_pad_script_cap = 0;
    g_pad_runs_n = g_pad_runs_cap = 0;
    g_padrec_ck_n = g_padrec_ck_cap = 0;
    g_pad_run_cursor = g_pad_run_folded = 0;
    g_pad_ck_next = g_pad_ck_ok = g_pad_ck_bad = g_pad_anchor_bad = 0;
    g_pad_replay_hash = PAD_FNV_OFFSET;
    g_pad_replay_frame = 0;
    g_pad_replay_past_end = 0;
    g_padrec_seen = g_padrec_version = g_padrec_refused = 0;
    g_padrec_frames = 0; g_padrec_switches = 0;
    g_pad_marks_n = g_pad_mark_echoed = 0;
    g_padrec_build[0] = g_padrec_gen[0] = 0;
    g_pad_replay_force = -1;
}

void xbox_PadScriptReset(void) { pad_script_reset_state(); }

/* Refuse a recording taken against other code.
 *
 * "It replayed and the crash did not reproduce" is a conclusion somebody will
 * draw, and it is worthless if the binary underneath changed. So the two
 * fields that decide what the code IS are fatal, and the switch set -- which
 * differs between the harness and the bundle in 29 places on any given day --
 * is loud but not fatal. */
static int pad_header_check(void)
{
    int bad = 0;
    if (!g_padrec_seen) return 1;
    if (g_padrec_version != PAD_REC_VERSION) {
        fprintf(stderr, "  [PAD-REPLAY] REFUSED: recording format version %d,"
                " this binary speaks %d\n", g_padrec_version, PAD_REC_VERSION);
        bad++;
    }
    if (strcmp(g_padrec_build, g_pad_build_id)) {
        fprintf(stderr, "  [PAD-REPLAY] REFUSED: recorded against build '%s',"
                " this binary is '%s'\n", g_padrec_build, g_pad_build_id);
        bad++;
    }
    if (strcmp(g_padrec_gen, g_pad_gen_id)) {
        fprintf(stderr, "  [PAD-REPLAY] REFUSED: recorded against generated tree"
                " '%s', this binary carries '%s'\n", g_padrec_gen, g_pad_gen_id);
        bad++;
    }
    if (g_padrec_switches != pad_switch_hash()) {
        fprintf(stderr, "  [PAD-REPLAY] the RECOMP_* switch set differs from the"
                " recording's (%016llx recorded, %016llx now). Not refused --"
                " the harness and the bundle differ in ~29 switches routinely"
                " -- but the guest may well behave differently.\n",
                (unsigned long long)g_padrec_switches,
                (unsigned long long)pad_switch_hash());
    }
    if (!bad) return 1;
    if (pad_replay_force()) {
        fprintf(stderr, "  [PAD-REPLAY] RECOMP_PAD_REPLAY_FORCE is set, so the"
                " refusal above is being ignored. Whatever this run shows is"
                " not evidence about the recorded session.\n");
        fflush(stderr);
        return 1;
    }
    fprintf(stderr, "  [PAD-REPLAY] no input will be replayed. Rebuild against"
            " the recording's build/gen, or set RECOMP_PAD_REPLAY_FORCE=1 to"
            " replay anyway and accept that the result means nothing.\n");
    fflush(stderr);
    g_padrec_refused = 1;
    return 0;
}

/* "@path" or inline text. Returns the event count, or -1 if a recording was
 * refused. */
int xbox_PadScriptLoad(const char *spec)
{
    pad_script_reset_state();
    g_pad_script_n = 0;
    if (spec && *spec == '@') {
        FILE *f = fopen(spec + 1, "rb");
        if (!f) {
            fprintf(stderr, "  [PAD-SCRIPT] cannot open %s: %s\n",
                    spec + 1, strerror(errno));
            fflush(stderr);
            return 0;
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
    } else if (spec && *spec) {
        pad_script_parse(spec);
    }
    if (g_padrec_seen) {
        if (!pad_header_check()) {
            g_pad_script_n = 0;
            return -1;
        }
        pad_runs_build();
        fprintf(stderr, "  [PAD-REPLAY] %d event(s) -> %d run(s) over %lu frame(s),"
                " %d checkpoint(s)%s\n",
                g_pad_script_n, g_pad_runs_n,
                g_pad_runs_n ? g_pad_runs[g_pad_runs_n - 1].f1 : 0ul,
                g_padrec_ck_n,
                g_padrec_frames ? "" : "; NO END MARKER (the recorded run was"
                                       " killed or crashed, which is normal for"
                                       " a crash recording)");
        fflush(stderr);
    }
    return g_pad_script_n;
}

static int pad_script_on(void)
{
    if (g_pad_script_n < 0) {
        const char *v = getenv("RECOMP_PAD_SCRIPT");
        g_pad_script_n = 0;
        if (v && *v) xbox_PadScriptLoad(v);
    }
    return g_pad_script_n > 0;
}

/* OR the schedule into whatever the caller has already filled in -- or, for a
 * recording, replace it outright. A scripted stick deflection yields to a
 * live one, so taking over a run mid-flight does not fight the script for the
 * camera; a recording does not yield, because a replay that a resting stick
 * can alter is not a replay. */
void xbox_PadScriptApplyAtFrame(XBOX_INPUT_STATE *st, unsigned long frame)
{
    double t;
    int i, j;

    if (g_pad_script_n <= 0) return;
    if (g_padrec_seen) { pad_replay_apply(st, frame); return; }

    t = fake_pad_seconds();
    for (i = 0; i < g_pad_script_n; i++) {
        struct pad_script_ev *ev = &g_pad_script[i];
        if (ev->frame_keyed) {
            if (frame < ev->f0 || frame >= ev->f1) continue;
        } else {
            if (t < ev->t0 || t >= ev->t1) continue;
        }
        if (!ev->fired) {
            ev->fired = 1;
            fprintf(stderr, "  [PAD-SCRIPT] t=%7.2f f=%lu fire #%d buttons=%04X"
                    " a=%u b=%u lx=%d ly=%d poll=%lu\n",
                    t, frame, i, (unsigned)ev->s.buttons,
                    (unsigned)ev->s.analog[XBOX_BUTTON_A],
                    (unsigned)ev->s.analog[XBOX_BUTTON_B],
                    (int)ev->s.lx, (int)ev->s.ly, g_pad_polls);
            fflush(stderr);
        }
        st->Gamepad.wButtons |= ev->s.buttons;
        for (j = 0; j < 8; j++)
            if (ev->s.analog[j] > st->Gamepad.bAnalogButtons[j])
                st->Gamepad.bAnalogButtons[j] = ev->s.analog[j];
        if (ev->s.lx && st->Gamepad.sThumbLX > -4096 && st->Gamepad.sThumbLX < 4096)
            st->Gamepad.sThumbLX = ev->s.lx;
        if (ev->s.ly && st->Gamepad.sThumbLY > -4096 && st->Gamepad.sThumbLY < 4096)
            st->Gamepad.sThumbLY = ev->s.ly;
        if (ev->s.rx && st->Gamepad.sThumbRX > -4096 && st->Gamepad.sThumbRX < 4096)
            st->Gamepad.sThumbRX = ev->s.rx;
        if (ev->s.ry && st->Gamepad.sThumbRY > -4096 && st->Gamepad.sThumbRY < 4096)
            st->Gamepad.sThumbRY = ev->s.ry;
    }
}

static void pad_script_apply(XBOX_INPUT_STATE *st)
{
    if (!pad_script_on()) return;
    xbox_PadScriptApplyAtFrame(st, xbox_InputFrame());
}

unsigned long long xbox_PadReplayHash(void) { return g_pad_replay_hash; }

int xbox_PadReplayStatus(int *ck_ok, int *ck_bad, unsigned long *frame,
                         int *state_bad)
{
    if (ck_ok)  *ck_ok  = g_pad_ck_ok;
    if (ck_bad) *ck_bad = g_pad_ck_bad;
    if (frame)  *frame  = g_pad_replay_frame;
    if (state_bad) *state_bad = g_pad_anchor_bad;
    return g_padrec_seen && !g_padrec_refused;
}

/* ── the recorder ────────────────────────────────────────────────────────*/

static FILE *g_pad_rec_f;
static char  g_pad_rec_path[1024];
static struct pad_state g_pad_rec_cur;      /* the state of the open run */
static struct pad_state g_pad_rec_pending;  /* the latest sample, not yet final */
static unsigned long g_pad_rec_open;        /* frame the open run started at */
static unsigned long g_pad_rec_pending_f;
static unsigned long g_pad_rec_runs, g_pad_rec_events;
static unsigned long long g_pad_rec_hash = PAD_FNV_OFFSET;
static unsigned long long g_pad_rec_bytes;

static void pad_mkdir_p(const char *path)
{
    char tmp[1024];
    size_t i, n = snprintf(tmp, sizeof tmp, "%s", path);
    if (n >= sizeof tmp) return;
    for (i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        mkdir(tmp, 0755);
        tmp[i] = '/';
    }
    mkdir(tmp, 0755);
}

/* Where the recording goes.
 *
 * NOT THE WORKING DIRECTORY, and this is the whole reason the switch exists
 * in the shape it does. A GUI-launched .app has no writable working
 * directory: the same mistake cost this tree three days and 91 silent fopen
 * failures with the indirect-branch database, and the player's own session --
 * the one worth recording -- is exactly the run that comes from a
 * double-click. So the default is absolute, under the support directory the
 * launcher already creates, and a failed open is loud rather than counted.
 */
static void pad_record_default_path(char *buf, size_t n)
{
    const char *e = getenv("RECOMP_PAD_RECORD_PATH");
    const char *home;
    time_t now;
    struct tm tmv;
    char stamp[32];

    if (e && *e) { snprintf(buf, n, "%s", e); return; }
    home = getenv("HOME");
    now = time(NULL);
    localtime_r(&now, &tmv);
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H%M%S", &tmv);
    if (home && *home)
        snprintf(buf, n, "%s/Library/Application Support/JSRF/padrec/"
                 "jsrf-%s-%d.padrec", home, stamp, (int)getpid());
    else
        snprintf(buf, n, "jsrf-%s-%d.padrec", stamp, (int)getpid());
}

int xbox_PadRecordOpen(const char *path)
{
    static const struct pad_state neutral;
    char dir[1024];
    char *slash;

    if (g_pad_rec_f) return 1;
    snprintf(g_pad_rec_path, sizeof g_pad_rec_path, "%s", path);
    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (slash) { *slash = 0; pad_mkdir_p(dir); }

    g_pad_rec_f = fopen(g_pad_rec_path, "w");
    if (!g_pad_rec_f) {
        fprintf(stderr,
                "  [PAD-RECORD] CANNOT WRITE %s: %s\n"
                "  [PAD-RECORD] NOTHING FROM THIS SESSION WILL BE RECORDED. A"
                " GUI-launched app bundle has no writable working directory,"
                " which is the usual cause; set RECOMP_PAD_RECORD_PATH to an"
                " absolute path under \"$HOME/Library/Application Support/JSRF\".\n",
                g_pad_rec_path, strerror(errno));
        fflush(stderr);
        return 0;
    }
    /* 64 KB and no line buffering: the cost claim for this instrument is that
     * it does not perturb the frame rate, and a write(2) per sample would not
     * be that. Durability comes from xbox_PadRecordFlush, which the crash
     * handler and the SIGTERM path both call. */
    setvbuf(g_pad_rec_f, NULL, _IOFBF, 65536);

    g_pad_rec_cur = neutral;
    g_pad_rec_pending = neutral;
    g_pad_rec_open = 0;
    g_pad_rec_pending_f = 0;
    g_pad_rec_runs = g_pad_rec_events = 0;
    g_pad_rec_hash = PAD_FNV_OFFSET;
    g_pad_rec_bytes = 0;

    g_pad_rec_bytes += (unsigned long long)fprintf(g_pad_rec_f,
        "# JSRF pad recording -- one pad state per frame, keyed to the guest's\n"
        "# own FLIP_STALL count, not to the clock. Replay it with\n"
        "#     RECOMP_PAD_SCRIPT=@%s\n"
        "# against a binary with the same build and gen as the header below.\n"
        "#!padrec %d\n"
        "#!build %s\n"
        "#!gen %s\n"
        "#!switches %016llx\n",
        g_pad_rec_path, PAD_REC_VERSION, g_pad_build_id, g_pad_gen_id,
        (unsigned long long)pad_switch_hash());
    fprintf(stderr, "  [PAD-RECORD] recording to %s\n", g_pad_rec_path);
    fflush(stderr);
    return 1;
}

/* Append to a fixed buffer, saturating rather than running off the end.
 * `*n` is left at or past `cap` when the text did not fit, which the caller
 * checks once instead of after every field. */
static void pad_append(char *buf, size_t cap, int *n, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (*n < 0 || (size_t)*n >= cap) { *n = (int)cap; return; }
    va_start(ap, fmt);
    r = vsnprintf(buf + *n, cap - (size_t)*n, fmt, ap);
    va_end(ap);
    if (r < 0) { *n = (int)cap; return; }
    *n += r;
}

/* Write one run out, and fold it into the checkpoint hash. A neutral run
 * needs no line: in a recording an uncovered frame IS neutral, which keeps an
 * idle minute at zero bytes rather than 3,600 lines of nothing. */
static void pad_rec_close_run(unsigned long f0, unsigned long f1,
                              const struct pad_state *s)
{
    if (f1 <= f0) return;
    g_pad_rec_runs++;
    g_pad_rec_hash = pad_fold_run(g_pad_rec_hash, s, f1 - f0);

    if (!pad_state_neutral(s) && g_pad_rec_f) {
        char line[512];
        int n = 0, k, first = 1;
        WORD left = s->buttons;
        pad_append(line, sizeof line, &n, "f%lu:", f0);
        for (k = 0; k < (int)(sizeof pad_script_bits / sizeof pad_script_bits[0]); k++)
            if (s->buttons & pad_script_bits[k].bit) {
                pad_append(line, sizeof line, &n, "%s%s",
                           first ? "" : "+", pad_script_bits[k].name);
                first = 0;
                left = (WORD)(left & ~pad_script_bits[k].bit);
            }
        if (left) {
            pad_append(line, sizeof line, &n, "%sBTN=0x%04X",
                       first ? "" : "+", (unsigned)left);
            first = 0;
        }
        /* BY TABLE INDEX, NOT BY BYTE INDEX. bAnalogButtons is in report
         * order (A B X Y Black White LT RT) and the name table is in
         * keyboard order (A B X Y White Black LT RT), so the two disagree at
         * slots 4 and 5 -- a recording that walked the bytes would write
         * every Black press as WHITE. */
        for (k = 0; k < (int)(sizeof pad_script_analog / sizeof pad_script_analog[0]); k++) {
            int idx = pad_script_analog[k].idx;
            if (s->analog[idx]) {
                pad_append(line, sizeof line, &n, "%s%s=%u",
                           first ? "" : "+", pad_script_analog[k].name,
                           (unsigned)s->analog[idx]);
                first = 0;
            }
        }
        {
            const SHORT ax[4] = { s->lx, s->ly, s->rx, s->ry };
            for (k = 0; k < (int)(sizeof pad_script_axis / sizeof pad_script_axis[0]); k++)
                if (ax[pad_script_axis[k].axis]) {
                    pad_append(line, sizeof line, &n, "%s%s=%d",
                               first ? "" : "+", pad_script_axis[k].name,
                               (int)ax[pad_script_axis[k].axis]);
                    first = 0;
                }
        }
        pad_append(line, sizeof line, &n, ":%lu\n", f1 - f0);
        if (n > 0 && n < (int)sizeof line) {
            fputs(line, g_pad_rec_f);
            g_pad_rec_bytes += (unsigned long long)n;
            g_pad_rec_events++;
        } else {
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "  [PAD-RECORD] a pad state did not fit one"
                        " line and was DROPPED; the replay of this file will"
                        " diverge at run %lu\n", g_pad_rec_runs);
            }
        }
    }
    if (g_pad_rec_f && (g_pad_rec_runs % PAD_REC_CK_RUNS) == 0)
        g_pad_rec_bytes += (unsigned long long)fprintf(g_pad_rec_f,
            "#!ck %lu %lu %016llx %08lx\n", g_pad_rec_runs, f1,
            (unsigned long long)g_pad_rec_hash, pad_anchor_now());
    if (g_pad_rec_f && (g_pad_rec_runs % PAD_REC_FLUSH_RUNS) == 0)
        fflush(g_pad_rec_f);
}

/* The sample point. Called once per pad poll, which is the guest's USB
 * schedule and not the frame loop -- measured at 118-124 Hz over four 5 s
 * reports while the frame rate moved between 24 and 202 fps -- so a frame can
 * hold several samples and a fast frame can hold none. Both are handled the
 * same way: THE LAST SAMPLE IN A FRAME IS THAT FRAME'S STATE, and a frame
 * with no sample inherits the previous one, which is exactly what the replay
 * does with an uncovered frame.
 *
 * The consequence worth stating: a press and release inside one frame cannot
 * be recorded, because a frame-keyed format has one state per frame by
 * construction. At 60 Hz that is a 16 ms press, which a human cannot make. */
void xbox_PadRecordSampleAtFrame(const XBOX_INPUT_STATE *st, unsigned long f)
{
    struct pad_state s;
    if (!g_pad_rec_f) return;
    pad_state_from_report(&s, st);

    pthread_mutex_lock(&g_pad_rec_lock);
    if (f == g_pad_rec_pending_f) {
        g_pad_rec_pending = s;
    } else if (f > g_pad_rec_pending_f) {
        if (!pad_state_eq(&g_pad_rec_pending, &g_pad_rec_cur)) {
            pad_rec_close_run(g_pad_rec_open, g_pad_rec_pending_f, &g_pad_rec_cur);
            g_pad_rec_cur  = g_pad_rec_pending;
            g_pad_rec_open = g_pad_rec_pending_f;
        }
        g_pad_rec_pending   = s;
        g_pad_rec_pending_f = f;
    }
    pthread_mutex_unlock(&g_pad_rec_lock);
}

static void pad_rec_finish_locked(void)
{
    if (!pad_state_eq(&g_pad_rec_pending, &g_pad_rec_cur)) {
        pad_rec_close_run(g_pad_rec_open, g_pad_rec_pending_f, &g_pad_rec_cur);
        g_pad_rec_cur  = g_pad_rec_pending;
        g_pad_rec_open = g_pad_rec_pending_f;
    }
    pad_rec_close_run(g_pad_rec_open, g_pad_rec_pending_f + 1, &g_pad_rec_cur);
    g_pad_rec_open = g_pad_rec_pending_f + 1;
}

/* Called from the crash handler and from the SIGTERM path, so a blocking lock
 * is wrong: the signal may have landed on the very thread that holds it.
 * Try, give up, and still flush what stdio has -- a recording missing its
 * last run is far better than a deadlocked crash handler. */
void xbox_PadRecordFlush(void)
{
    int got = 0, tries;
    if (!g_pad_rec_f) return;
    for (tries = 0; tries < 200 && !got; tries++)
        got = pthread_mutex_trylock(&g_pad_rec_lock) == 0;
    if (got) {
        pad_rec_finish_locked();
        pthread_mutex_unlock(&g_pad_rec_lock);
    } else {
        fprintf(stderr, "  [PAD-RECORD] could not take the record lock; the"
                " recording is flushed but its last run is missing\n");
    }
    fflush(g_pad_rec_f);
}

void xbox_PadRecordClose(void)
{
    if (!g_pad_rec_f) return;
    pthread_mutex_lock(&g_pad_rec_lock);
    pad_rec_finish_locked();
    fprintf(g_pad_rec_f, "#!ck %lu %lu %016llx %08lx\n", g_pad_rec_runs,
            g_pad_rec_open, (unsigned long long)g_pad_rec_hash, pad_anchor_now());
    fprintf(g_pad_rec_f, "#!frames %lu\n", g_pad_rec_open);
    fclose(g_pad_rec_f);
    g_pad_rec_f = NULL;
    pthread_mutex_unlock(&g_pad_rec_lock);
    fprintf(stderr, "  [PAD-RECORD] %s: %lu frame(s), %lu run(s), %lu event(s),"
            " %llu byte(s), hash %016llx\n",
            g_pad_rec_path, g_pad_rec_open, g_pad_rec_runs, g_pad_rec_events,
            (unsigned long long)g_pad_rec_bytes,
            (unsigned long long)g_pad_rec_hash);
    fflush(stderr);
}

unsigned long long xbox_PadRecordHash(void) { return g_pad_rec_hash; }

/* Mark this moment. The frame is the guest's own (xbox_InputFrameAdvance),
 * which is the key everything else in a recording uses, so a replay reaches
 * the mark at the same guest frame however fast the host runs. Written to
 * the open recording as a directive, and flushed at once: a mark is the one
 * line a person is waiting on, and a session that crashes ten seconds later
 * must still have it. Not a pad event, so the checkpoint hash is unchanged
 * and a marked recording replays identically to an unmarked one. */
static void (*g_pad_mark_hook)(unsigned long, const char *);
void xbox_PadRecordSetMarkHook(void (*fn)(unsigned long, const char *))
{ g_pad_mark_hook = fn; }

void xbox_PadRecordMark(const char *label)
{
    unsigned long f = g_pad_guest_frame;
    const char *lab = (label && *label) ? label : "mark";
    int k;
    pthread_mutex_lock(&g_pad_rec_lock);
    pad_mark_push(f, lab, strlen(lab));
    k = g_pad_marks_n;
    if (g_pad_rec_f) {
        g_pad_rec_bytes += (unsigned long long)
            fprintf(g_pad_rec_f, "#!mark %lu %s\n", f, lab);
        fflush(g_pad_rec_f);
    }
    pthread_mutex_unlock(&g_pad_rec_lock);
    fprintf(stderr, "  [PAD-MARK] #%d frame=%lu t=%.1fs \"%s\"%s\n", k, f,
            xbox_InputSeconds(), lab,
            g_pad_rec_f ? " (written to the recording)"
                        : " (no recording open: log only)");
    fflush(stderr);
    if (g_pad_mark_hook) g_pad_mark_hook(f, lab);
}

int xbox_PadNearMark(unsigned long frame, unsigned long slack, const char **label)
{
    int i;
    for (i = 0; i < g_pad_marks_n; i++) {
        unsigned long m = g_pad_marks[i].frame;
        unsigned long d = m > frame ? m - frame : frame - m;
        if (d <= slack) {
            if (label) *label = g_pad_marks[i].label;
            return 1;
        }
    }
    if (label) *label = NULL;
    return 0;
}

static int pad_record_on(void)
{
    static int on = -1;
    /* READ ONCE. getenv takes a lock and walks the environment linearly on
     * macOS, and a profile here once put __findenv above the draw call; this
     * is on the pad poll path, which runs at 120 Hz. */
    if (on < 0) on = recomp_switch_on("RECOMP_PAD_RECORD");
    return on;
}

static void pad_record_sample(const XBOX_INPUT_STATE *st)
{
    static int started;
    unsigned long f;

    if (!pad_record_on()) return;
    if (!started) {
        char path[1024];
        started = 1;
        pad_record_default_path(path, sizeof path);
        if (!xbox_PadRecordOpen(path)) return;
    }
    f = xbox_InputFrame();
    /* A recording keyed to a counter nobody advances is 100% of one frame,
     * and it would replay as a single press. Say so rather than producing
     * one: the counter is advanced by whoever consumes the guest's flip, so
     * an unset RECOMP_PB_EXEC is the way this happens. */
    if (f == 0) {
        static unsigned long polls_at_zero;
        if (++polls_at_zero == 600) {
            fprintf(stderr, "  [PAD-RECORD] 600 pad polls and the guest frame"
                    " counter is still 0 -- nothing is calling"
                    " xbox_InputFrameAdvance(), so every sample is landing on"
                    " frame 0 and this recording WILL NOT REPLAY. Is"
                    " RECOMP_PB_EXEC=1 set?\n");
            fflush(stderr);
        }
    }
    xbox_PadRecordSampleAtFrame(st, f);
}

void xbox_PadRecordReport(void)
{
    if (g_pad_rec_f)
        fprintf(stderr, "  [PAD-RECORD] frame=%lu runs=%lu events=%lu bytes=%llu"
                " hash=%016llx file=%s\n",
                g_pad_rec_pending_f, g_pad_rec_runs, g_pad_rec_events,
                (unsigned long long)g_pad_rec_bytes,
                (unsigned long long)g_pad_rec_hash, g_pad_rec_path);
    if (g_padrec_seen && !g_padrec_refused)
        fprintf(stderr, "  [PAD-REPLAY] frame=%lu/%lu run=%d/%d checkpoints"
                " ok=%d BAD=%d unreached=%d state=%s %s\n",
                g_pad_replay_frame, g_padrec_frames, g_pad_run_cursor,
                g_pad_runs_n, g_pad_ck_ok, g_pad_ck_bad,
                g_padrec_ck_n - g_pad_ck_next,
                g_pad_anchor_bad ? "MISALIGNED" : "aligned",
                g_pad_ck_bad ? "<<< DIVERGED"
                : g_pad_replay_past_end ? "(past the end; pad is neutral)"
                : "in sync");
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
    /* THE SAMPLE POINT, and it is here for the same reason pad_trace is:
     * every consumer -- the emulated USB gamepad and the report shim alike --
     * comes through this one function, and what it sees is the state the
     * guest is about to be given rather than SDL's raw view. Recording
     * anywhere else would record something the title never read. */
    pad_record_sample(st);
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
