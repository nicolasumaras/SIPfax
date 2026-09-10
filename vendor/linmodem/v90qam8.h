/* 7200/3200 mapping frames after symbol/trellis decoding. GPL-2.0. */
#ifndef V90QAM8_H
#define V90QAM8_H
#include "v90shell.h"
#include "v90trellis.h"
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
typedef struct {
    V90Qam8B1 b1;
    V90Trellis trellis;
    V90Qam8Frames frames;
    V90Carrier carrier;
    uint64_t symbols,origin,pairs,output_symbol,output_frames,rejected_frames;
    double score,a_re,a_im;
    uint8_t labels[8];
    unsigned locked,have_a,count;
    void *opaque;
    /* Includes B1, starting with output_frames=1. NULL bits denotes an
     * invalid shell; framing/descrambling consumers must treat it as erasure.
     * output_symbol identifies the final symbol of the decoded frame. */
    void (*receive_bits)(void *,const uint8_t *bits);
} V90Qam8Stream;
void v90_qam8_stream_init(V90Qam8Stream *s);
/* Returns -1 on invalid input/lost lock, 0 while acquiring, 1 when locked.
 * Acquisition replays B1 so downstream descrambling can start from zero.
 * Decision-directed carrier/gain tracking; symbol timing is supplied by caller.
 * Automatic reacquisition requires another B1; no blind data acquisition. */
int v90_qam8_stream_symbol(V90Qam8Stream *s,double re,double im);
#endif
