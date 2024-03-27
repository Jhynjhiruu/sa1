#ifndef _VIDEO_H
#define _VIDEO_H

#include <ultra64.h>

#define WIDTH (320)
#define HEIGHT (240)

#define FRAMEBUFFER_ALIGN (8)

extern u16 framebuffer[WIDTH * HEIGHT] __attribute__((aligned(FRAMEBUFFER_ALIGN)));

s32 setup_vi(u16 *);

#endif