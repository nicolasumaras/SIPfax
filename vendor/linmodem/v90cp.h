#ifndef V90CP_H
#define V90CP_H
#include <stdint.h>
#define V90_CP_MAX_BITS 1788
typedef struct {
    unsigned type,drn,sr,ack,alaw,lookahead,gain,upstream_mask;
    unsigned count,codec_masks,silence;
    int filter[4];
    uint8_t indices[6],mask[2][6][128];
} V90Cp;
/* 1 valid, 0 incomplete, -1 invalid. Output is unchanged on failure. */
int v90_cp_parse(V90Cp *out,const uint8_t *bits,unsigned count,unsigned *consumed);
#endif
