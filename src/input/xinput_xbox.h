/**
 * Xbox Input Compatibility Layer - Type Definitions
 *
 * Defines the Xbox XPP (Xbox Peripheral Port) input types and
 * translates them to Windows XInput. The Xbox controller API
 * differs from XInput in structure layout and polling model.
 *
 * The game uses 9 XPP entry points with 12 total calls:
 * - Controller state polling
 * - Vibration/force feedback
 * - Device enumeration
 */

#ifndef BURNOUT3_XINPUT_XBOX_H
#define BURNOUT3_XINPUT_XBOX_H

#include <stdint.h>
#include "platform/xbox_winnt.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Xbox input types
 * ================================================================ */

/* Xbox controller port count */
#define XBOX_MAX_CONTROLLERS 4

/* Xbox gamepad button flags */
#define XBOX_GAMEPAD_DPAD_UP        0x0001
#define XBOX_GAMEPAD_DPAD_DOWN      0x0002
#define XBOX_GAMEPAD_DPAD_LEFT      0x0004
#define XBOX_GAMEPAD_DPAD_RIGHT     0x0008
#define XBOX_GAMEPAD_START          0x0010
#define XBOX_GAMEPAD_BACK           0x0020
#define XBOX_GAMEPAD_LEFT_THUMB     0x0040
#define XBOX_GAMEPAD_RIGHT_THUMB    0x0080

/* Xbox analog button thresholds */
#define XBOX_ANALOG_BUTTON_THRESHOLD 30

typedef struct XBOX_GAMEPAD {
    WORD  wButtons;             /* Digital button bitmask */
    BYTE  bAnalogButtons[8];    /* A, B, X, Y, Black, White, LTrig, RTrig (0-255) */
    SHORT sThumbLX;             /* Left stick X (-32768 to 32767) */
    SHORT sThumbLY;             /* Left stick Y */
    SHORT sThumbRX;             /* Right stick X */
    SHORT sThumbRY;             /* Right stick Y */
} XBOX_GAMEPAD;

typedef struct XBOX_INPUT_STATE {
    DWORD dwPacketNumber;
    XBOX_GAMEPAD Gamepad;
} XBOX_INPUT_STATE;

typedef struct XBOX_VIBRATION {
    WORD wLeftMotorSpeed;       /* Low-frequency rumble (0-65535) */
    WORD wRightMotorSpeed;      /* High-frequency rumble (0-65535) */
} XBOX_VIBRATION;

typedef struct XBOX_INPUT_CAPABILITIES {
    BYTE Type;
    BYTE SubType;
    WORD Flags;
    XBOX_GAMEPAD Gamepad;
    XBOX_VIBRATION Vibration;
} XBOX_INPUT_CAPABILITIES;

/* Analog button indices */
#define XBOX_BUTTON_A           0
#define XBOX_BUTTON_B           1
#define XBOX_BUTTON_X           2
#define XBOX_BUTTON_Y           3
#define XBOX_BUTTON_BLACK       4
#define XBOX_BUTTON_WHITE       5
#define XBOX_BUTTON_LTRIGGER    6
#define XBOX_BUTTON_RTRIGGER    7

/* One clock for every input-side log line.
 *
 * RECOMP_PAD_SCRIPT's schedule, the pad trace and anything correlating the
 * title's state against a press all have to agree about what t=2.0 means, or
 * a schedule cannot be tuned from the log it produced. Zero is set at
 * xbox_InputInit. */
double xbox_InputSeconds(void);

/* ── THE GUEST'S FRAME, AND THE RECORD/REPLAY BUILT ON IT ────────────────
 *
 * The wall clock above is the wrong key for input. A schedule written in
 * seconds is a different schedule on every run, because the title's boot
 * time is not constant -- 19 Sep 2026 measured the same .pad file reaching
 * gameplay at 64-86 fps and never leaving the title screen above ~117 fps.
 * The title is frame-locked, so the frame is the key that holds still.
 *
 * xbox_InputFrameAdvance() is called by whoever consumes the guest's own
 * end-of-frame signal (FLIP_STALL in its push buffer), once per guest frame,
 * and by nothing else. Everything below counts in those frames.
 */
void          xbox_InputFrameAdvance(void);
unsigned long xbox_InputFrame(void);

