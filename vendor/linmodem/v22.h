
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
    double tphase;                   /* fractional timing offset in samples */
} V22DemodState;

extern s16 v22_tx_filter[V22_TX_FILTER_SIZE];

void V22_mod_init(V22ModState *s);
void V22_mod(V22ModState *s, s16 *samples, unsigned int nb);

void V22_test(void);
