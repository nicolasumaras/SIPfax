#ifndef V34PRIV_H
#define V34PRIV_H

#define MAX_MAPPING_FRAME_SIZE 79
#define M_MAX 18

/* symbol rate */
enum {
  V34_S2400,
  V34_S2743,
  V34_S2800,
  V34_S3000,
  V34_S3200,
  V34_S3429,
};

/* constellation parameters */
#define L_MAX 1664   /* max number of points in the constellation */
#define C_MIN   -11
#define C_MAX   11
#define C_MAX_SIZE ((C_MAX-C_MIN+1)*(C_MAX-C_MIN+1))
#define C_RADIUS (2*(C_MAX-C_MIN)+1) /* max coordinate of the constellation */
#define SYNC_PATTERN 0x77FA /* (table 12) synchronisation pattern for J=8 */

#define V34_SAMPLE_RATE_NUM 10
#define V34_SAMPLE_RATE_DEN 3
#define V34_SAMPLE_RATE ((2400*V34_SAMPLE_RATE_NUM)/V34_SAMPLE_RATE_DEN)

/* size of the raised root cosine filter (for both rx & tx) */
#define RC_FILTER_SIZE 40

#define TX_BUF_SIZE (2048)

#define RX_BUF1_SIZE   256
#define RX_BUF2_SIZE   256

/* size of the complex equalizer filter */
#define EQ_FRAC        3
#define EQ_SIZE        (52*EQ_FRAC)

#define AGC_WINDOW_SIZE 512 /* must be a power of two, in input samples */

#define TRELLIS_MAX_STATES 64
/* 5 times the constraint length */
#define TRELLIS_LENGTH (6*5)

/* 10 fractional bits for nyquist filters */
#define NQ_BITS 10
#define NQ_BASE (1 << NQ_BITS)