#if !defined(_WIN32)
/* Stamp the binary's identity into a recording's header, so a replay can
 * refuse a recording taken against other code. Both strings are copied. */
void xbox_PadRecordSetIdentity(const char *build, const char *gen);

/* Finish the open run and push stdio's buffer out. Safe from a crash handler
 * or a signal handler: it will not block on the record lock. */
void xbox_PadRecordFlush(void);

/* Counters for the periodic report; prints nothing when neither recording
 * nor replaying. */
void xbox_PadRecordReport(void);

/* ── the format, exposed so a test can drive it without a game ───────────
 *
 * These are the same parser, the same applier and the same writer the
 * runtime uses; there is no second implementation for tests to agree with.
 * The *AtFrame forms take the frame explicitly, which is what makes a
 * record -> replay round trip a pure function of (frame, state) pairs.
 */
int  xbox_PadScriptLoad(const char *spec);      /* "@path" or inline text.
                                                 * Events loaded, or -1 if a
                                                 * recording was refused. */
void xbox_PadScriptApplyAtFrame(XBOX_INPUT_STATE *st, unsigned long frame);
void xbox_PadScriptReset(void);
int  xbox_PadReplayStatus(int *ck_ok, int *ck_bad, unsigned long *frame,
                          int *state_bad);

/* One word of guest-visible state, sampled at each checkpoint and compared
 * on replay. A frame key cannot guarantee that frame N is the same MOMENT --
 * the boot path is not frame-locked, so a slower disc read spends more
 * frames on the logos -- and this is what makes that visible instead of
 * silent. Low 16 bits: a slow counter, compared with a small tolerance.
 * Above them: identity, compared exactly. NULL or unset means no check. */
void xbox_PadRecordSetAnchorFn(unsigned long (*fn)(void));
unsigned long long xbox_PadReplayHash(void);

int  xbox_PadRecordOpen(const char *path);
void xbox_PadRecordSampleAtFrame(const XBOX_INPUT_STATE *st, unsigned long frame);
void xbox_PadRecordClose(void);

/* A MARK: "the text was wrong HERE". Stamps the current guest frame and a
 * label into the open recording as a `#!mark <frame> <label>` directive and
 * onto stderr as [PAD-MARK]; the replay echoes each mark when it reaches
 * that frame, so a symptom the player saw becomes a frame number in the log
 * of every replay of that session. Marks from a loaded recording and marks
 * made live are the same table; xbox_PadNearMark answers whether `frame` is
 * within `slack` frames of any of them, which is what a probe that should
 * only fire around a symptom asks. */
void xbox_PadRecordMark(const char *label);
int  xbox_PadNearMark(unsigned long frame, unsigned long slack, const char **label);
/* Called from inside xbox_PadRecordMark, after the mark is written, with
 * the frame and label -- the harness hangs a picture off it. NULL clears. */
void xbox_PadRecordSetMarkHook(void (*fn)(unsigned long frame, const char *label));
unsigned long long xbox_PadRecordHash(void);
#endif /* !_WIN32 */

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the input system.
 * Maps Xbox controller ports to XInput slots.
 */
void xbox_InputInit(void);

/* Close any opened controllers and drop SDL's gamecontroller subsystem.
 * Called from the SIGTERM/SIGINT handler installed by xbox_InputInit, because
 * nothing else released the HID device when a run was killed -- see the long
 * comment on the definition. Safe to call more than once. */
void xbox_InputReleasePads(void);

/**
 * Get the state of a controller.
 * Port: 0-3 (Xbox controller ports)
 */
DWORD xbox_InputGetState(DWORD dwPort, XBOX_INPUT_STATE *pState);

/**
 * Set controller vibration.
 */
DWORD xbox_InputSetState(DWORD dwPort, const XBOX_VIBRATION *pVibration);

/**
 * Check if a controller is connected.
 */
BOOL xbox_InputIsConnected(DWORD dwPort);

/**
 * Get controller capabilities.
 */
DWORD xbox_InputGetCapabilities(DWORD dwPort, DWORD dwFlags, XBOX_INPUT_CAPABILITIES *pCaps);

#ifdef __cplusplus
}
#endif

#endif /* BURNOUT3_XINPUT_XBOX_H */
