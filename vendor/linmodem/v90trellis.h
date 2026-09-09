#ifndef V90TRELLIS_H
#define V90TRELLIS_H
/* Experimental four-point, 16-state V.34 upstream kernel. GPL-2.0.
 * Caller supplies carrier/gain-aligned pairs and the known V0 inversion bit.
 * Acquisition, superframe synchronization and PPP delivery are external.
 * No live data path uses this kernel yet. */
#include <stdint.h>
#define V90_TRELLIS_DEPTH 64

typedef struct {
    double metric[16];
    uint8_t previous[V90_TRELLIS_DEPTH][16];
    uint8_t labels[V90_TRELLIS_DEPTH][16];
    uint64_t pairs;
} V90Trellis;
void v90_trellis_init(V90Trellis *s);
/* Inputs must be finite and gain-normalized to unit magnitude.
 * Quadrants are clockwise from +Re: 0=+1, 1=-j, 2=-1, 3=+j.
 * Returns 1 with a decoded pair after 63 pairs of lookahead, otherwise 0. */
int v90_trellis_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                    unsigned inversion,unsigned *a,unsigned *b);
#endif