/* state of the signal processing part of the V34 transmitter */
typedef struct V34DSPState {
  /* V34 parameters */
  int calling; /* true if we are the caller */ 
  int S; /* index for symbol rate */
  int expanded_shape; /* true if expanded shape used */
  int R; /* transmit rate (in bits/s, including aux channel) */
  int conv_nb_states; /* number of states of the convolutional coder */
  int use_non_linear;
  int use_high_carrier;
  s16 h[3][2]; /* precoding coefficients (14 bits fractional part) */

    void *opaque;
    get_bit_func get_bit;  
    
    put_bit_func put_bit;  
  
  /* do not modify after this */

  int N; /* total number of bits in a data frame */
  int W; /* number of aux bits in a data frame (0 = no aux channel) */
  int J; /* number of data frame in a super frame */
  int P; /* number of mapping frame in a data frame */
  int b; /* max length of a mapping frame */
  int r; /* counter to know the length of the mapping frame */
  int K; /* mapping parameters */
  int q;
  int L; /* current number of points of the constellation */
  int M; /* current number of rings */ 
  int Z_1; /* previous Z value (see § 9.5) */

    int mapping_frame; /* number of the mapping frame */
    int rcnt, acnt;    /* fractional counters to know the number of bits
                          in a mapping frame */
  int half_data_frame_count;    /* number of half data frame */
  int sync_count; /* counter mod 2P for synchronisation */
  s16 x[3][2]; /* 3 most recent samples for precoding (7 bit fractional part) */
  int U0;
  int conv_reg; /* memory of the convolutional coder */
  int scrambler_reg; /* state of the self synchronizing scrambler */
  float carrier_freq; 
  float symbol_rate; 
    s8 constellation[C_MAX_SIZE][2];

    /* precomputed bases for the ring computation */
    int g2_tab[8*(M_MAX - 1) + 1];
    int g4_tab[8*(M_MAX - 1) + 1];
    int g8_tab[8*(M_MAX - 1) + 1];
    int z8_tab[8*(M_MAX - 1) + 1];
    
    /* for decoding only */
    u16 constellation_to_code[C_RADIUS+1][C_RADIUS+1];

    /* for encoding only */
    s16 *tx_filter;
    s16 tx_buf[TX_BUF_SIZE][2];
    int tx_buf_ptr, tx_outbuf_ptr, tx_buf_size;
    int tx_filter_wsize;
    int baud_num, baud_denom;
    int baud_incr;
    int baud_phase;
    int carrier_phase;
    int carrier_incr;

    s16 tx_amp; /* amplitude for transmit : each symbol is multiplied
                   by it (1:8:7) */

    int baud3_phase;
    s16 *rx_filter;
    int rx_filter_wsize;
    s16 rx_buf1[RX_BUF1_SIZE];
    int rx_buf1_ptr;
    
    /* symbol synchronization */
    s16 sync_low_mem[2];
    s16 sync_low_coef[2];
    s16 sync_high_mem[2];
    s16 sync_high_coef[2];
    s16 sync_A, sync_B, sync_C;

    /* equalizer */
    s32 eq_filter[EQ_SIZE][2];
    s16 eq_buf[EQ_SIZE];
    int eq_buf_ptr;
    int eq_shift;

    /* AGC */
    float agc_mem;
    float agc_coef;
    int agc_power;
    int agc_gain;
    
    /* Viterbi decoder */

    /* the previous decoded decision comming to this path. Each
       decision Y[5] is coded on one byte */

    s16 state_decision[TRELLIS_MAX_STATES][TRELLIS_LENGTH]; /* SIPFAX: was u8; holds 0..511 */
    u8  state_path[TRELLIS_MAX_STATES][TRELLIS_LENGTH];
    s16 state_memory[TRELLIS_LENGTH][4];
    u8  u0_memory[TRELLIS_LENGTH];
    int state_error[TRELLIS_MAX_STATES];
    int state_error1[TRELLIS_MAX_STATES];
    int trellis_ptr;

    /* decoder synchronization */
    int phase_4d; /* index of the current 2d symbol in the 4D symbol
                     (0 or 1) */
    int phase_mse; /* MSE to find if we are synchronized on a 4D symbol */
    int phase_mse_cnt;

    s16 yy[2][2]; /* current 4D symbol */
    s16 rx_mapping_frame[2*4][2]; 
    int rx_mapping_frame_count;

    /* rx state */
    int sym_count;

    /* current V34 protocol state */
    int state;
    int is_16states;
    int mp_16point;
    int trnref_state;        /* SIPFAX: 0=off 1=aligning 2=locked */
    unsigned int trnref_reg; /* local TRN scrambler, starts at 0 (10.1.3.6) */
    int trnref_n, trnref_d;
    double trnref_ri[24], trnref_rq[24];   /* reference symbols (ring) */
    double trnref_yi[24], trnref_yq[24];   /* received symbols (ring) */
    double rx16_rms;   /* SIPFAX: running mean power, for 16-point slicing */
    int rx16_z;        /* SIPFAX: previous z, for differential MP */
    int mp_hold;   /* SIPFAX: MP(ack=0) frames still to send with settled params */
    unsigned long long jph_hist[8]; /* SIPFAX: J' phase scorers */
    int jp_hunt;                    /* SIPFAX: J seen, waiting for J' */
    long jp_since;                  /* SIPFAX: symbols since J */ /* SIPFAX: MP/J constellation, independent of TRN */ /* 16 states required in the startup sequences */

    /* interaction with receiver */
    int J_received;
    /* streaming CMA/DD + carrier-recovery receiver state (Stage 1c) */
    int cma_init;
    double cma_wi[32], cma_wq[32], cma_bufi[32], cma_bufq[32];
    double cma_dcph, cma_residx, cma_pbi, cma_pbq; long cma_n;
    int cma_t2, cma_cnt, cma_ncma, cma_started;
    double cma_th, cma_freq, cma_pow, cma_erms, cma_symi, cma_symq;
    int cma_q[128]; long cma_qn;
    int cma_trn_hi, cma_trn_lo, cma_warm, cma_skipn;
    /* scrambler-tracking reference receiver (init-free TRN/J) */
    unsigned int srx_reg4[4]; unsigned long long srx_hist4[4];
    unsigned int srx_reg; unsigned long long srx_agree;
    int srx_locked, srx_rot, srx_lowrun;
    int jh_active[8]; unsigned int jh_bitpos[8]; unsigned long long jh_hist[8];
    unsigned int jh_reg[8]; int srx_pqd; unsigned int srx_regD;
    int jh_n, jh_wait;
    double srx_th, srx_s4i, srx_s4q; int srx_blkn;
    /* Phase-4 receiver + TX gating */
    int p4_mode;                 /* 0=phase3, 1=hunt caller P4 TRN, 2=MP collect */
    int p4_trn_syms;             /* caller TRN symbols since re-lock */
    unsigned int p4_ybits;       /* received-bit history for GPC FIR descramble */
    int p4_ones_run, p4_collect, p4_fn;
    u8 p4_frame[224];
    u8 p4_last[64]; int p4_last_valid;
    int p4_mp_rx, p4_mpp_rx, p4_e_rx;
    int p4_mp_rate_ca, p4_mp_rate_ac; unsigned int p4_mp_mask;
    int p4_trn_tx; int p4_mp_hunt_rx;
    int data_on; long data_n; double data_pw_target;  /* SIPFAX: data-mode feed */
    double data_mse_acc; long data_mse_n;             /* SIPFAX: decode-quality metric */
    double data_th, data_frq, data_meanc2;            /* SIPFAX: DD carrier loop */
    int rx_precode;                                  /* SIPFAX: invert 9.6.2 precoding */
    short peer_h[6];   /* SIPFAX: precoder coefficients the PEER asked OUR tx to use */
    double data_agc;   /* SIPFAX: decision-directed data-mode gain */
#define DATA_ACQ_N 2000
    int data_acq_done, data_acq_n;                   /* SIPFAX: data-mode acquisition */
    double data_acq_i[DATA_ACQ_N], data_acq_q[DATA_ACQ_N];
    int rx_j16;      /* SIPFAX: caller's J requested 16-point Phase 4 from US (0x0D91) */
    int jvar_wait, jvar_phase, jvar_c4, jvar_c16;  /* SIPFAX: J-variant vote in progress */
    int p4_key, p4_keyn, p4_mp_crcok, p4_trellis;
    /* SIPFAX: MP fold ring. Was 4096 bits, which at L=188 allowed a majority vote over
       only 8 repetitions - not enough to get a CRC-clean frame through the hybrid-echo
       BER on a real line, so MP was accepted on "consensus of the reliable head fields"
       instead and the PRECODER COEFFICIENTS, which the fold path does not even read, were
       never recovered. MP repeats continuously through Phase 4, so more repetitions cost
       nothing but memory. */
#define P4_RING_SZ 32768
#define P4_RING_MASK (P4_RING_SZ - 1)
    u8 p4_ring[P4_RING_SZ]; int p4_rn; int p4_try;
    int peer_nonlin;   /* SIPFAX: MP bit 31 - peer requests the 9.7 non-linear encoder */
    double cma_mfi[64], cma_mfq[64]; int cma_mfp;
    long cma_m; int cma_cphi;
    /* SIPFAX: tracked symbol clock for the LIVE receiver. The 7:6 resampler below used
       to be an exact integer grid (q=7m, ip=q/6) - rigid by construction and locked to
       OUR sample counter, so it could not follow the far end's clock at all. cma_pos is
       the position of the next T/2 output in input samples and cma_tinc the tracked rate
       correction; cma_h* is the short post-matched-filter history the interpolator needs
       once the grid is no longer integer. cma_g* hold Gardner's y(n-1/2) and y(n-1). */
    double cma_pos, cma_tinc, cma_hi[8], cma_hq[8];
    /* SIPFAX: 3x-oversampled front end. At 24 kHz the 3429 baud symbol rate divides
       EXACTLY - 7.0 samples/symbol - so the symbol grid is integer and T/2 is 3.5 samples,
       against 2.3333 samples/symbol at 8 kHz where every output needs interpolation on a
       barely-oversampled grid. This mirrors p4_front(), the block receiver that reaches
       3.4-4.7% EVM on TRN. */
    double rx3_i[512], rx3_q[512];      /* filtered complex baseband at 24 kHz */
    long   rx3_n;                       /* 24 kHz sample counter */
    int    rx3_cphi;                    /* downconversion phase, 4/49 per 24 kHz sample */
    double rx3_pos;                     /* next T/2 output, in 24 kHz samples */
    double rx3_bi[512], rx3_bq[512];    /* pre-filter downconverted ring */
    double rx3_last;                    /* last cma_pos seen, to carry Gardner across */
    /* SIPFAX: running average of the equaliser taps. Measured on a signal with NO channel,
       where the ideal equaliser is exactly a delta, the frozen taps still carry 3.2% error
       energy - a 15 dB ceiling before the decoder sees anything. That is CMA steady-state
       misadjustment at mu=2e-3, and freezing captures the INSTANTANEOUS tap noise. */
    double cma_ai[32], cma_aq[32]; long cma_an;   /* 32 == CMANT, which v34.c defines */
    double cma_gmi, cma_gmq, cma_gpi, cma_gpq;
    int cma_phase, cma_phn; double cma_c4i, cma_c4q, cma_pu4i, cma_pu4q, cma_sq;

    int dbg_last; long dbg_n; int dbg_last2;
    int trn_rounds;        /* answer TRN repetitions before J */
    int JP_received;
} V34DSPState;

