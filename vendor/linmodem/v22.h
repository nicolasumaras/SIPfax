
enum ModulationType {
    V34_MOD_600,
    V22_MOD_600,
    V22_MOD_1200,
    V22_MOD_2400, 
};

/* 40 phases (sure too much, but we don't optimize right now) */
#define V22_TX_FILTER_SIZE (20 * 40)
#define V22_TX_BUF_SIZE    64

typedef struct {
    /* parameters */
    int calling;
    enum ModulationType mod_type;         
    void *opaque;
    get_bit_func get_bit;

    /* state */
    int baud_phase, baud_num, baud_denom;
    int carrier_phase, carrier_incr;
    int carrier2_phase, carrier2_incr;
    int tx_filter_wsize;
    s16 tx_buf[V22_TX_BUF_SIZE][2];  /* complex symbols to be sent */
    int tx_outbuf_ptr;               /* index of the next symbol in tx_buf */
    int Z;              /* last value transmitted */
    double guard_gain;  /* 1800 Hz guard tone amplitude, answer side (SIPFAX_V22_GUARD) */
} V22ModState;

#define V22_RX_BUF_SIZE 32   /* SIPFAX: complex baseband history, power of two */

typedef struct {
    /* parameters */
    int calling;
    enum ModulationType mod_type;
    void *opaque;
    put_bit_func put_bit;

    int baud_phase, baud_num, baud_denom;
    int carrier_phase, carrier_incr;

    /* SIPFAX: receive state. V22_demod was an empty function - the whole V.22
       receiver is new. */
    int rx_buf[V22_RX_BUF_SIZE][2];  /* downconverted baseband history */
    int rx_ptr;
    int Z;                           /* previous quadrant, for differential decode */
    int started;                     /* seen enough signal to start slicing */
    long sym_count;
    double agc;                      /* running amplitude estimate */
    double ted_i, ted_q;             /* previous symbol, for Gardner */
    double mid_i, mid_q;             /* mid-symbol sample */
    double tphase;                   /* fractional position within the half-symbol */
    double step;                     /* samples per half symbol, adjusted by the loop */
    int    nwrap;                    /* timing-loop sample wraps, for diagnosis */
    double cph, cacc;                /* carrier phase correction and its integrator */
    double tadj;                     /* fractional sampling delay, steered by Gardner */
    double lp_i, lp_q;               /* image-rejection box output */
    int    bx_i[8], bx_q[8], bx_p;   /* 7-sample box history */
    double h_i[4], h_q[4];           /* recent baseband, for interpolation */
    int    half;                     /* 0 = symbol instant, 1 = midpoint */
    double pi_, pq_;                 /* previous symbol instant */
    double mi_, mq_;                 /* previous midpoint */
    double ted_acc;                  /* integral term */
} V22DemodState;

extern s16 v22_tx_filter[V22_TX_FILTER_SIZE];

void V22_mod_init(V22ModState *s);
void V22_mod(V22ModState *s, s16 *samples, unsigned int nb);

void V22_test(void);


/* SIPFAX: V.22 / V.22bis HANDSHAKE.

   linmodem had no V.22 handshake at all - only V22_mod and V22_demod. This is the
   ANSWER side, plus enough of the calling side to test it without placing a call.

   Entry is from V.8, not V.25: every call reaches us through ANSam/CM/JM/CJ, and
   V.8 S8.2.3 replaces the V.25 answer tone with "no signal for 75 +/- 5 ms, followed
   by sigA for the selected modulation". V8_SIGA already implements that silence, so
   this machine must transmit from its very first sample and must never emit 2100 Hz.

   1200 bit/s ONLY, deliberately. We never transmit S1, so the caller falls back to
   1200 on its own (V.22bis S6.3.1.1.1 c): a caller that sees scrambled binary 1 at
   1200 instead of S1 continues at 1200). That is not a workaround for a rate cap - it
   is because V22_demod does not yet decode 2400, so claiming 2400 would negotiate a
   rate we cannot receive. */

/* S6.3.1.1.2 b) qualifies over 270 +/- 40 ms, i.e. 230-310 ms. The window is not the
   whole story: the demodulator adds ~40 ms of its own latency before a bit is scored, so
   a 336-bit (280 ms) window declared at 320 ms measured end to end - just outside. 300
   bits lands the measured figure near 290 ms, inside the window with margin either way. */
#define V22_DETW  300   /* 250 ms at 1200 bit/s */
#define V22_DETN  285   /* 95% */

enum {
    V22_ANS_USB1,   /* TX unscrambled binary 1; await the caller's scrambled 1s/0s */
    V22_ANS_S11,    /* TX scrambled binary 1 for 765 ms (S6.3.1.2.2 c) */
    V22_ANS_DATA,   /* connected, 1200 bit/s */
    V22_ANS_FAIL,
};

enum {
    V22_CALL_WAIT,  /* silent until the answer modem's signal appears */
    V22_CALL_USB1,
    V22_CALL_S11,
    V22_CALL_DATA,
};

typedef struct {
    int state;
    long bits;                  /* bits emitted since entering this state */
    unsigned int sreg, dreg;    /* scrambler / descrambler, 1 + x^-14 + x^-17 */
    int ones_out;               /* consecutive 1s at the scrambler output */
    int raw_ones;               /* consecutive identical raw bits (unscrambled-1 detect) */
    int raw_last;
    unsigned char hist[V22_DETW];
    int hp, nones, nzeros, nraw, nfill;
    long total_bits;
    void *opaque;
    get_bit_func get_bit;       /* upper layer, used once in DATA */
    put_bit_func put_bit;
} V22HsState;

typedef struct {
    V22ModState   mod;
    V22DemodState demod;
    V22HsState    hs;
    int silent;                 /* gate the transmitter (calling side, before start) */
} V22Session;

void V22_answer_init(V22Session *s, get_bit_func gb, put_bit_func pb, void *opaque);
void V22_calling_init(V22Session *s, get_bit_func gb, put_bit_func pb, void *opaque);
void V22_session(V22Session *s, s16 *out, s16 *in, unsigned int nb);
int  V22_session_state(V22Session *s);
const char *V22_state_name(V22Session *s);
void V22_hs_test(void);
