#include <PR/bb_fs.h>
#include <bbtypes.h>
#include <bcp.h>
#include <macros.h>
#include <ultra64.h>

#include "launch_app.h"

#define MAX_CERTS 5

typedef struct {
    /* 0x00 */ BbTicket *ticket;
    /* 0x04 */ BbCertBase *ticketChain[MAX_CERTS];
    /* 0x18 */ BbCertBase *cmdChain[MAX_CERTS];
} BbTicketBundle; // size = 0x2C

typedef struct {
    /* 0x00 */ BbContentId contentId;
    /* 0x04 */ BbAesKey contentKey;
    /* 0x14 */ u32 state;
    /* 0x18 */ char unk18[8];
} RecryptListEntry;

typedef struct {
    /* 0x00 */ BbEccSig signature;
    /* 0x40 */ u32 numEntries;
    /* 0x44 */ RecryptListEntry entries[1 /*numEntries*/];
} RecryptList;

s32 skLaunchSetup(BbTicketBundle *, BbAppLaunchCrls *, RecryptList *);
s32 skLaunch(void *);

s32 osBbAtbSetup(u32, u16 *, u32);

void osBbSetErrorLed(u32);

BbTicket ticket = {.cmd = {.contentDesc = {0},
                           .head =
                               {
                                   .unusedPadding = 0,
                                   .caCrlVersion = 0,
                                   .cpCrlVersion = 1,
                                   .size = 0,
                                   .descFlags = 0,
                                   .commonCmdIv = {0},
                                   .hash = {0},
                                   .iv = {0},
                                   .execFlags = 0,
                                   .hwAccessRights = 0xFFFFFFFF,
                                   .secureKernelRights = 0xFFFFFFFF,
                                   .bbid = 0,
                                   .issuer = {0},
                                   .id = 0,
                                   .key = {0x27DAE074, 0x05A192C6, 0x3610BA22, 0x46EACF5C},
                                   .contentMetaDataSign = {0},
                               }},
                   .head = {
                       .bbId = 0,
                       .tid = 0,
                       .code = 0,
                       .limit = 0,
                       .reserved = 0,
                       .tsCrlVersion = 0,
                       .cmdIv = {0},
                       .serverKey = {0},
                       .issuer = {0},
                       .ticketSign = {0},
                   }};

static OSBbFs fs;

#define MAX_BLOCKS (4096)

u16 app_blocks[MAX_BLOCKS + 1];

void launch_app(const char *filename) {
    s32 fd;

    OSBbStatBuf stat;

    BbTicketBundle bundle = {
        .ticket = &ticket,
        .ticketChain = {NULL, NULL, NULL, NULL, NULL},
        .cmdChain = {NULL, NULL, NULL, NULL, NULL},
    };

    OSMesgQueue dma_queue;
    OSIoMesg dma_mesg;
    OSMesg dma_mesg_buf[1];
    OSPiHandle *cart_handle;

    void *entrypoint;
    void *load_addr;

    if (osBbFInit(&fs)) {
        return;
    }

    fd = osBbFOpen(filename, "r");
    if (fd < 0) {
        return;
    }

    if (osBbFStat(fd, &stat, app_blocks, MAX_BLOCKS)) {
        osBbFClose(fd);
        return;
    }

    ticket.cmd.head.size = stat.size;

    if (skLaunchSetup(&bundle, NULL, NULL)) {
        return;
    }

    app_blocks[MAX_BLOCKS] = 0;
    if (osBbAtbSetup(PI_DOM1_ADDR2, app_blocks, MAX_BLOCKS + 1)) {
        return;
    }

    osCreateMesgQueue(&dma_queue, dma_mesg_buf, ARRLEN(dma_mesg_buf));

    cart_handle = osCartRomInit();

    IO_WRITE(PI_48_REG, 0x1F008BFF);

    entrypoint = *(void **)PHYS_TO_K1(PI_DOM1_ADDR2 + 8);
    load_addr = entrypoint - 0x1000;

    dma_mesg.hdr.pri = OS_MESG_PRI_NORMAL;
    dma_mesg.hdr.retQueue = &dma_queue;
    dma_mesg.dramAddr = load_addr;
    dma_mesg.devAddr = 0;
    dma_mesg.size = MIN(stat.size, 1024 * 1024 + 0x1000);
    osEPiStartDma(cart_handle, &dma_mesg, OS_READ);

    osRecvMesg(&dma_queue, NULL, OS_MESG_BLOCK);

    osWritebackDCacheAll();
    // clear 64KiB of instruction cache for some reason????
    osInvalICache((void *)K0BASE, 64 * 1024);

    osRomBase = (void *)PHYS_TO_K1(PI_DOM1_ADDR2);
    osMemSize = 8 * 1024 * 1024;
    osTvType = OS_TV_NTSC;

    osBbSetErrorLed(0);

    skLaunch(entrypoint);
}
