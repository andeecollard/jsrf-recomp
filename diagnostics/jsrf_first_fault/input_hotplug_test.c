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
    int virtual_port = -1;
    int initially_connected[XBOX_MAX_CONTROLLERS] = { 0 };

    xbox_InputInit();
    for (int port = 0; port < XBOX_MAX_CONTROLLERS; port++)
        initially_connected[port] =
            xbox_InputGetState((DWORD)port, &state) == ERROR_SUCCESS;

    device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                       SDL_CONTROLLER_AXIS_MAX,
                                       SDL_CONTROLLER_BUTTON_MAX, 0);
    if (device < 0) {
        fprintf(stderr, "virtual controller attach failed: %s\n", SDL_GetError());
        return 1;
    }

    wait_for_rescan();
    for (int port = 0; port < XBOX_MAX_CONTROLLERS; port++) {
        if (!initially_connected[port] &&
            xbox_InputGetState((DWORD)port, &state) == ERROR_SUCCESS) {
            virtual_port = port;
            break;
        }
    }
    if (virtual_port < 0) {
        fprintf(stderr, "hot-plugged controller was not discovered: %s\n",
                SDL_GetError());
        return 1;
    }

    joystick = SDL_JoystickOpen(device);
    if (!joystick) {
        fprintf(stderr, "virtual controller open failed: %s\n", SDL_GetError());
        return 1;
    }

    for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; button++) {
        if (SDL_JoystickSetVirtualButton(joystick, button, 1) < 0) {
            fprintf(stderr, "virtual button %d press failed: %s\n",
                    button, SDL_GetError());
            return 1;
        }
    }
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, 12345);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTY, -12345);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTX, 23456);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_RIGHTY, -23456);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    SDL_GameControllerUpdate();
    if (xbox_InputGetState((DWORD)virtual_port, &state) != ERROR_SUCCESS ||
        state.Gamepad.wButtons !=
            (XBOX_GAMEPAD_DPAD_UP | XBOX_GAMEPAD_DPAD_DOWN |
             XBOX_GAMEPAD_DPAD_LEFT | XBOX_GAMEPAD_DPAD_RIGHT |
             XBOX_GAMEPAD_START | XBOX_GAMEPAD_BACK |
             XBOX_GAMEPAD_LEFT_THUMB | XBOX_GAMEPAD_RIGHT_THUMB) ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_A] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_B] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_X] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_Y] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] != 255 ||
        state.Gamepad.sThumbLX != 12345 ||
        state.Gamepad.sThumbLY != 12344 ||
        state.Gamepad.sThumbRX != 23456 ||
        state.Gamepad.sThumbRY != 23455) {
        fprintf(stderr,
                "hot-plugged controller controls were not fully translated:"
                " buttons=%04X analog=%u,%u,%u,%u,%u,%u,%u,%u"
                " sticks=%d,%d,%d,%d\n",
                state.Gamepad.wButtons,
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_A],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_B],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_X],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_Y],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER],
                state.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER],
                state.Gamepad.sThumbLX, state.Gamepad.sThumbLY,
                state.Gamepad.sThumbRX, state.Gamepad.sThumbRY);
        return 1;
    }

    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0);
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0);
    SDL_GameControllerUpdate();
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 1);
    SDL_GameControllerUpdate();
    if (xbox_InputGetState((DWORD)virtual_port, &state) != ERROR_SUCCESS ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] != 0) {
        fprintf(stderr, "left shoulder was not translated to Xbox White\n");
        return 1;
    }
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0);
    SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
    SDL_GameControllerUpdate();
    if (xbox_InputGetState((DWORD)virtual_port, &state) != ERROR_SUCCESS ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_BLACK] != 255 ||
        state.Gamepad.bAnalogButtons[XBOX_BUTTON_WHITE] != 0) {
        fprintf(stderr, "right shoulder was not translated to Xbox Black\n");
        return 1;
    }

    SDL_JoystickClose(joystick);
    if (SDL_JoystickDetachVirtual(device) < 0) {
        fprintf(stderr, "virtual controller detach failed: %s\n", SDL_GetError());
        return 1;
    }
    wait_for_rescan();
    if (xbox_InputGetState((DWORD)virtual_port, &state) == ERROR_SUCCESS) {
        fprintf(stderr, "detached controller still reported connected\n");
        return 1;
    }

    puts("controller hotplug, full control translation and disconnect passed");
    return 0;
}
