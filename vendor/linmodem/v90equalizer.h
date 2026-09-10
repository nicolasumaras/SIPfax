/* Experimental symbol-spaced complex equalizer. GPL-2.0. */
#ifndef V90EQUALIZER_H
#define V90EQUALIZER_H
#define V90_EQ_TAPS 7
#define V90_EQ_DELAY 3
typedef struct {
    double cr[V90_EQ_TAPS],ci[V90_EQ_TAPS];
    double re[V90_EQ_TAPS],im[V90_EQ_TAPS];
    unsigned position;
    unsigned long long samples;
} V90Equalizer;
void v90_equalizer_init(V90Equalizer *s);
/* Normalized, carrier-aligned B1 observations and known reference symbols.
 * Fit on 80 interior symbols and validate on the remaining interior symbols.
 * A rejected fit leaves the complete existing state unchanged. Inputs must
 * contain exactly 128 symbols. No automatic decision-directed adaptation. */
int v90_equalizer_train(V90Equalizer *s,const double *re,const double *im,
                       const double *target_re,const double *target_im);
/* Seven-tap FIR with three symbols of lookahead. Returns 0 while filling,
 * 1 with the next output, -1 without changing state for invalid input.
 * The first output represents input symbol 0, with zero-padded prehistory. */
int v90_equalizer_symbol(V90Equalizer *s,double re,double im,
                        double *out_re,double *out_im);
#endif
