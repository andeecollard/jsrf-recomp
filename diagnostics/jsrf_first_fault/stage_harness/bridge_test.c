/* Isolated negative controls for the actual bridge input lease and state guard. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <assert.h>
typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef uint8_t BYTE;
typedef int16_t SHORT;
typedef struct { WORD wButtons; BYTE bAnalogButtons[8]; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XBOX_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XBOX_GAMEPAD Gamepad; } XBOX_INPUT_STATE;
#define XBOX_BUTTON_A 0
#define XBOX_BUTTON_B 1
#define XBOX_BUTTON_RTRIGGER 7
#define JSRF_RAM_TOP 0x08000000u
#define JSRF_ROOT_PTR_VA 0x22FCE0u
#define JSRF_ROOT_VA 0x5E3A70u
#define JSRF_IDS_OFF 0x98u
#define JSRF_SEQ_COUNT 64
#define JSRF_SEQ_TABLE_VA 0x20D2B8u
static struct { uint32_t va; } jsrf_seq_tab[64];
static unsigned sequence = 12;
static uintptr_t xbox_GetMemoryOffset(void) { return 1; }
static uint32_t jsrf_seq_index(const uint8_t *b) { (void)b; return sequence; }
static unsigned long xbox_InputFrame(void) { return 0; }
static int pad_inject(void) { return 0; }
int nv2a_pb_exec_snapshot_to_file(const char *p) { (void)p; return 0; }
#include "bridge.h"
int main(void)
{
    XBOX_INPUT_STATE state;
    assert(!jsrf_stage_pad(&state)); /* Off by default. */
    g_stage_dir = "/unused";
    g_stage_expected = 12;
    g_stage_command = 7;
    g_stage_pad.wButtons = 16;
    g_stage_deadline = stage_now() + 1;
    assert(jsrf_stage_pad(&state) && state.Gamepad.wButtons == 16);
    assert(g_stage_deliveries == 1);
    sequence = 30; /* A delayed START can never become gameplay PAUSE. */
    assert(jsrf_stage_pad(&state) && state.Gamepad.wButtons == 0);
    assert(g_stage_deliveries == 1);
    sequence = 12;
    g_stage_deadline = stage_now() - 1; /* A dead driver releases controls. */
    assert(jsrf_stage_pad(&state) && state.Gamepad.wButtons == 0);
    assert(g_stage_deliveries == 1);
    puts("bridge lease and scene-guard negative controls passed");
    return 0;
}
