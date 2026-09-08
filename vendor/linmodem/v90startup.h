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
    long info0_at, reverse_due, first_tx_reversal, second_rx_reversal;
    int ranging_state, tone_count, tone_locked, tone_position, tone_bad;
    double tone_re, tone_im, tone_energy, ref_re, ref_im;
    int16_t tone_history[80];
    long tone_history_count;

} V90Startup;
void v90_info0d(unsigned char bits[62], int alaw);
void v90_startup_init(V90Startup *s, int alaw);
void v90_startup_history(V90Startup *s, const int16_t *in, int n);
void v90_startup_process(V90Startup *s, int16_t *out, const int16_t *in, int n);
#endif
