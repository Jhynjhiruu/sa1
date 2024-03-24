#include <bbtypes.h>
#include <gzip.h>
#include <libfb.h>
#include <macros.h>

#include "blocks.h"
#include "sa2.h"

extern const void __sa1_end;

s32 osBbAtbSetup(u32, u16 *, u32);

u8 cmd_buf[BYTES_PER_BLOCK];

#define SK_SIZE (4)
#define MAX_SKSA_BLOCKS (64)

// this is larger than it needs to be
// sa2's max size is MAX_SKSA_BLOCKS - SK_SIZE - sa1_num_blocks - 2
u16 sa2_blocks[MAX_SKSA_BLOCKS];

#define BUF_SIZE (1 * 1024 * 1024)
u8 compressed_buf[BUF_SIZE] __attribute__((aligned(BUF_SIZE), section(".buf")));
u8 decompressed_buf[BUF_SIZE] __attribute__((aligned(BUF_SIZE), section(".buf")));

#define N64_ROM_HEADER_SIZE (0x1000)
#define N64_ROM_HEADER_LOADADDR_OFFSET (8)

#define RAM_END (PHYS_TO_K0(0x00800000))

s32 read_page(u32 page) {
    IO_WRITE(PI_70_REG, page * BYTES_PER_PAGE);

    IO_WRITE(PI_48_REG, 0x9F008A10);

    do {
        if (IO_READ(MI_38_REG) & 0x02000000) {
            IO_WRITE(PI_48_REG, 0);
            return 2;
        }
    } while (IO_READ(PI_48_REG) & 0x80000000);

    if (IO_READ(PI_48_REG) & 0x00000400) {
        return 1;
    }

    return 0;
}

s32 block_link(u32 spare) {
    // the link is stored in the spare data 3 times, so get the best 2 of 3
    u8 a = (spare >> 8), b = (spare >> 16), c = (spare >> 24);
    if (a == b) {
        return a;
    } else {
        return c;
    }
}

s32 find_next_good_block(u16 *out_block, u16 start_block) {
    s32 ret;
    u32 block_status;

    while (TRUE) {
        s32 num_bad_bits = 0;

        ret = read_page(start_block * PAGES_PER_BLOCK);
        if (ret == 2) {
            // fatal error
            return 1;
        }

        block_status = IO_READ(PI_10404_REG);

        for (u32 i = 0; i < 8; i++) {
            if (((block_status >> (i + 16)) & 1) == 0) {
                num_bad_bits++;
            }
        }

        start_block++;

        if (num_bad_bits < 2) {
            break;
        }
    }

    if (ret == 0) {
        *out_block = start_block - 1;
    }

    return ret;
}

s32 load_sa2_blocks(BbContentMetaDataHead *cmd, u16 *blocks, u32 num_blocks, void *dst) {
    s32 ret;

    OSMesgQueue dma_queue;
    OSIoMesg dma_mesg;
    OSMesg dma_mesg_buf[1];
    OSPiHandle *cart_handle;

    osCreateMesgQueue(&dma_queue, dma_mesg_buf, ARRLEN(dma_mesg_buf));

    if (num_blocks == 0) {
        return 1;
    }

    // already done in the caller, but may as well make sure!
    blocks[num_blocks] = 0;
    ret = osBbAtbSetup(PI_DOM1_ADDR2, blocks, num_blocks + 1);
    if (ret < 0) {
        return 1;
    }

    cart_handle = osCartRomInit();

    IO_WRITE(PI_48_REG, 0x1F008BFF);

    dma_mesg.hdr.pri = OS_MESG_PRI_NORMAL;
    dma_mesg.hdr.retQueue = &dma_queue;
    dma_mesg.dramAddr = dst;
    dma_mesg.devAddr = 0;
    dma_mesg.size = num_blocks * BYTES_PER_BLOCK;
    osEPiStartDma(cart_handle, &dma_mesg, OS_READ);

    osRecvMesg(&dma_queue, NULL, OS_MESG_BLOCK);

    osWritebackDCacheAll();
    // clear 64KiB of instruction cache for some reason????
    osInvalICache((void *)K0BASE, 64 * 1024);

    osRomBase = (void *)PHYS_TO_K1(PI_DOM1_ADDR2);
    osMemSize = 4 * 1024 * 1024;
    osTvType = OS_TV_NTSC;

    return 0;
}

