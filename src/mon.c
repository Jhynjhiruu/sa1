#include <PR/bb_fs.h>
#include <bbtypes.h>
#include <libfb.h>
#include <macros.h>

#include "blocks.h"
#include "mon.h"
#include "stack.h"

s32 skGetId(BbId *);
s32 skSignHash(BbShaHash *, BbEccSig *);

void osBbSetErrorLed(s32);
void __osBbDelay(u32);

void osBbCardInit(void);
s32 osBbCardReadBlock(u32, u16, void *, void *);
s32 osBbCardEraseBlock(u32, u16);
s32 osBbCardWriteBlock(u32, u16, void *, void *);
s32 osBbCardStatus(u32, u8 *);
s32 osBbCardChange(void);
s32 osBbCardClearChange(void);
u32 osBbCardBlocks(u32);

s32 osBbReadHost(void *, u32);
s32 osBbWriteHost(void *, u32);

void osBbRtcInit(void);
void osBbRtcSet(u8, u8, u8, u8, u8, u8, u8);

extern s32 __osBbIsBb;

OSBbFs fs;

OSThread ledthread;
void ledproc(void *);
u8 ledstack[STACK_SIZE] __attribute__((aligned(STACK_ALIGN)));

OSMesgQueue led_mesg_queue;
OSMesg led_mesg_buf[1];

char filename_buf[0x100];

// holds 1 block
u8 block_buf[BYTES_PER_BLOCK];

// holds 1 spare
u8 spare_buf[16];

u8 bad_buf[BYTES_PER_BLOCK];

void flash_led(u32 delay) {
    osBbSetErrorLed(1);
    __osBbDelay(delay);
    osBbSetErrorLed(0);
    __osBbDelay(delay);
}

void ledproc(void *argv) {
    OSTimer timer;
    s32 led_val = 1;

    while (TRUE) {
        osBbSetErrorLed(led_val);
        osSetTimer(&timer, OS_USEC_TO_CYCLES(500000), 0, &led_mesg_queue, NULL);
        osRecvMesg(&led_mesg_queue, NULL, OS_MESG_BLOCK);
        led_val ^= 1;
    }
}

void start_led_thread(void) {
    static s32 initialised = FALSE;
    if (initialised == FALSE) {
        osCreateMesgQueue(&led_mesg_queue, led_mesg_buf, ARRLEN(led_mesg_buf));
        osCreateThread(&ledthread, 9, ledproc, NULL, ledstack + sizeof(ledstack), 9);
        initialised = TRUE;
    }
    osStartThread(&ledthread);
}

void stop_led_thread(void) {
    osStopThread(&ledthread);
    osBbSetErrorLed(0);
}

u32 update_checksum(u8 *data, u32 size, u32 checksum) {
    for (u32 i = 0; i < size; i++) {
        checksum += data[i];
    }
    return checksum;
}

s32 checksum_file(const char *filename, u32 size, u32 expected_checksum) {
    s32 ret;

    OSBbStatBuf stat;
    s32 fd;
    u32 offset;
    u32 computed_checksum = 0;
    u32 block_size;
    u32 remaining;

    fd = osBbFOpen(filename, "r");
    if (fd < 0) {
        return fd;
    }

    ret = osBbFStat(fd, &stat, NULL, 0);
    if (ret < 0) {
        return ret;
    }

    remaining = size;
    if (stat.size < remaining) {
        remaining = stat.size;
    }

    offset = 0;
    while (remaining > 0) {
        bzero(block_buf, sizeof(block_buf));
        osInvalDCache(block_buf, sizeof(block_buf));

        block_size = MIN(remaining, sizeof(block_buf));

        ret = osBbFRead(fd, offset, block_buf, block_size);
        if (ret < 0) {
            return ret;
        }
        remaining -= block_size;
        offset += block_size;
        computed_checksum = update_checksum(block_buf, block_size, computed_checksum);
    }

    return (expected_checksum == computed_checksum) ? 0 : 1;
}

