/* Experimental baud/half-symbol-spaced complex equalizer. GPL-2.0. */
#ifndef V90EQUALIZER_H
#define V90EQUALIZER_H
#define V90_EQ_TAPS 7
#define V90_EQ_LONG_TAPS 15
#define V90_EQ_HALF_TAPS 29
#define V90_EQ_MAX_TAPS V90_EQ_HALF_TAPS
#define V90_EQ_DELAY 3
typedef struct {
    double cr[V90_EQ_MAX_TAPS],ci[V90_EQ_MAX_TAPS];
    double re[V90_EQ_MAX_TAPS],im[V90_EQ_MAX_TAPS];
    unsigned position,taps;
    unsigned long long samples;
} V90Equalizer;
void v90_equalizer_init(V90Equalizer *s);
/* Select seven/fifteen baud-spaced or twenty-nine half-symbol-spaced taps. Invalid lengths leave state unchanged. */
int v90_equalizer_init_taps(V90Equalizer *s,unsigned taps);
/* Normalized, carrier-aligned B1 observations and known reference symbols.
 * Fit on 80 interior symbols and validate on the remaining interior symbols.
 * A rejected fit leaves the complete existing state unchanged. Inputs must
 * contain exactly 128 symbols; only seven/fifteen-tap states are accepted. No automatic decision-directed adaptation. */
int v90_equalizer_train(V90Equalizer *s,const double *re,const double *im,
                       const double *target_re,const double *target_im);
/* FIR with (taps-1)/2 input samples of lookahead. Returns 0 while filling,
 * 1 with the next output, -1 without changing state for invalid input.
 * The first output represents input sample 0, with zero-padded prehistory. */
int v90_equalizer_symbol(V90Equalizer *s,double re,double im,
                        double *out_re,double *out_im);
/* Normalized LMS update for the most recent output. Caller supplies a known
 * training symbol or a sufficiently confident decision. step is in (0,.1]
 * for baud-spaced states and (0,.2] for the half-symbol-spaced state.
 * Returns 0 without changing state for invalid input or excessive tap norm. */
int v90_equalizer_adapt(V90Equalizer *s,double target_re,double target_im,double step);
/* Twenty-nine taps at two samples per symbol. re/im contain 256 samples,
 * ordered midpoint-before-symbol, symbol; targets contain 128 symbols.
 * Same 80-symbol fit/held-out validation and state-preserving rejection.
 * Feed both samples through symbol(), consuming only symbol-time outputs.
 * Those outputs start at symbol zero after seven symbols of lookahead. */
int v90_equalizer_train_half(V90Equalizer *,const double *,const double *,const double *,const double *);
/* Explicit B1 length: symbols must be 120 (3000 baud) or 128 (3200 baud).
 * stride=1 requires 7/15 taps; stride=2 requires 29 taps and midpoint/symbol
 * ordering. Observations contain symbols*stride values, targets symbols.
 * The 80-symbol fit and held-out validation preserve the legacy 128 behavior.
 * Unsupported lengths/modes reject before reading arrays; state is unchanged. */
int v90_equalizer_train_symbols(V90Equalizer *,const double *,const double *,
    const double *,const double *,unsigned symbols,unsigned stride);
#endif
