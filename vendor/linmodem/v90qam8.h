/* 7200/9600/12000/14400/16800/19200/21600/24000/26400/28800/31200 at 3200 symbols/s mapping frames after symbol/trellis decoding. GPL-2.0. */
#ifndef V90QAM8_H
#define V90QAM8_H
#include "v90shell.h"
#include "v90mapping.h"
#include "v90trellis.h"
#include "v90equalizer.h"
typedef struct { V90Shell shell; unsigned previous,q; } V90Qam8Frames;
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous_quadrant);
/* Eight time-ordered quadrant|(ring<<2) labels -> 18 scrambled bits.
 * Caller establishes frame alignment and differential state. Rejected frames
 * leave output untouched; valid labels still advance differential history so
 * a bad shell does not corrupt the next frame. Invalid labels reset nothing. */
int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18]);
typedef V90Qam8Frames V90Qam12Frames;
void v90_qam12_frames_init(V90Qam12Frames *s,unsigned previous_quadrant);
/* Same frame contract, but M=3/K=12 and 24 scrambled bits for 9600/3200.
 * Labels are quadrant|(ring<<2), where ring is 0..2. */
int v90_qam12_frame(V90Qam12Frames *s,const uint8_t labels[8],uint8_t bits[24]);
typedef V90Qam8Frames V90Qam20Frames;
void v90_qam20_frames_init(V90Qam20Frames *s,unsigned previous_quadrant);
/* Same frame contract, M=5/K=18, 30 scrambled bits for 12000/3200.
 * This inverse alone does not enable 12000 acquisition or live reception. */
int v90_qam20_frame(V90Qam20Frames *s,const uint8_t labels[8],uint8_t bits[30]);
typedef V90Qam8Frames V90Qam32Frames;
void v90_qam32_frames_init(V90Qam32Frames *s,unsigned previous_quadrant);
/* M=8/K=24, 36 scrambled bits for 14400/3200. All shell tuples are valid.
 * This inverse alone does not enable live 14400 reception. */
int v90_qam32_frame(V90Qam32Frames *s,const uint8_t labels[8],uint8_t bits[36]);
typedef V90Qam8Frames V90Qam56Frames;
void v90_qam56_frames_init(V90Qam56Frames *s,unsigned previous_quadrant);
/* M=14/K=30, 42 scrambled bits for 16800/3200. Unused shell tuples reject.
 * This inverse alone does not enable live 16800 reception. */
int v90_qam56_frame(V90Qam56Frames *s,const uint8_t labels[8],uint8_t bits[42]);
typedef V90Qam8Frames V90Qam96Frames;
void v90_qam96_frames_init(V90Qam96Frames *s,unsigned previous_quadrant);
/* M=12/K=28/q=1: 48 scrambled bits, including uncoded symbol bits in parser
 * order. Labels carry Q=2*ring+uncoded_bit above their two quadrant bits.
 * This inverse alone does not enable live 19200 reception. */
int v90_qam96_frame(V90Qam96Frames *s,const uint8_t labels[8],uint8_t bits[48]);
typedef V90Qam8Frames V90Qam160Frames;
void v90_qam160_frames_init(V90Qam160Frames *s,unsigned previous_quadrant);
/* M=10/K=26/q=2: 54 bits, Q=4*ring+two_uncoded_bits. */
int v90_qam160_frame(V90Qam160Frames *s,const uint8_t labels[8],uint8_t bits[54]);
typedef V90Qam8Frames V90Qam256Frames;
void v90_qam256_frames_init(V90Qam256Frames *s,unsigned previous_quadrant);
/* M=8/K=24/q=3: 60 bits, Q=8*ring+three_uncoded_bits. All byte labels valid. */
int v90_qam256_frame(V90Qam256Frames *s,const uint8_t labels[8],uint8_t bits[60]);
typedef V90Qam8Frames V90Qam448Frames;
void v90_qam448_frames_init(V90Qam448Frames *s,unsigned previous);
int v90_qam448_frame(V90Qam448Frames *s,const uint16_t labels[8],uint8_t bits[66]);
/* 28800/3200 minimum constellation: M=12, K=28, q=4. */
typedef V90Qam8Frames V90Qam768Frames;
void v90_qam768_frames_init(V90Qam768Frames *s,unsigned previous);
int v90_qam768_frame(V90Qam768Frames *s,const uint16_t labels[8],uint8_t bits[72]);
/* 31200/3200 minimum constellation: M=10, K=26, q=5. */
typedef V90Qam8Frames V90Qam1280Frames;
void v90_qam1280_frames_init(V90Qam1280Frames *s,unsigned previous);
int v90_qam1280_frame(V90Qam1280Frames *s,const uint16_t labels[8],uint8_t bits[78]);
#define V90_QAM8_B1_SYMBOLS 128
typedef struct {
    uint16_t labels[V90_QAM8_B1_SYMBOLS];
    double re[V90_QAM8_B1_SYMBOLS],im[V90_QAM8_B1_SYMBOLS];
    double reference_energy;
    unsigned position,count,m,k,q,length;
} V90Qam8B1;
void v90_qam8_b1_init(V90Qam8B1 *s);
/* 7200/9600/12000/14400/16800/19200/21600/24000/26400/28800/31200 at 3200 symbols/s; invalid rate clears state and returns 0. */
int v90_qam_b1_init_rate(V90Qam8B1 *s,unsigned rate);
/* Explicit 3000/3200 profile, including 4800 bit/s. Initializes acquisition
 * only; does not enable that profile in the streaming PCM/data receiver.
 * Invalid profiles clear state. Observation buffers remain bounded at 128. */
