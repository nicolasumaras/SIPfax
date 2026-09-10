/* 7200/3200 mapping frames after symbol/trellis decoding. GPL-2.0. */
#ifndef V90QAM8_H
#define V90QAM8_H
#include "v90shell.h"
typedef struct { V90Shell shell; unsigned previous; } V90Qam8Frames;
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous_quadrant);
/* Eight time-ordered quadrant|(ring<<2) labels -> 18 scrambled bits.
 * Caller establishes frame alignment and differential state. Rejected frames
 * leave output untouched; valid labels still advance differential history so
 * a bad shell does not corrupt the next frame. Invalid labels reset nothing. */
int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18]);
#define V90_QAM8_B1_SYMBOLS 128
typedef struct {
    uint8_t labels[V90_QAM8_B1_SYMBOLS];
    double re[V90_QAM8_B1_SYMBOLS],im[V90_QAM8_B1_SYMBOLS];
    double reference_energy;
    unsigned position,count;
} V90Qam8B1;
void v90_qam8_b1_init(V90Qam8B1 *s);
/* Feed symbol-spaced matched-filter output. On a match, gain/phase describe
 * received = gain * exp(j*phase) * reference; score is normalized correlation.
 * This establishes the B1 end at this symbol, without tracking later data.
 * Nonfinite/oversized input clears the observation window. Outputs are only
 * written on a match. Caller must select the correct symbol timing lane. */
int v90_qam8_b1_symbol(V90Qam8B1 *s,double re,double im,
                      double *gain,double *phase,double *score);
#endif