u8 trellis_trans_4[256][4];
u8 trellis_trans_8[256][4];
u8 trellis_trans_16[256][4];

/* V34 states */
enum {
    V34_STARTUP3_S1,
    V34_STARTUP3_SINV1,
    V34_STARTUP3_S2,
    V34_STARTUP3_SINV2,
    V34_STARTUP3_PP,
    V34_STARTUP3_TRN,
    V34_STARTUP3_J,
    V34_STARTUP3_JP,
    V34_STARTUP3_WAIT_J,

    V34_STARTUP4_S,
    V34_STARTUP4_WAIT_JP,
    V34_STARTUP4_SINV,
    V34_STARTUP4_TRN,
    V34_STARTUP4_MP,
    V34_STARTUP4_MPP,
    V34_STARTUP4_E,
    V34_DATA,

    /* receive only */
    V34_STARTUP3_WAIT_S1,
};

void put_bits(u8 **pp, int n, int bits);
int calc_crc(u8 *buf, int size);
void v34_send_info0(V34DSPState *s, int ack);

#define DSPK_TX_FILTER_SIZE 321
extern s16 v34_dpsk_tx_filter[DSPK_TX_FILTER_SIZE];

extern s16 v34_rc_5_filter[];
extern s16 v34_rc_7_filter[];
extern s16 v34_rc_8_filter[];
extern s16 v34_rc_10_filter[];
extern s16 v34_rc_20_filter[];
extern s16 v34_rc_35_filter[];