int v90_qam_b1_init_profile(V90Qam8B1 *s,unsigned rate,unsigned symbol_rate);
/* Feed symbol-spaced matched-filter output. On a match, gain/phase describe
 * received = gain * exp(j*phase) * reference; score is normalized correlation.
 * This establishes the B1 end at this symbol, without tracking later data.
 * Nonfinite/oversized input clears the observation window. Outputs are only
 * written on a match. Caller must select the correct symbol timing lane. */
int v90_qam8_b1_symbol(V90Qam8B1 *s,double re,double im,
                      double *gain,double *phase,double *score);
#define V90_QAM_FEEDBACK_AGE 8
#define V90_QAM_FEEDBACK_HISTORY 32
#if V90_QAM_FEEDBACK_HISTORY <= 2*V90_QAM_FEEDBACK_AGE+1
#error Feedback history must retain the provisional pair
#endif
typedef struct {
    V90Qam8B1 b1;
    V90Mapping mapping;
    unsigned label_bits,frame_bits;
    /* Optional half-symbol input, supplied before each symbol by the PCM
     * frontend. B1 history is aligned with b1.position. */
    double mid_re,mid_im,mid_b1_re[128],mid_b1_im[128];
    unsigned have_mid;
    V90Trellis trellis;
    V90Qam8Frames frames;
    V90Carrier carrier;
    V90Equalizer equalizer;
    double history_re[V90_QAM_FEEDBACK_HISTORY][V90_EQ_MAX_TAPS],history_im[V90_QAM_FEEDBACK_HISTORY][V90_EQ_MAX_TAPS];
    uint64_t symbols,origin,pairs,output_symbol,output_frames,rejected_frames;
    double score,a_re,a_im;
    uint16_t labels[8];
    unsigned locked,have_a,count;
    void *opaque;
    /* Includes B1, starting with output_frames=1. NULL bits denotes an
     * invalid shell; framing/descrambling consumers must treat it as erasure.
     * output_symbol identifies the final symbol of the decoded frame.
     * frame_bits gives the current length, including low/high switching. */
    void (*receive_bits)(void *,const uint8_t *bits);
} V90Qam8Stream;
void v90_qam8_stream_init(V90Qam8Stream *s);
int v90_qam_stream_init_rate(V90Qam8Stream *s,unsigned rate);
/* Symbol-spaced profile receiver. This does not configure the PCM frontend.
 * 3000 permits 4800..28800, 3200 permits 4800..31200 in 2400 increments.
 * receive_bits uses frame_bits (set before each callback), including erasures. */
int v90_qam_stream_init_profile(V90Qam8Stream *,unsigned rate,unsigned symbol_rate);
/* Returns -1 on invalid input/lost lock, 0 while acquiring, 1 when locked.
 * Acquisition replays B1 so downstream descrambling can start from zero.
 * Decision-directed carrier/gain tracking; symbol timing is supplied by caller.
 * Automatic reacquisition requires another B1; no blind data acquisition. */
int v90_qam8_stream_symbol(V90Qam8Stream *s,double re,double im);
#endif
