#include <PR/bb_fs.h>
#include <PR/os_internal.h>
#include <bbtypes.h>
#include <macros.h>
#include <ultra64.h>

#include "blocks.h"
#include "launch_app.h"
#include "sa2.h"
#include "stack.h"
#include "video.h"

void __osBbVideoPllInit(s32);
void osBbPowerOff(void);
void osBbSetErrorLed(u32);

s32 osBbUsbSetCtlrModes(s32, u32);
s32 osBbUsbInit(void);
u32 osBbUsbGetResetCount(s32);

void __osBbDelay(u32);

extern s32 __osBbIsBb;
extern u32 __osBbHackFlags;

u8 bootStack[STACK_SIZE] __attribute__((aligned(STACK_ALIGN)));

OSThread idlethread;
void idleproc(void *);
u8 idlestack[STACK_SIZE] __attribute__((aligned(STACK_ALIGN)));

OSThread mainthread;
void mainproc(void *);
u8 mainstack[STACK_SIZE] __attribute__((aligned(STACK_ALIGN)));

/*OSThread buttonthread;
void buttonproc(void *);
u8 buttonstack[STACK_SIZE] __attribute__((aligned(STACK_ALIGN)));*/

#define MESG_BUF_SIZE (200)

OSMesgQueue pi_mesg_queue;
OSMesg pi_mesg_buf[MESG_BUF_SIZE];

OSMesgQueue si_mesg_queue;
OSMesg si_mesg_buf[MESG_BUF_SIZE];

OSMesgQueue nmi_mesg_queue;
OSMesg nmi_mesg_buf[1];

OSMesgQueue vi_mesg_queue;
OSMesg vi_mesg_buf[1];
OSMesg vi_retrace_mesg;

OSMesgQueue controller_mesg_queue;
OSContStatus controller_status[MAXCONTROLLERS];
OSContPad controller_data[MAXCONTROLLERS];
u16 last_frame_buttons[MAXCONTROLLERS];

void *memcpy(void *dest, const void *src, size_t count) {
    u8 *const dest_ptr = dest;
    const u8 *const src_ptr = src;

    for (size_t i = 0; i < count; i++) {
        dest_ptr[i] = src_ptr[i];
    }

    return dest;
}

u8 stashed_state[0x100];
#define STASH_ADDR ((void *)PHYS_TO_K1(0x04A80100))

u32 controller_init(void) {
    u8 attached;
    u32 num_controllers = 0;

    osCreateMesgQueue(&controller_mesg_queue, &vi_retrace_mesg, sizeof(vi_retrace_mesg) / sizeof(OSMesg));
    osSetEventMesg(OS_EVENT_SI, &controller_mesg_queue, (OSMesg)0);

    osContInit(&controller_mesg_queue, &attached, controller_status);

    for (u32 i = 0; i < MAXCONTROLLERS; i++) {
        if ((attached & (1 << i)) && ((controller_status[i].errno & CONT_NO_RESPONSE_ERROR) == 0)) {
            num_controllers++;
        }
    }

    return num_controllers;
}

u32 read_controllers(void) {
    u16 status[MAXCONTROLLERS];
    u16 change[MAXCONTROLLERS];

    osRecvMesg(&controller_mesg_queue, &vi_retrace_mesg, OS_MESG_BLOCK);
    osContGetReadData(controller_data);

    for (u32 i = 0; i < MAXCONTROLLERS; i++) {
        OSContPad *pad = &controller_data[i];
        if (pad->errno == 0) {
            status[i] = pad->button;
            change[i] = status[i] ^ last_frame_buttons[i];
            last_frame_buttons[i] = status[i];
        }
    }

    return (status[0] << 16) | change[0];
}

#define PRESSED(key) ((change & (key)) && (status & (key)))

