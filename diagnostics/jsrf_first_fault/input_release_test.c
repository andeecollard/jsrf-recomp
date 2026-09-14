/* The pad is released when a run is stopped, and releasing is idempotent.
 *
 * WHY THIS EXISTS. Nothing used to give the controller back. The only
 * SDL_GameControllerClose was in the hotplug path, there was no signal
 * handler, and main.c uses _exit on purpose so no atexit handler can run. A
 * run therefore always died with the HID interface still claimed, and the NEXT
 * run opened the device, got a handle, and received nothing ever again --
 * polls climbing, connected tracking them, nonneutral flat. It reads exactly
 * like a dead controller, which is how it cost most of a hand-played session
 * on 14 Sep 2026 before anyone suspected the harness rather than the pad.
 *
 * Uses SDL's virtual joystick, like input_hotplug_test, so it needs no
 * hardware and can run anywhere.
 *
 * NOT tested here, because a unit test cannot: that SIGKILL bypasses all of
 * this. It does, and always will -- the point of the handler is that play.sh
 * and play_scripted.sh send SIGTERM first and only escalate after five
 * seconds. Stopping a run with kill -9 still leaks the device.
 */
#include "xinput_xbox.h"

#include <SDL.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static void wait_for_rescan(void)
{
    const struct timespec delay = { 0, 300000000 };
    nanosleep(&delay, NULL);
}

int main(int argc, char **argv)
{
    const char *self = argv[0];

    /* The child half: bring input up, signal ourselves, and never reach the
     * _exit. If the handler swallows the signal we exit 42 instead and the
     * parent's WIFSIGNALED check fails. */
    if (argc > 1 && strcmp(argv[1], "--raise-sigterm") == 0) {
        xbox_InputInit();
        raise(SIGTERM);
        _exit(42);
    }

    XBOX_INPUT_STATE state;
    int device;
    int opened_any = 0;

    xbox_InputInit();

    /* Behaviour, not disposition. An earlier version asserted
     * sigaction(SIGTERM) != SIG_DFL and PASSED against code with the handlers
     * deleted -- SDL installs handlers of its own, so a non-default
     * disposition proves nothing about ours. The negative control caught it.
     *
     * What separates them is what happens next: ours releases the pad and
     * re-raises with the default disposition, so the process dies BY SIGTERM;
     * SDL's posts a quit event and lets it carry on. So re-exec ourselves with
     * an argument and check the child really died of the signal.
     *
     * fork+exec, not bare fork: macOS aborts on CoreFoundation use after a
     * fork without exec, and SDL is full of it -- a bare fork child dies the
     * wrong way and the test passes or fails for the wrong reason. */
    {
        pid_t pid = fork();
        CHECK(pid >= 0);
        if (pid == 0) {
            char *argv[] = { (char *)self, (char *)"--raise-sigterm", NULL };
            execv(self, argv);
            _exit(127);                  /* exec failed */
        } else {
            int st = 0;
            CHECK(waitpid(pid, &st, 0) == pid);
            CHECK(WIFSIGNALED(st));      /* not a normal exit */
            CHECK(WTERMSIG(st) == SIGTERM);
        }
    }

    /* A virtual pad, so there is something real to hand back. */
    device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                       SDL_CONTROLLER_AXIS_MAX,
                                       SDL_CONTROLLER_BUTTON_MAX, 0);
    if (device >= 0) {
        wait_for_rescan();
        for (int port = 0; port < XBOX_MAX_CONTROLLERS; port++)
            if (xbox_InputGetState((DWORD)port, &state) == ERROR_SUCCESS)
                opened_any = 1;
    }

    /* The subsystem is up either way -- xbox_InputInit brings it up. That is
     * the thing a killed run used to leave behind. */
    CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) != 0);

    xbox_InputReleasePads();
    CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0);

    /* Idempotent: the signal handler can race the normal exit path, and a
     * second signal arriving mid-release must not double-close anything. */
    xbox_InputReleasePads();
    CHECK(SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0);

    if (device >= 0)
        SDL_JoystickDetachVirtual(device);
    printf("input release: OK (handlers installed, subsystem dropped, "
           "idempotent; virtual pad %s)\n",
           opened_any ? "was opened" : "not opened on any port");
    return 0;
}
