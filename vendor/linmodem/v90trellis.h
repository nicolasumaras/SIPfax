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
/* Acquire the 448-pair J=7 superframe phase from 896..16384 hard pairs.
 * Labels are a|(b<<2). offset is relative to labels[0]. Returns 0 if
 * confidence is insufficient; errors reports the best syndrome score. */
int v90_trellis_sync(const uint8_t *labels,unsigned n,unsigned *offset,unsigned *errors);
unsigned v90_trellis_inversion(unsigned pair,unsigned offset);
typedef struct {
    double phase,gain,coherence;
    unsigned pair_alignment,offset,errors,pairs;
} V90TrellisAcquisition;
/* Buffered acquisition from one matched-filter timing hypothesis. Tests both
 * pair alignments; phase has an irrelevant quadrant ambiguity. No tracking
 * or timing-hypothesis selection is performed here. Returns 0 on rejection. */
int v90_trellis_acquire(const double *re,const double *im,unsigned symbols,
                       V90TrellisAcquisition *result);
typedef struct {
    double phase,gain,frequency;
    unsigned initialized;
} V90Carrier;
int v90_carrier_init(V90Carrier *s,double phase,double gain);
/* One matched-filter complex symbol per call. This tracks carrier/gain,
 * not symbol timing; returns zero for invalid state or nonfinite input. */
int v90_carrier_normalize(V90Carrier *s,double re,double im,double *out_re,double *out_im);
#endif