extern s16 v34_rx_filter_2400_1600[];
extern s16 v34_rx_filter_2400_1800[];
extern s16 v34_rx_filter_2743_1646[];
extern s16 v34_rx_filter_2743_1829[];
extern s16 v34_rx_filter_2800_1680[];
extern s16 v34_rx_filter_2800_1867[];
extern s16 v34_rx_filter_3000_1800[];
extern s16 v34_rx_filter_3000_2000[];
extern s16 v34_rx_filter_3200_1829[];
extern s16 v34_rx_filter_3200_1920[];
extern s16 v34_rx_filter_3429_1959[];

/* v34eq.c */
typedef struct {
    s16 re, im;
} icomplex;

#define V34_PP_SIZE 48
extern icomplex tabPP[V34_PP_SIZE];

void V34eq_init(void);
void V34_fast_equalize(V34DSPState *s, s16 *input);

typedef struct V34State {
    /* V34 parameters test */
    int calling; /* true if we are the caller */
    int S; /* index for symbol rate */
    int expanded_shape; /* true if expanded shape used */
    int R; /* transmit rate (in bits/s, excluding aux channel) */
    int conv_nb_states; /* number of states of the convolutional coder */
    int use_non_linear;
    int use_high_carrier;
    int use_aux_channel;
    s16 h[3][2]; /* precoding coefficients (14 bits fractional part) */

    V34DSPState v34_tx;
    V34DSPState v34_rx;
    void *phase2;
    long p3n; s16 *p3rep; long p3rep_len; long p3rep_ptr;
    double p3x1; double p3h[31];
    int p3go;              /* caller joined Phase 3 (resume TX) */           /* pre-emphasis filter memory */              /* samples since Phase-3 handoff (turn-taking window) */        /* V.34 Phase-2 answer SM (opaque) */
    int phase2_active;
} V34State;

#endif

