#ifndef XBOXRECOMP_APU_SDL2_H
#define XBOXRECOMP_APU_SDL2_H

#include <stdint.h>

int apu_sdl2_init(void);
void apu_sdl2_shutdown(void);
int apu_sdl2_is_active(void);
int apu_sdl2_submit_samples(const int16_t *samples, int sample_frames);

#endif