typedef enum {
    CMD_WRITE_BLOCK = 6,
    CMD_READ_BLOCK = 7,

    CMD_NAND_BLOCK_STATS = 0xD,

    CMD_WRITE_BLOCK_WITH_SPARE = 0x10,
    CMD_READ_BLOCK_WITH_SPARE = 0x11,
    CMD_INIT_FS = 0x12,

    CMD_CARD_SIZE = 0x15,
    CMD_SET_SEQ_NUM = 0x16,
    CMD_GET_SEQ_NUM = 0x17,

    CMD_FILE_CHECKSUM = 0x1C,
    CMD_SET_LED = 0x1D,
    CMD_SET_TIME = 0x1E,
    CMD_GET_BBID = 0x1F,
    CMD_SIGN_HASH = 0x20,
} CmdId;

s32 mon(void) {
    s32 ret = 0;

    u32 data_in[2];
    u32 data_out[2];
    u8 status;

    s32 card_present;
    s32 card_changed;
    u32 card_seqno = 0;

    fbClear();

    fbPrintStr(FB_WHITE, 3, 3, "Mon init");
    osWritebackDCacheAll();

    if (__osBbIsBb) {
        osBbRtcInit();
        osBbCardInit();
        osBbFInit(&fs);
    }

    card_present = osBbCardClearChange();
    flash_led(100000);

    while (TRUE) {
        if (ret < 0) {
            stop_led_thread();
        }

        ret = osBbReadHost(data_in, sizeof(data_in));
        if (ret < 0) {
            continue;
        }

        osBbCardStatus(0, &status);
        card_changed = osBbCardChange();
        if (card_changed) {
            card_present = osBbCardClearChange();
            if (card_present) {
                osBbCardInit();
            }
        }

        data_out[0] = 0xFF - data_in[0];
        data_out[1] = 0;

        switch (data_in[0]) {
            case CMD_FILE_CHECKSUM:
                {
                    u32 length = ALIGN(data_in[1], 4);

                    length = MIN(length, sizeof(filename_buf));

                    ret = osBbReadHost(filename_buf, length);
                    if (ret < 0) {
                        break;
                    }

                    // ensure null-terminated
                    filename_buf[ARRLEN(filename_buf) - 1] = 0;

                    ret = osBbReadHost(data_in, sizeof(data_in));
                    if (ret < 0) {
                        break;
                    }

                    data_out[1] = (checksum_file(filename_buf, data_in[1], data_in[0]) == 0) ? 0 : -1;
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_SET_LED:
                {
                    u32 led_value;

                    stop_led_thread();

                    led_value = data_in[1] & 3;

                    if ((led_value == 0) || (led_value == 1)) {
                        osBbSetErrorLed(0);
                    } else if (led_value == 2) {
                        osBbSetErrorLed(1);
                    } else if (led_value == 3) {
                        start_led_thread();
                    }
                    data_out[1] = 0;
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_SET_TIME:
                {
                    u8 year, month, day, dow, hour, min, sec;

                    data_out[1] = 0;

                    year = data_in[1] >> 24 & 0xFF;
                    month = data_in[1] >> 16 & 0xFF;
                    day = data_in[1] >> 8 & 0xFF;
                    dow = data_in[1] >> 0 & 0xFF;

                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    if (ret < 0) {
                        break;
                    }

                    ret = osBbReadHost(data_in, sizeof(data_in[0]));
                    if (ret < 0) {
                        break;
                    }

                    hour = data_in[0] >> 16 & 0xFF;
                    min = data_in[0] >> 8 & 0xFF;
                    sec = data_in[0] >> 0 & 0xFF;

                    osBbRtcSet(year, month, day, dow, hour, min, sec);
                    break;
                }

            case CMD_WRITE_BLOCK:
                {
                    ret = osBbReadHost(block_buf, sizeof(block_buf));
                    if (ret < 0) {
                        break;
                    }

                    osBbCardEraseBlock(0, data_in[1]);
                    data_out[1] = osBbCardWriteBlock(0, data_in[1], block_buf, NULL);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_WRITE_BLOCK_WITH_SPARE:
                {
                    ret = osBbReadHost(block_buf, sizeof(block_buf));
                    if (ret < 0) {
                        break;
                    }

                    ret = osBbReadHost(spare_buf, sizeof(spare_buf));
                    if (ret < 0) {
                        break;
                    }

                    osBbCardEraseBlock(0, data_in[1]);
                    data_out[1] = osBbCardWriteBlock(0, data_in[1], block_buf, spare_buf);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_READ_BLOCK:
                {
                    data_out[1] = osBbCardReadBlock(0, data_in[1], block_buf, NULL);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    if (ret < 0) {
                        break;
                    }

                    ret = osBbWriteHost(block_buf, sizeof(block_buf));
                    break;
                }

            case CMD_READ_BLOCK_WITH_SPARE:
                {
                    data_out[1] = osBbCardReadBlock(0, data_in[1], block_buf, spare_buf);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    if (ret < 0) {
                        break;
                    }

                    ret = osBbWriteHost(block_buf, sizeof(block_buf));
                    if (ret < 0) {
                        break;
                    }

                    ret = osBbWriteHost(spare_buf, sizeof(spare_buf));
                    break;
                }

            case CMD_NAND_BLOCK_STATS:
                {
                    u32 num_blocks = osBbCardBlocks(0);

                    for (u32 i = 0; i < num_blocks; i++) {
                        spare_buf[5] = 0xFF;
                        osBbCardReadBlock(0, i, block_buf, spare_buf);
                        // indicates a bad block
                        bad_buf[i] = (spare_buf[5] != 0xFF);
                    }

                    data_out[1] = num_blocks;

                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    if (ret < 0) {
                        break;
                    }

                    ret = osBbWriteHost(bad_buf, num_blocks * sizeof(bad_buf[0]));
                    break;
                }

            case CMD_INIT_FS:
                {
                    data_out[1] = osBbFInit(&fs);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_CARD_SIZE:
                {
                    data_out[1] = osBbCardBlocks(0);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_SET_SEQ_NUM:
                {
                    card_seqno = card_present ? data_in[1] : 0;
                    data_out[1] = card_seqno;
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_GET_SEQ_NUM:
                {
                    if (card_present == 0) {
                        data_out[1] = __UINT32_MAX__;
                    } else {
                        data_out[1] = card_seqno;
                    }
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_GET_BBID:
                {
                    skGetId(&data_out[1]);
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }

            case CMD_SIGN_HASH:
                {
                    u32 size = data_in[1];

                    if (size == sizeof(BbShaHash)) {
                        BbShaHash hash;
                        BbEccSig ecc_sig;

                        ret = osBbReadHost(&hash, sizeof(hash));
                        if (ret < 0) {
                            break;
                        }

                        skSignHash(&hash, &ecc_sig);
                        data_out[1] = sizeof(ecc_sig);

                        ret = osBbWriteHost(data_out, sizeof(data_out));
                        if (ret < 0) {
                            break;
                        }

                        ret = osBbWriteHost(&ecc_sig, sizeof(ecc_sig));
                        break;
                    }

                    data_out[1] = __UINT32_MAX__;

                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    if (ret < 0) {
                        break;
                    }

                    while (size > 0) {
                        u32 remaining = MIN(size, BYTES_PER_BLOCK);

                        ret = osBbReadHost(block_buf, remaining);
                        if (ret < 0) {
                            break;
                        }

                        size -= remaining;
                    }
                    break;
                }

            default:
                {
                    data_out[1] = __UINT32_MAX__;
                    ret = osBbWriteHost(data_out, sizeof(data_out));
                    break;
                }
        }
    }

    return ret;
}