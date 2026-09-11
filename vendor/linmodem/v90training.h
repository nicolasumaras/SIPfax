#ifndef V90TRAINING_H
#define V90TRAINING_H
#include <stdint.h>
#include "v90dil.h"
#include "v90cp.h"
#define V90_RX_TAPS 81
#define V90_RX_PHASES 10
#define V90_RX_MAX_PHASES 20
typedef struct {
    double previous_re,previous_im;
    int have_previous,ones,count,have_data_cp;
    unsigned scrambler;
    uint8_t bits[V90_JA_MAX_BITS];
} V90JaLane;
typedef struct {
    double taps[4][V90_RX_TAPS],re[V90_RX_TAPS],im[V90_RX_TAPS];
    unsigned position;
    long samples;
    unsigned symbol_rate,phase_count;
    double carrier,symbol_period,next_symbol[V90_RX_MAX_PHASES];
    double filtered_re[32],filtered_im[32];
    int found,cp_mode,e_seen;
    V90Cp cp;
    V90JaLane lanes[V90_RX_MAX_PHASES];
    V90Dil dil;
} V90Training;
void v90_training_init(V90Training *s);
/* 3000/3200 baud and high/low carrier; invalid profiles leave state unchanged. */
int v90_training_init_profile(V90Training *,unsigned symbol_rate,unsigned high_carrier);
int v90_training_receive(V90Training *s,const int16_t *pcm,int count);
/* S has coherent lines at fc and fc +/- baud/2. This detector is for
 * the currently negotiated 3200/high-carrier mode. Events: 1=S, 2=Sbar. */
typedef struct {
    unsigned samples,good,bad,latched,reversed;
    double re[3],im[3],energy,short_re,short_im,ref_re,ref_im;
    double normalized_re[3],normalized_im[3],block_energy;
} V90SDetect;
int v90_s_detect(V90SDetect *s,int16_t sample);
#endif
