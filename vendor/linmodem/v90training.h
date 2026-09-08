#ifndef V90TRAINING_H
#define V90TRAINING_H
#include <stdint.h>
#include "v90dil.h"
#define V90_RX_TAPS 81
typedef struct {
    double previous_re,previous_im;
    int have_previous,ones,count;
    unsigned scrambler;
    uint8_t bits[V90_JA_MAX_BITS];
} V90JaLane;
typedef struct {
    double taps[V90_RX_TAPS],re[V90_RX_TAPS],im[V90_RX_TAPS];
    double last_re,last_im;
    unsigned position;
    long samples;
    int found;
    V90JaLane lanes[5];
    V90Dil dil;
} V90Training;
void v90_training_init(V90Training *s);
int v90_training_receive(V90Training *s,const int16_t *pcm,int count);
/* S has coherent lines at fc and fc +/- baud/2. This detector is for
 * the currently negotiated 3200/high-carrier mode. Events: 1=S, 2=Sbar. */
typedef struct {
    unsigned samples,good,bad,latched,reversed;
    double re[3],im[3],energy,short_re,short_im,ref_re,ref_im;
} V90SDetect;
int v90_s_detect(V90SDetect *s,int16_t sample);
#endif