s32 decompress_sa2(SA2Entry *loadaddr, BbContentMetaDataHead *cmd, u16 *blocks, u32 num_blocks) {
    s32 ret;
    u32 decompressed_size;

    ret = load_sa2_blocks(cmd, blocks, num_blocks, compressed_buf);
    if (ret) {
        return ret;
    }

    fbPrintf(FB_WHITE, 3, 7, "Loaded SA2 blocks");
    osWritebackDCacheAll();

    ret = expand_gzip((void *)compressed_buf, (void *)decompressed_buf, cmd->size, MAX_SKSA_BLOCKS * BYTES_PER_BLOCK);
    if (ret < 0) {
        fbPrintf(FB_WHITE, 3, 8, "GZIP error: %d", ret);
        osWritebackDCacheAll();

        return 1;
    }

    decompressed_size = ret;

    fbPrintf(FB_WHITE, 3, 8, "Size: 0x%x", decompressed_size);
    osWritebackDCacheAll();

    if (decompressed_size < N64_ROM_HEADER_SIZE) {
        return 1;
    }

    *loadaddr = *(SA2Entry *)(decompressed_buf + N64_ROM_HEADER_LOADADDR_OFFSET);

    fbPrintf(FB_WHITE, 3, 9, "Addr: 0x%x", *loadaddr);
    osWritebackDCacheAll();

    // disallow overwriting SA1, except do it properly
    if ((void *)K1_TO_K0(*loadaddr) < &__sa1_end) {
        return 1;
    }

    if (K1_TO_K0(*loadaddr) >= RAM_END) {
        return 1;
    }

    bcopy(decompressed_buf + N64_ROM_HEADER_SIZE, *loadaddr, decompressed_size - N64_ROM_HEADER_SIZE);

    return 0;
}

s32 load_sa_ticket(u16 *sa_start_block, u16 start_block) {
    s32 ret;
    u16 ticket_block;

    ret = find_next_good_block(&ticket_block, start_block);
    if (ret) {
        return ret;
    }

    for (u32 i = 0; i < PAGES_PER_BLOCK; i++) {
        ret = read_page((ticket_block * PAGES_PER_BLOCK) + i);
        if (ret) {
            return ret;
        }

        if (i == 0) {
            *sa_start_block = block_link(IO_READ(PI_10400_REG));
        }

        for (u32 j = 0; j < BYTES_PER_PAGE; j += 4) {
            *(u32 *)(cmd_buf + i * BYTES_PER_PAGE + j) = IO_READ(PI_10000_BUF(j));
        }
    }

    return ret;
}

s32 load_sa2(SA2Entry *loadaddr) {
    s32 ret;
    BbContentMetaDataHead *cmd;
    u16 sa1_start, sa2_start;
    u32 sa1_num_blocks, sa2_num_blocks;
    u16 sa2_cmd;

    ret = load_sa_ticket(&sa1_start, SK_SIZE);
    if (ret) {
        return ret;
    }

    fbPrintf(FB_WHITE, 3, 3, "SA1: 0x%x", sa1_start);
    osWritebackDCacheAll();

    cmd = (BbContentMetaDataHead *)cmd_buf;
    sa1_num_blocks = cmd->size / BYTES_PER_BLOCK;

    sa2_cmd = sa1_start;
    for (u32 i = 0; i < sa1_num_blocks; i++) {
        ret = read_page(sa2_cmd * PAGES_PER_BLOCK);
        if (ret) {
            return ret;
        }

        sa2_cmd = block_link(IO_READ(PI_10400_REG));
    }

    fbPrintf(FB_WHITE, 3, 4, "SA2 CMD: 0x%x", sa2_cmd);
    osWritebackDCacheAll();

    ret = load_sa_ticket(&sa2_start, sa2_cmd);
    if (ret) {
        return ret;
    }

    fbPrintf(FB_WHITE, 3, 5, "SA2: 0x%x", sa2_start);
    osWritebackDCacheAll();

    sa2_num_blocks = cmd->size / BYTES_PER_BLOCK;
    if (sa2_num_blocks > (MAX_SKSA_BLOCKS - SK_SIZE - sa1_num_blocks - 2)) {
        return 1;
    }

    fbPrintf(FB_WHITE, 3, 6, "SA2 blocks: 0x%x", sa2_num_blocks);
    osWritebackDCacheAll();

    sa2_blocks[0] = sa2_start;
    for (u32 i = 0; i < sa2_num_blocks; i++) {
        ret = read_page(sa2_blocks[i] * PAGES_PER_BLOCK);
        if (ret) {
            return ret;
        }

        sa2_blocks[i + 1] = block_link(IO_READ(PI_10400_REG));
    }
    sa2_blocks[sa2_num_blocks] = 0;

    return decompress_sa2(loadaddr, cmd, sa2_blocks, sa2_num_blocks);
}