#ifndef SIPFAX_V34_S_DETECT_H
#define SIPFAX_V34_S_DETECT_H
#include <stdint.h>
#define V34_S_DETECT_MAX 460
/* Match 128 S symbols followed by eight S-bar symbols, with unknown phase. */
typedef struct {
    double a[V34_S_DETECT_MAX], b[V34_S_DETECT_MAX];
    double pcm[V34_S_DETECT_MAX], inv00, inv01, inv11, energy;
    double baud, score;
    unsigned length, cursor, filled;
    uint64_t samples;
    int found;
    double transition_sample;
} V34SDetect;
int v34_s_detect_init(V34SDetect *s, double baud, double carrier);
int v34_s_detect_sample(V34SDetect *s, int16_t pcm);
#endif
