#ifndef _SA2_H
#define _SA2_H

#include <ultra64.h>

typedef void (*SA2Entry)(u32);

s32 load_sa2(SA2Entry *);

#endif