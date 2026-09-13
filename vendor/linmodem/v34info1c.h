#ifndef SIPFAX_V34_INFO1C_H
#define SIPFAX_V34_INFO1C_H
#include <stdint.h>
#define V34_INFO1C_BITS 105
/* Validate framing and CRC before exposing the negotiated MD interval. */
int v34_info1c_parse(const uint8_t *bits, unsigned length, unsigned *md_ms);
typedef struct {
    unsigned clock, count, used;
    double re, im, previous_re, previous_im;
    int have_previous;
    uint8_t bits[V34_INFO1C_BITS];
} V34Info1cLane;
typedef struct {
    V34Info1cLane lanes[16];
    unsigned samples, md_ms;
    int valid;
} V34Info1c;
void v34_info1c_init(V34Info1c *s);
int v34_info1c_receive(V34Info1c *s, const int16_t *pcm, unsigned length);
#endif
