#ifndef V90STARTUP_H
#define V90STARTUP_H
#include <stdint.h>
/* Digital-side V.90 Phase 2. All clocks are counted in 8 kHz samples. */
typedef struct {
    int clock, count, have_previous;
    double re, im, previous_re, previous_im;
    unsigned char bits[49];
} V90InfoRx;
typedef struct {
    long samples;
    int alaw, tx_symbol, tx_sign, info0_received;
    unsigned char info0d[62];
    V90InfoRx rx[14];
} V90Startup;
void v90_info0d(unsigned char bits[62], int alaw);
void v90_startup_init(V90Startup *s, int alaw);
void v90_startup_history(V90Startup *s, const int16_t *in, int n);
void v90_startup_process(V90Startup *s, int16_t *out, const int16_t *in, int n);
#endif
