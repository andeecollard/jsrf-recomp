#include "xinput_xbox.h"

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void wait_for_rescan(void)
{
    const struct timespec delay = { 0, 300000000 };
    nanosleep(&delay, NULL);
}

int main(void)
{
    XBOX_INPUT_STATE state;
    SDL_Joystick *joystick;
    int device;

    xbox_InputInit();
    if (xbox_InputGetState(0, &state) == ERROR_SUCCESS) {
        fprintf(stderr, "unexpected physical controller in isolated test\n");
        return 1;
    }

    device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                       SDL_CONTROLLER_AXIS_MAX,
                                       SDL_CONTROLLER_BUTTON_MAX, 0);
    if (device < 0) {
        fprintf(stderr, "virtual controller attach failed: %s\n", SDL_GetError());
        return 1;
    }

    wait_for_rescan();
    if (xbox_InputGetState(0, &state) != ERROR_SUCCESS) {
        fprintf(stderr, "hot-plugged controller was not discovered: %s\n",
                SDL_GetError());
        return 1;
    }

    joystick = SDL_JoystickOpen(device);
    if (!joystick ||
        SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, 1) < 0) {
        fprintf(stderr, "virtual A press failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GameControllerUpdate();
    if (xbox_InputGetState(0, &state) != ERROR_SUCCESS ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 255) {
        fprintf(stderr, "hot-plugged controller A press was not translated\n");
        return 1;
    }

    SDL_JoystickClose(joystick);
    if (SDL_JoystickDetachVirtual(device) < 0) {
        fprintf(stderr, "virtual controller detach failed: %s\n", SDL_GetError());
        return 1;
    }
    wait_for_rescan();
    if (xbox_InputGetState(0, &state) == ERROR_SUCCESS) {
        fprintf(stderr, "detached controller still reported connected\n");
        return 1;
    }

    puts("controller hotplug, button translation and disconnect passed");
    return 0;
}