void boot(u32 entry_type) {
    // clear button interrupt
    IO_WRITE(MI_3C_REG, 0x01000000);

    osBbSetErrorLed(1);

    bcopy(STASH_ADDR, stashed_state, sizeof(stashed_state));

    if ((entry_type & 0x4C) == 0) {
        // coldboot

        __osBbVideoPllInit(OS_TV_NTSC);

        __osBbHackFlags = 0;

        // don't care about return value from this
        setup_vi(framebuffer);
    } else {
        // warmboot

        IO_WRITE(VI_H_VIDEO_REG, 0);
    }

    osInitialize();

    osCreateThread(&idlethread, 1, idleproc, (void *)entry_type, idlestack + sizeof(idlestack), 20);
    osStartThread(&idlethread);
}

void idleproc(void *argv) {
    osCreatePiManager(OS_PRIORITY_PIMGR, &pi_mesg_queue, pi_mesg_buf, ARRLEN(pi_mesg_buf));

    osCreateMesgQueue(&si_mesg_queue, si_mesg_buf, ARRLEN(si_mesg_buf));
    osSetEventMesg(OS_EVENT_SI, &si_mesg_queue, (OSMesg)ARRLEN(si_mesg_buf));

    osCreateThread(&mainthread, 3, mainproc, argv, mainstack + sizeof(mainstack), 18);
    osStartThread(&mainthread);

    osCreateMesgQueue(&nmi_mesg_queue, nmi_mesg_buf, ARRLEN(nmi_mesg_buf));
    osSetEventMesg(OS_EVENT_PRENMI, &nmi_mesg_queue, (OSMesg)ARRLEN(nmi_mesg_buf));

    osSetThreadPri(NULL, OS_PRIORITY_IDLE);

    while (TRUE)
        ;
}

void launch_sa2(SA2Entry addr, u32 entry_type) {
    __osDisableInt();
    addr(entry_type);
}

void mainproc(void *argv) {
    s32 ret;
    SA2Entry sa2_addr;
    u32 cont_data;
    u16 status, change;
    u32 launch_which = 0;

    osCreateViManager(OS_PRIORITY_VIMGR);

    osCreateMesgQueue(&vi_mesg_queue, vi_mesg_buf, ARRLEN(vi_mesg_buf));
    osViSetEvent(&vi_mesg_queue, vi_retrace_mesg, 1);
    osViBlack(1);
    osViSwapBuffer(framebuffer);

    osViBlack(0);
    osWritebackDCacheAll();
    osViSwapBuffer(framebuffer);

    /*osCreateThread(&buttonthread, 5, buttonproc, argv, buttonstack + sizeof(buttonstack), 15);
    osStartThread(&buttonthread);*/

    controller_init();

    osRecvMesg(&vi_mesg_queue, NULL, OS_MESG_BLOCK);
    osContStartReadData(&controller_mesg_queue);

    cont_data = read_controllers();

    status = cont_data >> 16;
    change = cont_data;

    if (PRESSED(START_BUTTON)) {
        launch_which = 1;
    }

    if (launch_which == 0) {
        ret = load_sa2(&sa2_addr);
        if (!ret) {
            // launch SA2!

            bcopy(stashed_state, STASH_ADDR, sizeof(stashed_state));

            osBbSetErrorLed(0);

            launch_sa2(sa2_addr, (u32)argv);
        }
        launch_which = 1;
    }
    if (launch_which == 1) {
        launch_app("btstrplo.app");
    }

    osBbPowerOff();
}

/*void buttonproc(void *argv) {
    static s32 button_released = TRUE;

    while (TRUE) {
        osRecvMesg(&vi_mesg_queue, NULL, OS_MESG_BLOCK);

        if (button_released == TRUE) {
            if (osRecvMesg(&nmi_mesg_queue, NULL, OS_MESG_NOBLOCK) == 0) {
                osBbPowerOff();
            }
        } else if ((IO_READ(MI_38_REG) & 0x01000000) == 0) {
            IO_WRITE(MI_3C_REG, 0x02000000);
            button_released = TRUE;
        }
    }
}*/