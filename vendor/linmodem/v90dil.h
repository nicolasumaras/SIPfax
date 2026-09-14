#ifndef V90DIL_H
#define V90DIL_H
#include <stdint.h>
#define V90_JA_MAX_BITS 2654
typedef struct {
    unsigned n, lsp, ltp;
    uint8_t sp[128],tp[128],h[8],reference[8],ucodes[255];
} V90Dil;
/* 1 valid; 0 incomplete; -1 invalid. consumed includes optional even padding. */
int v90_dil_parse(V90Dil *d,const uint8_t *bits,unsigned count,unsigned *consumed);
#endif
