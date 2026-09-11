/* V.90 upstream V.34 framing parameters and inverse mapping. GPL-2.0. */
#ifndef V90MAPPING_H
#define V90MAPPING_H
#include "v90shell.h"
typedef struct {
    unsigned rate, symbol_rate, p, j, b, high_frames, k, m, q;
    double low_carrier, high_carrier;
    V90Shell shell;
} V90Mapping;
/* Minimum shaping, no auxiliary channel. Unsupported profiles leave s unchanged. */
int v90_mapping_init(V90Mapping *s,unsigned rate,unsigned symbol_rate);
/* Absolute mapping-frame index: frame zero starts B1, which occupies P frames.
 * Caller advances the index even when decoding an erased frame. */
unsigned v90_mapping_frame_bits(const V90Mapping *s,unsigned long long frame);
/* Decode eight trellis-decided labels. Returns bit count, or zero on invalid
 * labels/shell/arguments. Outputs are unchanged on rejection. Previous is the
 * first symbol quadrant of the preceding pair; next_previous is labels[6]&3.
 * Input/output storage must not overlap the mapping object. */
unsigned v90_mapping_decode(const V90Mapping *s,unsigned long long frame,
    unsigned previous,const uint16_t labels[8],uint8_t *bits,unsigned capacity,
    unsigned *next_previous);
#endif
