/* Experimental symbol-spaced complex equalizer. GPL-2.0. */
#ifndef V90EQUALIZER_H
#define V90EQUALIZER_H
#define V90_EQ_TAPS 7
#define V90_EQ_MAX_TAPS 15
#define V90_EQ_DELAY 3
typedef struct {
    double cr[V90_EQ_MAX_TAPS],ci[V90_EQ_MAX_TAPS];
    double re[V90_EQ_MAX_TAPS],im[V90_EQ_MAX_TAPS];
    unsigned position,taps;
    unsigned long long samples;
} V90Equalizer;
void v90_equalizer_init(V90Equalizer *s);
/* Select seven or fifteen taps. Invalid lengths leave state unchanged. */
int v90_equalizer_init_taps(V90Equalizer *s,unsigned taps);
/* Normalized, carrier-aligned B1 observations and known reference symbols.
 * Fit on 80 interior symbols and validate on the remaining interior symbols.
 * A rejected fit leaves the complete existing state unchanged. Inputs must
 * contain exactly 128 symbols. No automatic decision-directed adaptation. */
int v90_equalizer_train(V90Equalizer *s,const double *re,const double *im,
                       const double *target_re,const double *target_im);
/* FIR with (taps-1)/2 symbols of lookahead. Returns 0 while filling,
 * 1 with the next output, -1 without changing state for invalid input.
 * The first output represents input symbol 0, with zero-padded prehistory. */
int v90_equalizer_symbol(V90Equalizer *s,double re,double im,
                        double *out_re,double *out_im);
/* Normalized LMS update for the most recent output. Caller supplies a known
 * training symbol or a sufficiently confident decision. step is in (0,.1].
 * Returns 0 without changing state for invalid input or excessive tap norm. */
int v90_equalizer_adapt(V90Equalizer *s,double target_re,double target_im,double step);
#endif
