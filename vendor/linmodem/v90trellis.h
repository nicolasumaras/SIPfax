#ifndef V90TRELLIS_H
#define V90TRELLIS_H
/* Experimental four/eight/twelve/twenty-point, 16-state V.34 upstream kernel. GPL-2.0.
 * Caller supplies carrier/gain-aligned pairs and the known V0 inversion bit.
 * Acquisition, superframe synchronization and PPP delivery are external.
 * Four-point native integration is opt-in with SIPFAX_V90_SOFT_RX=1.
 * Higher-rate acquisition and native integration live in v90qam8/v90upstream. */
#include <stdint.h>
#define V90_TRELLIS_DEPTH 64

typedef struct {
    double metric[16];
    uint8_t previous[V90_TRELLIS_DEPTH][16];
    uint32_t labels[V90_TRELLIS_DEPTH][16];
    uint64_t pairs;
} V90Trellis;
void v90_trellis_init(V90Trellis *s);
/* Read a provisional best-survivor pair with age pairs of lookahead.
 * Does not advance or mutate the decoder. label_bits must match its input
 * constellation (2..9). Returns 0 without writing outputs if unavailable. */
int v90_trellis_peek(const V90Trellis *s,unsigned age,unsigned label_bits,
                     unsigned *a,unsigned *b);
/* Inputs must be finite and gain-normalized to unit magnitude.
 * Quadrants are clockwise from +Re: 0=+1, 1=-j, 2=-1, 3=+j.
 * Returns 1 with a decoded pair after 63 pairs of lookahead, otherwise 0. */
int v90_trellis_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                    unsigned inversion,unsigned *a,unsigned *b);
/* Eight-point minimum constellation, no precoding/nonlinear encoder.
 * Caller aligns carrier/gain to quarter points (1,1),(-3,1). Returned labels
 * are quadrant|(ring<<2), with clockwise quadrants. Same 63-pair lookahead.
 * Returns -1 without changing state for nonfinite/unreasonably large input.
 * Reset before switching between four- and eight-point pair functions.
 * Ring decisions are nearest within a quadrant, not joint shell decoding. */
int v90_trellis_qam8_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                         unsigned inversion,unsigned *a,unsigned *b);
/* Twelve-point minimum constellation for 9600/3200, q=0, M=3.
 * Quarter points (1,1),(-3,1),(1,-3); labels retain ring index in bits 2..3.
 * Same return contract as qam8_pair. Reset before changing constellation.
 * This kernel alone does not enable 9600-bit/s acquisition or live reception. */
int v90_trellis_qam12_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *a,unsigned *b);
/* Twenty-point minimum constellation for 12000/3200, q=0, M=5.
 * Quarter points (1,1),(-3,1),(1,-3),(-3,-3),(1,5).
 * Labels use five bits; packed history retains both complete labels.
 * Same contract as qam12_pair; acquisition/live 12000 reception is separate. */
int v90_trellis_qam20_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *a,unsigned *b);
/* Thirty-two-point minimum constellation for 14400/3200, q=0, M=8.
 * Quarter points add (5,1),(-3,5),(5,-3) to the twenty-point set.
 * Same contract as qam20_pair; live acquisition is separate. */
int v90_trellis_qam32_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *a,unsigned *b);
/* Fifty-six-point minimum constellation for 16800/3200, q=0, M=14.
 * Labels occupy six bits; packed history retains all twelve pair bits.
 * Same contract as qam32_pair. Live acquisition is separate. */
int v90_trellis_qam56_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                          unsigned inversion,unsigned *a,unsigned *b);
/* Ninety-six-point minimum constellation for 19200/3200, M=12, q=1.
 * Labels are quadrant | (quarter-point index << 2), with seven bits each.
 * The quarter-point index includes the uncoded bit. Same pair contract. */
int v90_trellis_qam96_pair(V90Trellis *s,double ar,double ai,double br,double bi,
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
/* Experimental single timing-lane stream. Buffers acquisition samples and
 * replays them so acquisition latency does not discard early PPP bytes.
 * Callbacks can arrive in a bounded burst on acquisition. No timing tracking
 * or automatic loss-of-lock detection; caller resets at retraining. */
#define V90_STREAM_BUFFER 4096
typedef struct {
    double re[V90_STREAM_BUFFER],im[V90_STREAM_BUFFER];
    unsigned count,locked,have_a;
    double a_re,a_im;
    uint64_t pair_index,symbols,pair_origin;
    /* Original zero-based index of pair B, valid during receive_pair. */
    uint64_t output_symbol;
    V90TrellisAcquisition acquisition;
    V90Carrier carrier;
    V90Trellis trellis;
    void *opaque;
    void (*receive_pair)(void *,unsigned,unsigned);
} V90TrellisStream;
void v90_trellis_stream_init(V90TrellisStream *s);
void v90_trellis_stream_symbol(V90TrellisStream *s,double re,double im);
/* 21600/3200 minimum constellation, M=10/q=2, eight-bit labels. */
int v90_trellis_qam160_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                           unsigned inversion,unsigned *out_a,unsigned *out_b);
/* 24000/3200 minimum constellation, M=8/q=3, eight-bit labels. */
int v90_trellis_qam256_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                           unsigned inversion,unsigned *out_a,unsigned *out_b);
int v90_trellis_qam448_pair(V90Trellis *s,double ar,double ai,double br,double bi,
                           unsigned inversion,unsigned *out_a,unsigned *out_b);
#endif
