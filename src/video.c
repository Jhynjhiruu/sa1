#include <PR/os_internal.h>
#include <ultra64.h>

#include "video.h"

void __osBbDelay(u32);

u16 framebuffer[WIDTH * HEIGHT] __attribute__((aligned(FRAMEBUFFER_ALIGN)));

#define TEST_VALUE (0x43210123)

void setup_buffer_test(void) {
    u32 save_ctrl = IO_READ(VI_CONTROL_REG);

    IO_WRITE(VI_CONTROL_REG, VI_CTRL_TEST_ENABLE | VI_CTRL_KILL_WE);

    IO_WRITE(VI_BUFTEST_ADDR_REG, 0);
    IO_WRITE(VI_BUFTEST_DATA_REG, TEST_VALUE);

    IO_WRITE(VI_BUFTEST_ADDR_REG, 1);
    IO_WRITE(VI_BUFTEST_DATA_REG, 0);

    IO_WRITE(VI_BUFTEST_ADDR_REG, 2);
    IO_WRITE(VI_BUFTEST_DATA_REG, 0);

    IO_WRITE(VI_BUFTEST_ADDR_REG, 3);
    IO_WRITE(VI_BUFTEST_DATA_REG, 0);

    IO_WRITE(VI_CONTROL_REG, save_ctrl);
}

s32 buffer_test(void) {
    u32 save_ctrl = IO_READ(VI_CONTROL_REG);
    u32 data;

    IO_WRITE(VI_CONTROL_REG, VI_CTRL_TEST_ENABLE | VI_CTRL_KILL_WE);

    IO_WRITE(VI_BUFTEST_ADDR_REG, 0);
    data = IO_READ(VI_BUFTEST_DATA_REG);

    IO_WRITE(VI_CONTROL_REG, save_ctrl);

    if (data == TEST_VALUE) {
        return TRUE;
    }
    return FALSE;
}

#define LINES_TO_CLEAR (10)
#define WAIT_UNTIL_LINE (48)

s32 setup_vi(u16 *fbuf) {
    u32 temp;
    int attempts = 0;
    u32 save_mask = __osDisableInt();

    bzero(fbuf, WIDTH * LINES_TO_CLEAR * sizeof(u16));

    do {
        IO_WRITE(VI_CONTROL_REG, 0);
        __osBbDelay(10);

        temp = IO_READ(MI_30_REG) & ~0x02000000;
        IO_WRITE(MI_30_REG, temp);
        __osBbDelay(1);
        IO_WRITE(MI_30_REG, temp | 0x02000000);

        IO_WRITE(VI_ORIGIN_REG, fbuf);

        // NTSC LAN1
        IO_WRITE(VI_WIDTH_REG, 320);
        IO_WRITE(VI_INTR_REG, 520);
        IO_WRITE(VI_BURST_REG, (62 << 20) | (5 << 16) | (34 << 8) | 57);
        IO_WRITE(VI_V_SYNC_REG, 525);
        IO_WRITE(VI_H_SYNC_REG, 3093);
        IO_WRITE(VI_LEAP_REG, (3093 << 16) | 3093);
        IO_WRITE(VI_H_START_REG, (108 << 16) | 748);
        IO_WRITE(VI_V_START_REG, (37 << 16) | 511);
        IO_WRITE(VI_V_BURST_REG, (0 << 20) | (14 << 16) | (2 << 8) | 4);
        IO_WRITE(VI_X_SCALE_REG, 0x200);
        IO_WRITE(VI_Y_SCALE_REG, 0x400);

        setup_buffer_test();

        IO_WRITE(VI_CURRENT_REG, 0);

        IO_WRITE(VI_CONTROL_REG, VI_CTRL_PIXEL_ADV_1 | VI_CTRL_ANTIALIAS_MODE_2 | VI_CTRL_GAMMA_ON | VI_CTRL_GAMMA_DITHER_ON | VI_CTRL_TYPE_16);

        while (IO_READ(VI_CURRENT_REG) < WAIT_UNTIL_LINE)
            ;

        attempts++;
    } while (buffer_test());

    IO_WRITE(VI_H_START_REG, 0);

    __osRestoreInt(save_mask);

    return attempts - 1;
}