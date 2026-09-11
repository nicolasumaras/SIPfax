#ifndef V90STARTUP_H
#define V90STARTUP_H
#include <stdint.h>
#include "v90training.h"
#include "v90train_tx.h"
#include "v90phase4.h"
/* Digital-side V.90 Phase 2. All clocks are counted in 8 kHz samples. */
typedef struct {
    int clock, count, have_previous;
    double re, im, previous_re, previous_im;
    unsigned char bits[70];
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
    long probe_start, info1_start, probe_reply, round_trip, info1_received_at;
    unsigned char info1d[109];
    unsigned upstream_max_rate,upstream_data_rate,peer_large_constellations;
    unsigned peer_carriers,forced_symbol_rate,upstream_symbol_rate,upstream_high_carrier;
    V90Training training;
    int training_active,training_tx_active;
    V90TrainTx training_tx;
    V90SDetect s_detector;
    unsigned s_transitions;
    int phase4_active;
    V90Phase4 phase4;
    int info1_received, upstream_rate, uinfo, downstream_rate;
    double probe_energy;
    int probe_samples;
    unsigned retrains,retrain_tone_windows;
    long retrain_mute_until;


} V90Startup;
void v90_info0d(unsigned char bits[62], int alaw);
void v90_info1d(unsigned char bits[109]);
void v90_startup_init(V90Startup *s, int alaw);
int v90_startup_data_retrain(V90Startup *s);
void v90_startup_history(V90Startup *s, const int16_t *in, int n);
void v90_startup_process(V90Startup *s, int16_t *out, const int16_t *in, int n);
#endif
