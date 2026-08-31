#include <stdlib.h>
#include <time.h>
/* 
 * Implementation of the V34 modulation/demodulation
 * 
 * Copyright (c) 1999,2000 Fabrice Bellard.
 *
 * This code is released under the GNU General Public License version
 * 2. Please read the file COPYING to know the exact terms of the
 * license.
 * 
 * This implementation is totally clean room. It was written by
 * reading the V34 specification and by using basic signal processing
 * knowledge.  
 */

#include "lm.h"
#include "v34priv.h"

#define DEBUG

void print_bits(int val, int n)
{
    int i;

    for(i=n-1;i>=0;i--) {
        putchar('0' + ((val >> i) & 1));
    }
}

void print_bit_vector(char *str, u8 *tab, int n)
{
  int i;
  fprintf(stderr, "%s[%d]= ", str, n);
  for(i=n-1;i>=0;i--) putchar(tab[i] + '0');
  fprintf(stderr, "\n");
}


static void agc_init(V34DSPState *s);
void baseband_decode_impl(V34DSPState *s, int si, int sq);
static void v34_rx_data_params(V34DSPState *s, int R);   /* SIPFAX: data-mode reconfig */
static void v34_tx_data_params(V34DSPState *s, int R);   /* SIPFAX: TX rate from negotiated ac */
static int  data_slice(V34DSPState *s, double xi, double xq, double *di, double *dq);
static double data_lattice_rms(const double *bi, const double *bq, int n,
                               double g, double ct, double st);
static int  p4_block_step(const short *x, int n, int *ca, int *ac, int *trel, int *ack,
                          int *shape, unsigned int *mask, int *sixteen_out, short *hout,
                          int *nlout);
void baseband_decode_pub(V34DSPState *s, int si, int sq);
int v34_dbg = 0;  /* offline decode verbosity */
long g_moob = 0, g_mtot = 0, g_mmax = 0;   /* SIPFAX: out-of-constellation ring indices */
long g_dh[8] = {0}, g_dmiss = 0, g_dn = 0;  /* SIPFAX: decided-coordinate histogram */
/* SIPFAX: mapping-frame bit dumps. The encoder's data[] holds the SCRAMBLED source bits it
   packs into a frame; the decoder's data[] should reproduce them exactly, after which the
   self-synchronising descrambler recovers the source within 23 bits. Comparing the two
   localises the fault to either the frame assembly or everything after it. */
FILE *g_mftx = 0, *g_mfrx = 0;
FILE *g_decf = 0;
int g_v0est = 0, g_v0have = 0; FILE *g_v0f = 0; FILE *g_v0f2 = 0;
long g_y0same = 0, g_y0tot = 0;
long g_cs_tot = 0, g_cs_ok = 0, g_cs_all4 = 0;
long g_z_n=0,g_z_min0=0,g_z_cnt=0,g_z_hist[8]={0};double g_z_minv=0;
int g_ntup=0; long g_az_n=0,g_az_ok=0,g_az_half=0,g_az_reach=0;
int *g_v0orc=0; long g_v0orn=0, g_v0ori=0;
/* SIPFAX: v0 superframe-sync acquisition. v0 is deterministic in
   (sync_count, half_data_frame_count) with sync_count in 0..29 (P=15) and
   half_data_frame_count in 0..15, so the pattern has period 30*16 = 480 4D symbols and
   carries exactly 12 ones (2.500%). Acquisition is therefore a search over one phase in
   [0,480). Measured: v0[t-1] == U0[t] ^ conv_reg[t]&1 exactly, and both are observable -
   U0 as the half of the global minimum (u0_memory) and conv_reg parity as the LSB of the
   traceback endpoint, which tracks the encoder at 100.00% at lag -29. */
#define V0_PER 480
long g_sym = 0, g_v0hn = 0;
/* SIPFAX: u0_memory is a ring of exactly TRELLIS_LENGTH (30) entries and the traceback walks
   back TRELLIS_LENGTH-1 = 29, so its endpoint index k is congruent to trellis_ptr mod 30 -
   u0_memory[k] IS u0_memory[trellis_ptr], the CURRENT symbol's U0, not the one belonging to
   the symbol being emitted. (That is also why reading u0 at k rather than trellis_ptr changed
   nothing measurable.) Keep a deeper history so U0 can be paired with the traceback's own
   time. */
#define U0H 128
int g_u0hist[U0H];
int  g_v0h[V0_PER];
int  g_v0base[V0_PER];
int  g_v0lock = 0, g_v0ph = 0, g_v0margin = 0, g_v0applied = 0;
/* SIPFAX: automatic mapping-frame alignment, replacing the hand-set SIPFAX_DATA_SKIP.
   Two things have to be right and the v0 lock pins both.
   (1) 4D PAIRING. v0 recovery needs consecutive 2D symbols paired the way the encoder paired
       them; measured, v0 locks for every ODD SIPFAX_DATA_SKIP and never for an even one. So a
       failure to lock within the acquisition window means the pairing is off by one 2D symbol
       - drop one and retry.
   (2) FRAME GROUPING. With the pairing right, the locked phase phi and correctness are related
       exactly: phi mod 20 == 19 decodes, everything else does not (measured over skip 1..39
       plus 59: 99.8% and 99.7% at phi 59 and 79, 51-79% at all eighteen other phases). 20 4D
       symbols is the rcnt cycle, P/gcd(r,P) = 5 mapping frames of 4. Dropping one 4D symbol
       advances phi by one, so the correction is d = (19 - phi) mod 20 4D symbols. Verified
       against the sweep: phi 51 -> d 8 -> skip 3+16 = 19; phi 58 -> d 1 -> 17+2 = 19;
       phi 60 -> d 19 -> 21+38 = 59. */
int  g_v0_pairtry = 0, g_v0_pairdrop = 0, g_v0_aligned = 0;
long g_v0_realign = 0;
static void v0_base_init(void)
{
    int k; static int done = 0;
    if (done) return; done = 1;
    {   /* SIPFAX: DETECTOR-SIDE ONLY overrides, so the convention can be bisected against a
           real transmitter. The encoder keeps the shipped constant, which is the point: every
           link in the chain from constellation index to sync bit is written twice in this
           file with the same expression, so a wrong-but-consistent convention scores 99.7%
           in loopback and still fails against a real peer. Sweeping these two on a live
           capture is the only way to see it. */
        static unsigned int pat = 0; static int grid = 0;
        if (!pat)  { char *e = getenv("SIPFAX_SYNC_PAT"); pat  = e ? (unsigned)strtoul(e,0,0) : SYNC_PATTERN; }
        if (!grid) { char *e = getenv("SIPFAX_V0GRID");   grid = e ? atoi(e) : 30; if (grid < 1) grid = 30; }
        for (k = 0; k < V0_PER; k++)
            g_v0base[k] = ((k % grid) == 0)
                ? ((pat >> (15 - ((k / grid) % 16))) & 1) : 0;
    }
}
static void v0_try_lock(long need)
{
    int ph, k, best = -1, bestph = 0, second = -1;
    if (g_v0lock || g_v0hn < need) return;
    for (ph = 0; ph < V0_PER; ph++) {
        int sc = 0;
        for (k = 0; k < V0_PER; k++) if (g_v0base[(k + ph) % V0_PER]) sc += g_v0h[k];
        if (sc > best) { second = best; best = sc; bestph = ph; }
        else if (sc > second) second = sc;
    }
    /* The pattern is sparse (12 ones in 480), so score the COINCIDENCE of ones, not overall
       agreement - an all-zero guess already agrees on 97.5% of symbols. The ones sit on the
       k == 0 (mod 30) grid, so a WRONG rotation of SYNC_PATTERN still aligns about 9 of 12:
       with perfect estimates the margin is ~399 vs ~300, a fixed 1.33x that does NOT grow
       with more data because it is structural rather than noise. So require a 1.25x margin
       over the runner-up AND that the winner account for at least 3/4 of every observed one
       (which a wrong rotation cannot, since it predicts ones where none were seen). */
    { int tot = 0, k2; for (k2 = 0; k2 < V0_PER; k2++) tot += g_v0h[k2];
      if (!(best > 0 && best * 4 >= (second > 0 ? second : 1) * 5 && best * 4 >= tot * 3)) {
          /* SIPFAX: a failed lock used to return in silence, so every investigation could
             only report "no lock" and had to guess why. Print what the search already
             computed. The shape of the histogram says which convention is wrong:
               tot ~25% of the symbols and the counts flat  -> the trellis/labelling is
                 wrong and these are not sync bits at all (ground truth gives tot/n ~2.9%);
               tot small and the ones SHARP but on a 60-grid -> the sync bit is once per
                 data frame, not per half frame, so (k % 30) here is wrong;
               tot small, ones sharp on the 30-grid, spelling a word that is not 0x77FA or
                 a rotation of it -> SYNC_PATTERN's value or bit order is wrong. */
          extern int v34_dbg;
          static long lastprint = -1;
          if (v34_dbg && g_v0hn - lastprint >= 480) {
              int j, bg = bestph % 30;
              lastprint = g_v0hn;
              fprintf(stderr, "[v0] no lock at %ld syms: tot=%d (%.1f%%) best=%d second=%d "
                      "phase=%d grid=%d\n", g_v0hn, tot, 100.0*tot/(g_v0hn?g_v0hn:1),
                      best, second, bestph, bg);
              fprintf(stderr, "[v0]   ones on residue %d, 16 slots:", bg);
              for (j = 0; j < 16; j++) fprintf(stderr, " %d", g_v0h[(bg + 30*j) % V0_PER]);
              fprintf(stderr, "\n");
              {   /* where do the ones actually sit modulo 30 and modulo 60? a sharp peak on
                     one residue confirms the grid; a split says the grid is wrong. */
                  int m30[30], m60[60], q;
                  for (q = 0; q < 30; q++) m30[q] = 0;
                  for (q = 0; q < 60; q++) m60[q] = 0;
                  for (q = 0; q < V0_PER; q++) { m30[q % 30] += g_v0h[q]; m60[q % 60] += g_v0h[q]; }
                  fprintf(stderr, "[v0]   ones mod 30:");
                  for (q = 0; q < 30; q++) fprintf(stderr, " %d", m30[q]);
                  fprintf(stderr, "\n[v0]   ones mod 60:");
                  for (q = 0; q < 60; q++) fprintf(stderr, " %d", m60[q]);
                  fprintf(stderr, "\n");
              }
          }
          return;
      }
      g_v0margin = tot; }
    if (1) {
        { int tot2 = 0, k3; for (k3 = 0; k3 < V0_PER; k3++) tot2 += g_v0h[k3];
          g_v0lock = 1; g_v0ph = bestph; g_v0margin = best - second;
          fprintf(stderr, "[v0] LOCKED phase %d after %ld symbols "
                  "(captured %d of %d observed ones; runner-up %d)\n",
                  bestph, g_v0hn, best, tot2, second); }
    }
}
int g_surv_bs = 0;
FILE *g_encf = 0;
FILE *g_survf = 0;
FILE *g_zf = 0;   /* SIPFAX: survivor-path dump */
FILE *g_tblf = 0;
int *g_truest = 0; long g_truen = 0, g_truei = 0;
long g_et_n = 0, g_et_zero = 0; double g_et_min = 0, g_et_max = 0, g_et_sum = 0, g_et_spread = 0;
long g_ss_n = 0, g_ss_tied = 0; double g_ss_spread = 0;
long g_tr_argmin = 0, g_tr_tot = 0, g_tr_rank = 0, g_tr_gap = 0, g_tr_inf = 0;
/* SIPFAX: ride the CONTINUOUSLY TRACKED carrier in data mode instead of a static angle.
   The 4.9% EVM that makes the front end look healthy on Phase-4 TRN is measured on
   pi_/pq_, which are derotated by srx_th - the 4th-power estimator, updated every 64
   symbols with a 90-degree unwrap. Data mode instead derotates the RAW oi/oq by data_th,
   seeded ONCE at entry and never updated (its DD gains default to 0). So the two paths do
   not differ only in constellation: one tracks carrier and the other does not. Measured
   against ground truth, the raw equaliser output has a per-symbol phase error of 96 deg
   stdev, so there IS something for a tracker to remove. */
static int g_data_srx = -1;
static double data_carrier(V34DSPState *s)
{
    if (g_data_srx < 0) { char *e = getenv("SIPFAX_DATA_SRX"); g_data_srx = e ? atoi(e) : 0; }
    return g_data_srx ? (s->srx_th + s->data_th) : s->data_th;
}
double g_tedacc = 0; long g_tedn = 0;   /* SIPFAX: Gardner TED magnitude, a lock indicator */
int v34_symdump[40000]; int v34_symdump_n = 0;  /* equalized quadrant dump */
int v34_softi[40000], v34_softq[40000];  /* soft equalized symbols */
int eq_notrack = 0, eq_freeze = 0;  /* diagnostic knobs */
long v34_ntrn = 0, v34_nj = 0;  /* send-call counters */
static void put_sym(V34DSPState *s, int si, int sq);

/* divide the bit sequence by poly */
#define SCRAMBLER_DEG 23
#define V34_GPC       (1 | (1 << (23-18)))
#define V34_GPA       (1 | (1 << (23-5)))

int scramble_bit(V34DSPState *s, int b, int poly)
{
  int b1, reg;
 
  reg = s->scrambler_reg;

  b1 = (reg >> (SCRAMBLER_DEG-1)) ^ b;
  reg = (reg << 1) & ((1 << SCRAMBLER_DEG) - 1);
  if (b1)
    reg ^= poly;
  
  s->scrambler_reg = reg;

  return b1;
}

int unscramble_bit(V34DSPState *s, int b, int poly)
{
  int b1, reg;
 
  reg = s->scrambler_reg;

  b1 = (reg >> (SCRAMBLER_DEG-1)) ^ b;
  reg = (reg << 1) & ((1 << SCRAMBLER_DEG) - 1);
  if (b)
    reg ^= poly;
  
  s->scrambler_reg = reg;

  return b1;
}

/* build the constellation (no need to store it completely, we are lazy!) */

static int constellation_cmp(const void *a_ptr, const void *b_ptr)
{
  int x1,y1,x2,y2,d;
  x1= ((s8 *)a_ptr)[0];
  y1= ((s8 *)a_ptr)[1];
  x2= ((s8 *)b_ptr)[0];
  y2= ((s8 *)b_ptr)[1];
  
  d = (x1 * x1 + y1 * y1) - (x2 * x2 + y2 * y2) ;
  if (d != 0) 
    return d;
  else
    return y2 - y1;
}

#define rotate_clockwise(x, y, x1, y1, z)\
{\
      switch(z) {\
      case 0:\
        x = x1;\
        y = y1;\
        break;\
      case 1:\
        x = -y1;\
        y = x1;\
        break;\
      case 2:\
        x = -x1;\
        y = -y1;\
        break;\
      default:\
        x = y1;\
        y = -x1;\
        break;\
      }\
}

static void build_constellation(V34DSPState *s)
{
  int x,y,i,j,k;

  k = 0;
  for(y=C_MIN; y<= C_MAX; y++) {
      for(x=C_MIN; x<= C_MAX; x++) {
          s->constellation[k][0] = 4*x+1;
          s->constellation[k][1] = 4*y+1;
          k++;
      }
  }

  /* now sort the constellation */
  qsort(s->constellation, C_MAX_SIZE, 2, constellation_cmp);

#if 0
  for(i=0;i<L_MAX/4;i++) fprintf(stderr, "%d: x=%d y=%d\n",
                            i, s->constellation[i][0], s->constellation[i][1]);
#endif

  /* build the table for the decoder (not the best table, the corners
     are not ok) */
  
  memset(s->constellation_to_code, 0, sizeof(s->constellation_to_code));
  for(j=0;j<4;j++) {
    for(i=0;i<L_MAX/4;i++) {
      int x1,y1;

      x1 = s->constellation[i][0];
      y1 = s->constellation[i][1];

      rotate_clockwise(x, y, x1, y1, j);

      x = (x + C_RADIUS) >> 1;
      y = (y + C_RADIUS) >> 1;
      
      s->constellation_to_code[x][y] = i | (j << 14);
    }
  }
}

/* index to ring utilities */

static inline int g2(V34DSPState *st, int p, int m)
{
  if (p >= 0 && p <= 2*(m-1))
    return m - abs(p-(m-1));
  else {
    return 0;
  }
}

static inline int g4(V34DSPState *st, int p, int m)
{
  int s,i;
  
  s = 0;
  if (p >= 0 && p <= 4*(m-1)) {
    for(i=0;i<=p;i++) s += st->g2_tab[i] * st->g2_tab[p-i];
  }
  return s;
}

static inline int g8(V34DSPState *st, int p, int m)
{
  int s,i;

  s = 0;
  if (p >= 0 && p <= 8*(m-1)) {
    for(i=0;i<=p;i++) s += st->g4_tab[i] * st->g4_tab[p-i];
  }
  return s;
}

static void index_to_rings(V34DSPState *s, int ring[4][2], int r0)
{
  int a,b,c,d,e,f,g,h,r1,r2,r3,r4,r5,tmp,m;
  
  m = s->M;

  a = -1;
  r1 = 0;
  for(;;) {
    tmp = r0 - s->z8_tab[a+1];
    if (tmp < 0) break;
    r1 = tmp;
    a++;
  }
  
  b = 0;
  for(;;) {
    tmp = r1 - s->g4_tab[b] * s->g4_tab[a-b];
    if (tmp < 0) break;
    r1 = tmp;
    b++;
  }
  
  tmp = s->g4_tab[b];
  r2 = r1 % tmp;
  r3 = (r1 - r2) / tmp;

  c = 0;
  r4 = r2;
  for(;;) {
    tmp = r4 - s->g2_tab[c] * s->g2_tab[b-c];
    if (tmp < 0) break;
    r4 = tmp;
    c++;
  }

  d = 0;
  r5 = r3;
  for(;;) {
    tmp = r5 - s->g2_tab[d] * s->g2_tab[a-b-d];
    if (tmp < 0) break;
    r5 = tmp;
    d++;
  }

  tmp = s->g2_tab[c];
  e = r4 % tmp;
  f = (r4 - e) / tmp;
  
  tmp = s->g2_tab[d];
  g = r5 % tmp;
  h = (r5 - g) / tmp;
  
  if (c < m) {
    ring[0][0] = e;
    ring[0][1] = c - ring[0][0];
  } else {
    ring[0][1] = m - 1 - e;
    ring[0][0] = c - ring[0][1];
  }
    
  if ((b-c) < m) {
    ring[1][0] = f;
    ring[1][1] = b - c - ring[1][0];
  } else {
    ring[1][1] = m - 1 - f;
    ring[1][0] = b - c - ring[1][1];
  }
  
  if (d < m) {
    ring[2][0] = g;
    ring[2][1] = d - ring[2][0];
  } else {
    ring[2][1] = m - 1 - g;
    ring[2][0] = d - ring[2][1];
  }

  if ((a-b-d) < m) {
    ring[3][0] = h;
    ring[3][1] = a - b - d - ring[3][0];
  } else {
    ring[3][1] = m - 1 - h;
    ring[3][0] = a - b - d - ring[3][1];
  }
}

/* return the K bit index corresponding to the rings */
static int rings_to_index(V34DSPState *s, int ring[4][2])
{
  int a,b,c,d,e,f,g,h,r0,r1,r2,r3,r4,r5,m,i;

  m = s->M;

  /* find back the parameters */
  c = ring[0][0] + ring[0][1];
  if (c < m) e = ring[0][0]; else e = m - 1 - ring[0][1];
  
  b = ring[1][0] + ring[1][1];
  if (b < m) f = ring[1][0]; else f = m - 1 - ring[1][1];
  b += c;
  
  d = ring[2][0] + ring[2][1];
  if (d < m) g = ring[2][0]; else g = m - 1 - ring[2][1];
  
  a = ring[3][0] + ring[3][1];
  if (a < m) h = ring[3][0]; else h = m - 1 - ring[3][1];
  a += b + d;

  r5 = h * s->g2_tab[d] + g;
  r4 = f * s->g2_tab[c] + e;
  
  r3 = r5;
  for(i=0;i<d;i++) r3 += s->g2_tab[i] * s->g2_tab[a-b-i];
  
  r2 = r4;
  for(i=0;i<c;i++) r2 += s->g2_tab[i] * s->g2_tab[b-i];

  r1 = r3 * s->g4_tab[b] + r2;
  
  for(i=0;i<b;i++) r1 += s->g4_tab[i] * s->g4_tab[a-i];

  r0 = r1 + s->z8_tab[a];

  return r0;
}

/* initialize the g2, g4, g8 & z8 tables */
static void build_rings(V34DSPState *s)
{
  int n,i,m;
  m = s->M;
  n = 8*(m - 1) + 1;
  for(i=0;i<n;i++) s->g2_tab[i] = g2(s,i,m);
  for(i=0;i<n;i++) s->g4_tab[i] = g4(s,i,m);
  for(i=0;i<n;i++) s->g8_tab[i] = g8(s,i,m);
  
  s->z8_tab[0] = 0;
  for(i=1;i<n;i++) {
    s->z8_tab[i] = s->z8_tab[i-1] + s->g8_tab[i-1];
  }

#if 0
  {
    int i;
    for(i=0;i<=(1 << s->K);i++) {
      int m[4][2],j, r1,r0;
      r0 = random() % (1 << s->K);
      index_to_rings(s, m, r0);
      r1 = rings_to_index(s, m);
      fprintf(stderr, "%d:", r0);
      for(j=0;j<4;j++) fprintf(stderr, " %d %d",m[j][0], m[j][1]);
      fprintf(stderr, "\n");
      if (r0 != r1) {
        fprintf(stderr, "error r0=%d r1=%d\n" , r0, r1);
        exit(1);
      }
    }
  }
#endif

}

/* parameters for each symbol rate */
static u8 S_tab[6][8] = {
  /* a, c, d1, e1, d2, e2, J, P */
    { 1, 1, 2, 3, 3, 4, 7, 12, }, /* S=2400 */
    { 8, 7, 3, 5, 2, 3, 8, 12, }, /* S=2743 */
    { 7, 6, 3, 5, 2, 3, 7, 14, }, /* S=2800 */
    { 5, 4, 3, 5, 2, 3, 7, 15, }, /* S=3000 */
    { 4, 3, 4, 7, 3, 5, 7, 16, }, /* S=3200 */
    {10, 7, 4, 7, 4, 7, 8, 15, }, /* S=3429 */
};

/* this table depends on the sample rate. We have S=a1/c1 * V34_SAMPLE_RATE */
static u8 baud_tab[6][2] = {
    /* a1, c1 */
    { 3, 10 },
    { 12, 35 },
    { 7, 20 },
    { 3, 8 },
    { 2, 5 },
    { 3, 7 },
};

static s16 *rc_filter[6] = {
    v34_rc_10_filter,
    v34_rc_35_filter,
    v34_rc_20_filter,
    v34_rc_8_filter,
    v34_rc_5_filter,
    v34_rc_7_filter,
};
    
static void build_tx_filter(V34DSPState *s)
{
    /* sampled at every symbol */
    
    s->tx_filter = rc_filter[s->S];
    s->baud_incr = s->symbol_rate * (float)0x10000 / (float)V34_SAMPLE_RATE;
    s->baud_phase = 4;

    s->carrier_phase = 0;
    s->carrier_incr = s->carrier_freq * (float)0x10000 / (float)V34_SAMPLE_RATE;
    /* init TX fifo */
    s->tx_filter_wsize = RC_FILTER_SIZE;
    s->tx_buf_ptr = 0;
    s->tx_outbuf_ptr = s->tx_filter_wsize;
    s->tx_buf_size = 0;
}

float hilbert[156] = 
{
/* alpha=0.000000 beta=0.000000 */
  0.0000000000e+00,
  1.0438059849e-03,
  0.0000000000e+00,
  1.1113855722e-03,
  0.0000000000e+00,
  1.2232369871e-03,
  0.0000000000e+00,
  1.3825587134e-03,
  0.0000000000e+00,
  1.5926453386e-03,
  0.0000000000e+00,
  1.8569074639e-03,
  0.0000000000e+00,
  2.1788971424e-03,
  0.0000000000e+00,
  2.5623400268e-03,
  0.0000000000e+00,
  3.0111756977e-03,
  0.0000000000e+00,
  3.5296080377e-03,
  0.0000000000e+00,
  4.1221680212e-03,
  0.0000000000e+00,
  4.7937919718e-03,
  0.0000000000e+00,
  5.5499192449e-03,
  0.0000000000e+00,
  6.3966145267e-03,
  0.0000000000e+00,
  7.3407216217e-03,
  0.0000000000e+00,
  8.3900579330e-03,
  0.0000000000e+00,
  9.5536620984e-03,
  0.0000000000e+00,
  1.0842111879e-02,
  0.0000000000e+00,
  1.2267936058e-02,
  0.0000000000e+00,
  1.3846153846e-02,
  0.0000000000e+00,
  1.5594989773e-02,
  0.0000000000e+00,
  1.7536833977e-02,
  0.0000000000e+00,
  1.9699551684e-02,
  0.0000000000e+00,
  2.2118299263e-02,
  0.0000000000e+00,
  2.4838091053e-02,
  0.0000000000e+00,
  2.7917505894e-02,
  0.0000000000e+00,
  3.1434171201e-02,
  0.0000000000e+00,
  3.5493106154e-02,
  0.0000000000e+00,
  4.0239829657e-02,
  0.0000000000e+00,
  4.5881743462e-02,
  0.0000000000e+00,
  5.2724604849e-02,
  0.0000000000e+00,
  6.1238171887e-02,
  0.0000000000e+00,
  7.2182437365e-02,
  0.0000000000e+00,
  8.6871563629e-02,
  0.0000000000e+00,
  1.0778971907e-01,
  0.0000000000e+00,
  1.4026261876e-01,
  0.0000000000e+00,
  1.9814073999e-01,
  0.0000000000e+00,
  3.3221536070e-01,
  0.0000000000e+00,
  9.9962693916e-01,
  0.0000000000e+00,
 -9.9962693916e-01,
 -0.0000000000e+00,
 -3.3221536070e-01,
 -0.0000000000e+00,
 -1.9814073999e-01,
 -0.0000000000e+00,
 -1.4026261876e-01,
 -0.0000000000e+00,
 -1.0778971907e-01,
 -0.0000000000e+00,
 -8.6871563629e-02,
 -0.0000000000e+00,
 -7.2182437365e-02,
 -0.0000000000e+00,
 -6.1238171887e-02,
 -0.0000000000e+00,
 -5.2724604849e-02,
 -0.0000000000e+00,
 -4.5881743462e-02,
 -0.0000000000e+00,
 -4.0239829657e-02,
 -0.0000000000e+00,
 -3.5493106154e-02,
 -0.0000000000e+00,
 -3.1434171201e-02,
 -0.0000000000e+00,
 -2.7917505894e-02,
 -0.0000000000e+00,
 -2.4838091053e-02,
 -0.0000000000e+00,
 -2.2118299263e-02,
 -0.0000000000e+00,
 -1.9699551684e-02,
 -0.0000000000e+00,
 -1.7536833977e-02,
 -0.0000000000e+00,
 -1.5594989773e-02,
 -0.0000000000e+00,
 -1.3846153846e-02,
 -0.0000000000e+00,
 -1.2267936058e-02,
 -0.0000000000e+00,
 -1.0842111879e-02,
 -0.0000000000e+00,
 -9.5536620984e-03,
 -0.0000000000e+00,
 -8.3900579330e-03,
 -0.0000000000e+00,
 -7.3407216217e-03,
 -0.0000000000e+00,
 -6.3966145267e-03,
 -0.0000000000e+00,
 -5.5499192449e-03,
 -0.0000000000e+00,
 -4.7937919718e-03,
 -0.0000000000e+00,
 -4.1221680212e-03,
 -0.0000000000e+00,
 -3.5296080377e-03,
 -0.0000000000e+00,
 -3.0111756977e-03,
 -0.0000000000e+00,
 -2.5623400268e-03,
 -0.0000000000e+00,
 -2.1788971424e-03,
 -0.0000000000e+00,
 -1.8569074639e-03,
 -0.0000000000e+00,
 -1.5926453386e-03,
 -0.0000000000e+00,
 -1.3825587134e-03,
 -0.0000000000e+00,
 -1.2232369871e-03,
 -0.0000000000e+00,
 -1.1113855722e-03,
 -0.0000000000e+00,
 -1.0438059849e-03,
};


s16 *v34_rx_filters[12] = {
    v34_rx_filter_2400_1600,
    v34_rx_filter_2400_1800,
    v34_rx_filter_2743_1646,
    v34_rx_filter_2743_1829,
    v34_rx_filter_2800_1680,
    v34_rx_filter_2800_1867,
    v34_rx_filter_3000_1800,
    v34_rx_filter_3000_2000,
    v34_rx_filter_3200_1829,
    v34_rx_filter_3200_1920,
    v34_rx_filter_3429_1959,
    v34_rx_filter_3429_1959,
};
    
static void build_rx_filter(V34DSPState *s)
{
    float a, f_low, f_high;
    int i;


    s->rx_filter = v34_rx_filters[s->S * 2 + s->use_high_carrier];

    /* XXX: temporary hack to synchronize */
    if (s->S == V34_S3429)
        s->baud_phase = 1;
    else
        s->baud_phase = 2;
    s->baud_num = (s->baud_num * 3);

    s->carrier_incr = s->carrier_freq * (float)0x10000 / s->symbol_rate;
    s->carrier_phase = 0;
    s->rx_buf1_ptr = 0;
    s->rx_filter_wsize = (s->baud_denom * RC_FILTER_SIZE) / s->baud_num;
    fprintf(stderr, "cincr=%d baudincr=%d\n", s->carrier_incr, s->baud_incr);

    s->baud_phase = s->baud_phase << 16;
    s->baud_num = s->baud_num << 16;
    s->baud_denom = s->baud_denom << 16;

    /* equalizer : init to identity */
    s->eq_filter[EQ_SIZE/2][0] = 0x4000 << 16;
    /* XXX: hilbert normalization ? */
    for(i=0;i<EQ_SIZE;i++) 
        s->eq_filter[i][1] = (int)(hilbert[i] * 0.61475 * (0x4000 << 16));
    
    /* adaptation shift : big at the beginning, should be small after. */
    s->eq_shift = 0;

    /* synchronization : Nyquist filters at the upper & lower frequencies */
    a = 0.99;
    f_low = 2 * M_PI * (s->carrier_freq - s->symbol_rate / 2.0) / 
        (3.0 * s->symbol_rate);
    f_high = 2 * M_PI * (s->carrier_freq + s->symbol_rate / 2.0) / 
        (3.0 * s->symbol_rate);
    fprintf(stderr, "%f %f\n", f_low, f_high);

    s->sync_low_coef[0] = (int)(2 * a * cos(f_low) * 0x4000);
    s->sync_low_coef[1] = (int)( - a * a * 0x4000);

    s->sync_high_coef[0] = (int)(2 * a * cos(f_high) * 0x4000);
    s->sync_high_coef[1] = (int)(- a * a * 0x4000);

    /* precomputed constants to compute the cross correlation */
    s->sync_A = (int)( - a * a * sin(f_high - f_low) * 0x4000);
    s->sync_B = (int)( a * sin(f_high) * 0x4000);
    s->sync_C = (int)( - a * sin(f_low) * 0x4000);
}

int V34_init_low(V34DSPState *s, V34State *p, int transmit)
{
  int S,d,e;
  
  /* copy the params */
  s->calling = p->calling;
  s->S = p->S;
  s->expanded_shape = p->expanded_shape;
  s->R = p->R;
  if (p->use_aux_channel)
      s->R += 200;
  s->conv_nb_states = p->conv_nb_states;
  s->use_non_linear = p->use_non_linear;
  s->use_high_carrier = p->use_high_carrier;
  memcpy(s->h, p->h, sizeof(s->h));

  /* superframe & data frame size */
  S = s->S;
  if (!s->use_high_carrier) {
    d = S_tab[S][2];
    e = S_tab[S][3];
  } else {
    d = S_tab[S][4];
    e = S_tab[S][5];
  }
  s->symbol_rate = 2400.0 * (float)S_tab[S][0] / (float)S_tab[S][1];
  s->carrier_freq = s->symbol_rate * (float)d / (float)e;

  s->J = S_tab[S][6];
  s->P = S_tab[S][7];
  s->N = (s->R * 28) / (s->J * 100); 
  /* max length of a mapping frame (no need for table 8 as in the spec !) */
  s->b = s->N / s->P; 
  if ((s->b * s->P) < s->N) s->b++;
  s->r = s->N - (s->b - 1) * s->P;
  
  /* aux channel */
  if (p->use_aux_channel)
      s->W = 15 - s->J; /* no need to test as in the spec ! */
  else
      s->W = 0; /* no aux channel */

  /* mapping parameters */
  s->q = 0;
  if (s->b <= 12) {
    s->K = 0;
  } else {
    s->K = s->b - 12;
    while (s->K >= 32) {
      s->K -= 8;
      s->q++;
    }
  }

  /* XXX: use integer arith ! */
  if (!s->expanded_shape) {
    s->M = (int) ceil(pow(2.0, s->K / 8.0));
  } else {
    s->M = (int) rint(1.25 * pow(2.0, s->K / 8.0));
  }
  s->L = 4 * s->M * (1 << s->q);

#ifdef DEBUG
  fprintf(stderr, "S_index=%d (S=%0.0f carrier=%0.0f)\n"
         "R=%d J=%d P=%d N=%d b=%d r=%d W=%d\n", 
         s->S, s->symbol_rate, s->carrier_freq,
         s->R, s->J, s->P, s->N, s->b, s->r, s->W);
  fprintf(stderr, "K=%d q=%d M=%d L=%d\n", s->K, s->q, s->M, s->L);
#endif

  build_constellation(s);
  
  build_rings(s);

  s->baud_num = baud_tab[S][0];
  s->baud_denom = baud_tab[S][1];

  if (transmit) {
      build_tx_filter(s);
  } else {
      build_rx_filter(s);
      agc_init(s);
      s->phase_4d = 0;
  }

  s->Z_1 = 0;
  s->U0 = 0; /* trellis coder memory */
  memset(s->x, 0, sizeof(s->x));
  s->half_data_frame_count = 0;
  s->sync_count = 0;
  s->conv_reg = 0;
  s->scrambler_reg = 0;

  s->mapping_frame = 0; /* mapping frame counters */
  s->acnt = 0;
  s->rcnt = 0;

  return 0;
}

/* shift by n & round toward zero */ 
static inline int shr_round0(int val, int n)
{
  int offset;

  offset = (1 << (n-1)) - 1;
  if (val >= 0) {
    val = (val + offset) >> n;
  } else {
    val = -val;
    val = (val + offset) >> n;
    val = -val;
  }
  return val;
}

/* clamp a between -v & v */
static inline int clamp(int a, int v)
{
  if (a > v) 
    return v;
  else if (a < -v) 
    return -v;
  else
    return a;
}

/* compute the next state in the trellis (Y[0] is ignored) */
static int trellis_next_state(int conv_nb_states, int conv_reg, int trans)
{
    int i,Y0;
    int Y[5];
    
    Y[1] = (trans & 1);
    Y[2] = ((trans >> 1) & 1);
    Y[4] = ((trans >> 2) & 1);
    Y[3] = ((trans >> 3) & 1);

    Y0 = conv_reg & 1;
    switch(conv_nb_states) {
    case 16:
        /* figure 10 */
        conv_reg ^= (Y[1] << 1) | (Y[2] << 2) | ((Y[2] ^ Y0) << 3) | (Y0 << 4);
        conv_reg >>= 1;
        break;
    case 32:
        /* figure 11 */
        conv_reg ^= (Y[2] << 1) | (Y[4] << 2) | (Y[1] << 3) | (Y[2] << 4) | (Y0 << 5);
        conv_reg >>= 1;
        break;
    default:
    case 64:
        /* figure 12 */
        {
            int r[6], s[6], tmp1, tmp2;
            
            for(i=0;i<6;i++) r[i] = (conv_reg >> i) & 1;
            
            s[0] = r[1] ^ r[3] ^ Y[2];
            s[1] = r[0];
            s[2] = r[3];
            tmp2 = Y[1] ^ r[4];
            s[3] = r[3] ^ tmp2;
            tmp1 = r[4] ^ r[5];
            s[4] = r[2] ^ Y[3] ^ (r[3] & Y[2]) ^ tmp1;
            s[5] = Y[4] ^ tmp1 ^ (r[3] & tmp2);
            
            conv_reg = 0;
            for(i=0;i<6;i++) conv_reg |= s[i] << i;
        }
        break;
    }
    return conv_reg;
}

/* (§ 9.6.3) trellis encoder, return U0 */

static int trellis_encoder(V34DSPState *s, int c0, int yy[2][2])
{
  int v0, ss[2][3], Y[5], i, trans;

  /* convolutional coder */

  /* (§ 9.6.3.1) find Y vector. We traducted the table into binary expressions */
  for(i=0;i<2;i++) {
    int x,y,y0,x0,y1,x1;

    /* XXX: is it right to suppose that we use figure 9 as a periodic mapping ? */
    x = yy[i][0];
    y = yy[i][1];
    x = ((x + 3) >> 1) & 3;
    y = ((y + 3) >> 1) & 3;
    x0 = x & 1;
    x1 = ((x & 2) >> 1);
    y0 = y & 1;
    y1 = ((y & 2) >> 1);
    
    ss[i][2] = x1 ^ y1 ^ y0 ^ x0;
    ss[i][1] = y0;
    ss[i][0] = y0 ^ x0;
  }

  /* table 13 traducted into binary operations */
  Y[4] = ss[0][2] ^ ss[1][2];
  Y[3] = ss[0][1];
  Y[2] = ss[0][0];
  Y[1] = (ss[0][0] & ~ss[1][0] & 1) ^ ss[0][1] ^ ss[1][1];

  /* compute the next trellis state */
  trans = (Y[3] << 3) | (Y[4] << 2) | (Y[2] << 1) | Y[1];
  {   /* SIPFAX: dump what the encoder ACTUALLY emits for this branch - the branch index
         (trans), the y0 that selects the table half, and the four coset labels the decoder
         will compare against, computed the same way the encoder does: ((coord+3)>>1)&3.
         The decoder's ACS scores branch (trans + 16*y0) using the tuples in that row block,
         so if the emitted tuple is not IN that block, the table's row order disagrees with
         the encoder's trans packing and every branch metric is attached to the wrong
         branch. */
      extern FILE *g_tblf;
      if (!g_tblf) { char *e = getenv("SIPFAX_TBLDUMP"); if (e) g_tblf = fopen(e,"w"); }
      if (g_tblf) {
          int c0l = ((yy[0][0] + 3) >> 1) & 3, c1l = ((yy[0][1] + 3) >> 1) & 3;
          int c2l = ((yy[1][0] + 3) >> 1) & 3, c3l = ((yy[1][1] + 3) >> 1) & 3;
          fprintf(g_tblf, "%d %d %d %d %d %d\n", trans, s->conv_reg & 1,
                  c0l, c1l, c2l, c3l);
      }
  }

  { extern FILE *gt_f; if (gt_f) fprintf(gt_f, "%d %d %d %d %d %d ", yy[0][0], yy[0][1], yy[1][0], yy[1][1], trans, s->conv_reg); }
  s->conv_reg = trellis_next_state(s->conv_nb_states, s->conv_reg, trans);
  Y[0] = s->conv_reg & 1;
  { extern FILE *gt_f; if (gt_f) fprintf(gt_f, "%d %d\n", s->conv_reg, Y[0]); }

  /* super frame synchronisation pattern */
  if (s->sync_count == 0) {
    v0 = (SYNC_PATTERN >> (15 - s->half_data_frame_count)) & 1;
  } else {
    v0 = 0;
  }

  return Y[0] ^ c0 ^ v0;
}


static int get_bit(V34DSPState *s)
{
    int b, poly;

    b = s->get_bit(s->opaque);
    if (b == -1) b = 1;
    if (s->calling)
        poly = V34_GPC;
    else
        poly = V34_GPA;
    b = scramble_bit(s, b, poly);
    return b;
}

/* auxilary channel bit */
static int aux_get_bit(V34DSPState *s)
{
    return 0;
}

/* encode size bits into the corresponding symbols of the constellation */
/* return exactly 8 baseband complex samples */
static void encode_mapping_frame(V34DSPState *s)
{
  int m[4][2], r0; /* rings */
  u8 I[3][4];
  int Q[4][2], Z[2], Y[2][2];
  u8 *ptr;
  int t,i,j,k,x,y,x1,y1,w,mp_size;
  int u_re, u_im, p_re, p_im, c_re, c_im, C0, x_re, x_im, xp_re, xp_im;
  u8 data[MAX_MAPPING_FRAME_SIZE];

  /* compute mapping frame size */
  s->rcnt += s->r;
  if (s->rcnt < s->P) {
      mp_size = s->b - 1;
  } else {
      s->rcnt -= s->P;
      mp_size = s->b;
  }
    
  /* send an auxilary channel bit if needed */
  s->acnt += s->W;
  if (s->acnt < s->P) {
      data[0] = get_bit(s);
  } else {
      s->acnt -= s->P;
      data[0] = aux_get_bit(s); 
  }

  for(i=1;i<mp_size;i++) data[i] = get_bit(s);
  { extern FILE *g_mftx; if (!g_mftx) { char *e = getenv("SIPFAX_MFDUMP_TX");
      if (e) g_mftx = fopen(e,"w"); }
    if (g_mftx) { int z; for (z=0;z<mp_size;z++) fputc('0'+(data[z]&1), g_mftx); fputc('\n', g_mftx); } }

  //  print_bit_vector("sent", data, mp_size);
  
  if (s->b <= 12) {
    /* (§ 9.3.2) simple case: no shell mapping */
    memset(m, 0, sizeof(m));
    for(i=0;i<s->b;i++) ((u8 *)I)[i] = data[i];
    for(i=s->b;i<12;i++) ((u8 *)I)[i] = 0;
    memset(Q, 0, sizeof(Q));
  } else {
    /* (§ 9.3.1) */
    /* leave one bit if low frame */
    int n;

    n = s->K;
    if (mp_size < s->b) n--;
    
    ptr = data;
    r0 = 0;
    for(i=0;i<n;i++) r0 |= *ptr++ << i;
    index_to_rings(s, m, r0);

    for(j=0;j<4;j++) {
      I[0][j] = ptr[0];
      I[1][j] = ptr[1];
      I[2][j] = ptr[2];
      ptr += 3;

      t = 0;
      for(i=0;i<s->q;i++) t |= *ptr++ << i;
      Q[j][0] = t;

      t = 0;
      for(i=0;i<s->q;i++) t |= *ptr++ << i;
      Q[j][1] = t;
    }
  }
  
  if (s->b < 56) 
    w = 1;
  else 
    w = 2;

  for(j=0;j<4;j++) {
    /* for each 4D symbol */

    /* (§ 9.5) differential coding */
    Z[0] = (I[1][j] + 2 * I[2][j] + s->Z_1) & 3;
    s->Z_1 = Z[0];
    
    /* (§ 9.6.1) mapping to 2D symbols */
    Z[1] = (Z[0] + 2 * I[0][j] + s->U0) & 3;
    {   /* SIPFAX: encoder-side ground truth for the sync work - the Z pair actually
           transmitted, the U0 folded into Z[1] (which is the PREVIOUS trellis_encoder
           call's return), and the v0 this symbol will carry. Diffing this against the
           decoder's recovered values localises the remaining fault without another round
           of reasoning about the indexing. */
        extern FILE *g_encf;
        int v0e = (s->sync_count == 0)
                ? ((SYNC_PATTERN >> (15 - s->half_data_frame_count)) & 1) : 0;
        if (!g_encf) { char *e = getenv("SIPFAX_ENCDUMP"); if (e) g_encf = fopen(e,"w"); }
        if (g_encf) fprintf(g_encf, "%d %d %d %d %d %d %d\n", Z[0], Z[1], s->U0, v0e,
                            s->sync_count, s->half_data_frame_count, s->conv_reg);
    }

    C0 = 0; /* for trellis coding */
    for(i=0;i<2;i++) {
      t = Q[j][i] + (m[j][i] << s->q);

      assert(t >= 0 && t < L_MAX/4);
      x1 = s->constellation[t][0];
      y1 = s->constellation[t][1];
      /* rotation by Z[i] * 90 degress clockwise */
      rotate_clockwise(x, y, x1, y1, Z[i]);
      u_re = x;
      u_im = y;

      /* (§ 9.6.2) precoder */
      x = 0;
      y = 0;
      for(k=0;k<3;k++) {
        x += s->x[k][0] * s->h[k][0] - s->x[k][1] * s->h[k][1];
        y += s->x[k][1] * s->h[k][0] + s->x[k][0] * s->h[k][1];
      }
      /* round to 2^-7 */
      p_re = shr_round0(x, 14);
      p_im = shr_round0(y, 14);
      
      /* compute c(n) */
      c_re = shr_round0(p_re, 7 + w) << w;
      c_im = shr_round0(p_im, 7 + w) << w;
      C0 += c_re + c_im;

      Y[i][0] = clamp(u_re + c_re, 255);
      Y[i][1] = clamp(u_im + c_im, 255);
      
      x_re = (Y[i][0] << 7) - p_re;
      x_im = (Y[i][1] << 7) - p_im;

      for(k=2;k>=1;k--) {
        s->x[k][0] = s->x[k-1][0];
        s->x[k][1] = s->x[k-1][1];
      }
      s->x[0][0] = x_re;
      s->x[0][1] = x_im;

      /* (§ 9.7) non linear encoder */

      if (!s->use_non_linear) {
        xp_re = x_re;
        xp_im = x_im;
      } else {
        double x2, dzeta, theta, x2mean;
        /* SIPFAX: answer the XXX above - the normalisation IS the average power, and
           without it this was not a warp at all. dzeta was pinned at 0.3125, so theta came
           out a constant 1.0529 for every symbol: a flat 5.3% gain, which is a scale, not a
           non-linearity. The author's commented-out intent, x2/128.0, cannot work either -
           at L=384 the mean |x|^2 is about 4.0e6, so that expression reaches ~244 and the
           series explodes. Both problems have the same cause: zeta is a RATIO and needs the
           constellation's mean power underneath it.

           theta = 1 + z/6 + z^2/120 is the truncation of sinh(sqrt z)/sqrt z, the 9.7
           warp: outer points expand more than inner ones, which is where the shaping gain
           comes from. Scaling zeta by |x|^2 / mean|x|^2 makes it that, and keeping the
           constant at 0.3125 means a symbol AT the mean power is warped exactly as much as
           the old code warped everything, so the average level does not jump.
           SIPFAX_NL_K overrides the constant. */
        x2 = (double)x_re * x_re + (double)x_im * x_im;
        x2mean = (s->nl_meanc2 > 0) ? s->nl_meanc2 * 16384.0 : x2;
        { static double nlk = -1;
          if (nlk < 0) { char *e = getenv("SIPFAX_NL_K"); nlk = e ? atof(e) : 0.3125; }
          dzeta = (x2mean > 0) ? nlk * (x2 / x2mean) : nlk; }
        if (dzeta > 4.0) dzeta = 4.0;   /* the 2-term series is only good for small z */
        theta = (1 + dzeta / 6.0 + dzeta * dzeta / 120.0);
        
        xp_re = rint(theta * x_re);
        xp_im = rint(theta * x_im);
      }

      put_sym(s, xp_re, xp_im);
    }
    s->U0 = trellis_encoder(s, (C0 >> 1) & 1, Y);

    /* 4D symbol count & data frame count for synchronisation */
    if (++s->sync_count == 2*s->P) {
      s->sync_count = 0;
      if (++s->half_data_frame_count == 2*s->J) {
        s->half_data_frame_count = 0;
      }
    }
  }

  if (++s->mapping_frame >= s->P) {
      /* new data frame */
      s->mapping_frame = 0;
      s->rcnt = 0;
      s->acnt = 0;
  }
}


/* put a new baseband symbol in the tx queue */
void (*g_symtap)(int, int) = 0;
FILE *gt_f = 0;
/* SIPFAX: a PASSIVE transmit-symbol tap. g_symtap below RETURNS after calling the hook, so
   installing it bypasses the modulator entirely - which is exactly why the "100% bit match"
   loopback never exercised V34_mod or V34_demod_cma. This one only records and lets the
   symbol continue to the modulator, so real audio can be generated WITH ground truth. */
FILE *g_txsymf = 0;
/* SIPFAX: measurement tap - see v34_shaped_meanc2() */
static double shp_acc; static long shp_n; static int shp_on;

static void put_sym(V34DSPState *s, int si, int sq)
{
    if (shp_on) { shp_acc += (double)si*si + (double)sq*sq; shp_n++; return; }
    if (g_txsymf) fprintf(g_txsymf, "%d %d\n", si, sq);
    if (g_symtap) { g_symtap(si, sq); return; }
    s->tx_buf[s->tx_buf_ptr][0] = (si * s->tx_amp) >> 7;
    s->tx_buf[s->tx_buf_ptr][1] = (sq * s->tx_amp) >> 7;

    s->tx_buf_ptr = (s->tx_buf_ptr + 1) & (TX_BUF_SIZE - 1);
    s->tx_buf_size++;

    assert(s->tx_buf_size <= TX_BUF_SIZE);
}


/* write at most nb samples, return the number of samples
   written. Stops if no more baseband symbols in tx queue */
static int V34_baseband_to_carrier(V34DSPState *s, 
                                   s16 *samples, unsigned int nb)
{
    int si, sq, ph, i, j, k;

    for(i=0;i<nb;i++) {
        /* is there enough symbols in the queue ? */
        if (s->tx_buf_size < s->tx_filter_wsize)
            break;

        /* apply the spectrum shaping filter */
        ph = s->baud_phase;
        si = sq = 0;
        for(j=0;j<s->tx_filter_wsize;j++) {
            k = (s->tx_outbuf_ptr - j - 1) & 
                (TX_BUF_SIZE - 1);
            si += s->tx_buf[k][0] * s->tx_filter[ph];
            sq += s->tx_buf[k][1] * s->tx_filter[ph];
            ph += s->baud_denom;
        }
        si = si >> 14;
        sq = sq >> 14;
        if ( si != (short)si || sq != (short)sq) {
            { extern int v34_dbg; if (v34_dbg > 1) fprintf(stderr, "error %d %d\n", si, sq); }
        }
        // fprintf(stderr, "M: phase=%04X %d %d\n", s->baud_phase, si, sq);
        /* get next baseband symbols */
        s->baud_phase += s->baud_num;
        if (s->baud_phase >= s->baud_denom) {
            s->baud_phase -= s->baud_denom;
            s->tx_outbuf_ptr = (s->tx_outbuf_ptr + 1) & (TX_BUF_SIZE - 1);
            s->tx_buf_size--;
            { extern long g_txsym_out; g_txsym_out++; }   /* SIPFAX: drain accounting */
        }
        
        /* center on the carrier */
        samples[i] = (si * dsp_cos(s->carrier_phase) - 
            sq * dsp_cos((PHASE_BASE/4) - s->carrier_phase)) >> COS_BITS;
        s->carrier_phase += s->carrier_incr;
    }
    return i;
}

#define S_POWER     (1 + 1)
#define TRN4_POWER  (1 + 1)
#define TRN16_POWER ((1 + 1 + 2*(9 + 1) + 9 + 9) / 4.0)

/* compute the normalized amplitude */
#define CALC_AMP(x) (int)( (128.0 * 128.0) / sqrt(x) )

/* send the V34 S sequence, duration: 128 T */

/* SIPFAX: 16-point slicer for Phase-4 TRN/MP. Returns the (q,z) whose point
   base[q] rotated CLOCKWISE by z*90 is nearest the received symbol. base = points 0..3 of
   the quarter-superconstellation (10.1.3.6): (1,1) (-3,1) (1,-3) (-3,-3), i.e. increasing
   magnitude with ties broken by greatest imaginary part. */
static int srx_rx16(void)
{   /* 16-point RX applies exactly when we signalled J16POINTS - i.e. when OUR J told the
       caller to transmit 16-point. It used to key off SIPFAX_MP16/SLCOMPAT, which are
       about our own TX and left the streaming tracker using a 16-pt Godard radius on the
       caller's 4-point signal. */
    static int v = -1;
    if (v < 0) { char *e = getenv("SIPFAX_TX_J16"); v = (e && atoi(e)) ? 1 : 0; }
    return v;
}

static void srx_slice16(double pi_, double pq_, double rms, int *qo, int *zo,
                        double *dio, double *dqo)
{
    static const int bi[4] = { 1, -3, 1, -3 };
    static const int bq[4] = { 1, 1, -3, -3 };
    double sc, bd = 1e30;
    int q, z, bqi = 0, bzi = 0;
    /* the 16-point set has mean power 10 (in base units); normalise the input to match */
    sc = (rms > 1e-12) ? sqrt(10.0 / rms) : 1.0;
    pi_ *= sc; pq_ *= sc;
    for (q = 0; q < 4; q++) {
        double x = bi[q], y = bq[q];
        for (z = 0; z < 4; z++) {
            double dx = pi_ - x, dy = pq_ - y, d = dx*dx + dy*dy;
            double nx;
            if (d < bd) { bd = d; bqi = q; bzi = z; }
            nx = y; y = -x; x = nx;      /* rotate CLOCKWISE by 90 for the next z */
        }
    }
    *qo = bqi; *zo = bzi;
    if (dio) {   /* decided point, scaled back into signal units */
        static const int rbi[4] = { 1, -3, 1, -3 };
        static const int rbq[4] = { 1, 1, -3, -3 };
        double x = rbi[bqi], y = rbq[bqi], nx;
        int zz;
        for (zz = 0; zz < bzi; zz++) { nx = y; y = -x; x = nx; }
        *dio = x / sc; *dqo = y / sc;
    }
}

static void V34_send_S(V34DSPState *s)
{
    int i;

    /* transmit amplitude multiplier */
    s->tx_amp = CALC_AMP(S_POWER);

    for(i=0;i<64;i++) {
        put_sym(s, 128, 128); /* 0 deg */
        put_sym(s, -128, 128); /* -90 deg */
    }
}

/* send the V34 S bar sequence, duration: 16 T */
static void V34_send_Sinv(V34DSPState *s)
{
    int i;

    s->tx_amp = CALC_AMP(S_POWER);
    for(i=0;i<8;i++) {
        put_sym(s, -128, -128); /* 180 deg */
        put_sym(s, 128, -128); /* 90 deg */
    }
}

/* send the PP sequence, duration: 288 T */
static void V34_send_PP(V34DSPState *s)
{
    int k,p;

    s->tx_amp = 128;

    /* 6 periods */
    for(p=0;p<6;p++) {
        for(k=0;k<V34_PP_SIZE;k++) {
            put_sym(s, tabPP[k].re, tabPP[k].im);
        }
    }
}

/* send the TRN sequence, (4 or 16 states), sent for 2048 T (XXX: this
   time was choosen randomly */
static void V34_send_TRN(V34DSPState *s)
{
    { extern long v34_ntrn; extern int v34_dbg; if(v34_dbg) fprintf(stderr,"[scr] V34_send_TRN entry #%ld scrambler_reg=0x%x\n",v34_ntrn,s->scrambler_reg); v34_ntrn++; }
    int i,x,y,x1,y1,z,poly,I1,I2,Q1,Q2,q;

    if (s->calling)
        poly = V34_GPC;
    else
        poly = V34_GPA;

    if (s->is_16states) 
        s->tx_amp = CALC_AMP(TRN16_POWER);
    else
        s->tx_amp = CALC_AMP(TRN4_POWER);
        
    for(i=0;i<1024;i++) {
        I1 = scramble_bit(s, 1, poly);
        I2 = scramble_bit(s, 1, poly);
        if (s->is_16states) {
            Q1 = scramble_bit(s, 1, poly);
            Q2 = scramble_bit(s, 1, poly);
            q = (Q2 << 1) | Q1;
        } else {
            q = 0;
        }
        x1 = s->constellation[q][0] << 7;
        y1 = s->constellation[q][1] << 7;
        z = (I2 << 1) | I1;
        { extern int v34_dbg; static int tn=0; if(v34_dbg && tn<40){fprintf(stderr,"%d",z); if(++tn==40)fprintf(stderr," <-TRN z\n");} }
        /* spec 10.1.3.8: rotate CLOCKWISE by z*90; the macro rotates CCW -> negate */
        rotate_clockwise(x, y, x1, y1, (4 - z) & 3);
        put_sym(s, x, y);
    }
    s->Z_1 = z; /* the last value z is used for the next sequence */
}

void put_bits(u8 **pp, int n, int bits)
{
    u8 *p;
    int i;

    p = *pp;
    for(i=n-1;i>=0;i--) {
        *p++ = (bits >> i) & 1;
    }
    *pp = p;
}

/* from § 10.1.2.3.2 */
int calc_crc(u8 *buf, int size)
{
    int crcinv, crc,i,b;

    crc = 0xffff;
    
    for(i=0;i<size;i++) {
        b = (crc & 1) ^ buf[i];
        crc = (crc >> 1) ^ ((b << 15) | (b << 10) | (b << 3));
    }

    /* invert the order of the crc bits (could be done while
       computing, but who cares ? */
    crcinv = 0;
    for(i=0;i<16;i++) {
        crcinv |= ((crc >> i) & 1) << (15-i);
    }
    return crcinv;
}

/* modulate an MP sequence */
static void V34_mod_MP(V34DSPState *s, u8 *buf, int size, int is_16states)
{
    int x,y,x1,y1,z,poly,I1,I2,Q1,Q2,q;
    u8 *p;
    
    if (s->calling)
        poly = V34_GPC;
    else
        poly = V34_GPA;

    if (is_16states) 
        s->tx_amp = CALC_AMP(TRN16_POWER);
    else
        s->tx_amp = CALC_AMP(TRN4_POWER);
        
    p = buf;
    z = s->Z_1;
    while ((p - buf) < size) {
        I1 = scramble_bit(s, *p++, poly);
        I2 = scramble_bit(s, *p++, poly);
        if (is_16states) {
            Q1 = scramble_bit(s, *p++, poly);
            Q2 = scramble_bit(s, *p++, poly);
            q = (Q2 << 1) | Q1;
        } else {
            q = 0;
        }
        x1 = s->constellation[q][0] << 7;
        y1 = s->constellation[q][1] << 7;
        { extern int v34_dbg; static int jn=0; if(v34_dbg && jn<40){fprintf(stderr,"[b%d/i%d%d]",(int)(p-buf),I1,I2);} }
        z = (((I2 << 1) | I1) + z) & 3;
        { extern int v34_dbg; static int jn=0; if(v34_dbg && jn<40){fprintf(stderr,"%d",z); if(++jn==40)fprintf(stderr," <-J z\n");} }
        /* spec 10.1.3.3: rotate CLOCKWISE by Zn*90 -> negate for the CCW macro */
        rotate_clockwise(x, y, x1, y1, (4 - z) & 3);
        put_sym(s, x, y);
    }
    s->Z_1 = z;
}


/* send MP sequence. 'type' select its type (0 or 1). 'do_ack' selects
   if it is an acknowledge sequence */
/* SIPFAX: our RECEIVER's own residual-ISI estimate, defined further down. Tentative
   declarations so the MP builder can advertise it. */
static s16 p4_hest[3][2];
static int p4_have_h;

static void V34_send_MP(V34DSPState *s, int type, int do_ack)
{
    u8 buf[188],*p;
    int i,j,crc;

    p = buf;
    /* SIPFAX: mirror the parameters the caller proposed in its MP instead of
       advertising a fixed 28800/28800 + 16-state trellis. A peer will not
       acknowledge an MP whose parameters contradict its own proposal, and without
       its MP' we never advance to E/B1. Before the caller's MP arrives we fall
       back to our own maximum. */
    {
        int r_ca, r_ac, trel, mi, shape = 0;
        unsigned int msk;
        {   /* SIPFAX: slmodem-compatible MP. Decoding slmodem's own transmission (which
               this caller acknowledges) shows it advertises its OWN capabilities rather
               than mirroring the caller: 16-point MP, ca=ac=16800, trellis 16-state,
               expanded shaping, mask 0x3fff. */
            char *sl = getenv("SIPFAX_MP_SLCOMPAT");
            if (sl && atoi(sl)) {
                /* SIPFAX: ca is the rate we ask the CALLER to transmit at, i.e. our
                   RECEIVE rate. 7 (=16800) is what slmodem advertises, but this line
                   cannot carry it: measured on the caller's own Phase-4 TRN the channel
                   plus our equaliser delivers 22.4-24.9 dB, and the 48/56-point
                   constellation R=16800 uses needs >=24 dB - no margin, on a 4-point
                   signal that is far easier to equalise than data mode. Dropping to
                   9600 (r_ca=4) takes the constellation from L=56 to L=12, worth about
                   6.7 dB, which turns a marginal link into a comfortable one. The caller
                   reached the same conclusion independently: it proposes ac=9600 for our
                   direction. SIPFAX_MP_CA / SIPFAX_MP_AC override (units of 2400 bit/s). */
                r_ca = 4; r_ac = 7; trel = 0; msk = 0x3fff;
                { char *mc = getenv("SIPFAX_MP_CA"); if (mc) r_ca = atoi(mc);
                  char *ma = getenv("SIPFAX_MP_AC"); if (ma) r_ac = atoi(ma); }
                /* SIPFAX: the shaping bit we advertise. slmodem sends 1, and the caller
                   obliges - its transmitted distribution measures kurtosis 1.71 against
                   1.48 unshaped / 1.60 shaped for our own encoder. Setting this to 0 is
                   the experiment that establishes whether OUR MP controls the caller's
                   transmit shaping; if its distribution moves toward 1.48 it does, which
                   tells us the same is likely true of precoding (the live suspect for the
                   missing 4th-power line in its data mode). SIPFAX_MP_SHAPE overrides. */
                { char *ms = getenv("SIPFAX_MP_SHAPE"); shape = ms ? atoi(ms) : 1; }
                goto mp_params_done;
            }
        }
        if (s->p4_mp_rx && s->p4_mp_rate_ca > 0 && s->p4_mp_rate_ac > 0 && s->p4_mp_mask != 0) {
            r_ca = s->p4_mp_rate_ca;
            r_ac = s->p4_mp_rate_ac;
            trel = s->p4_trellis;
            msk  = s->p4_mp_mask;
        } else {
            r_ca = 12; r_ac = 12; trel = 0; msk = 0x0fff;
        }
        mp_params_done:
        put_bits(&p, 17, 0x1ffff); /* frame sync */
        put_bits(&p, 1, 0); /* start bit */
        put_bits(&p, 1, type);
        put_bits(&p, 1, 0); /* reserved */
        put_bits(&p, 4, r_ca); /* call to answer max rate (negotiated) */
        put_bits(&p, 4, r_ac); /* answer to call max rate (negotiated) */
        put_bits(&p, 1, 0); /* no aux channel */
        put_bits(&p, 2, trel); /* trellis: match the caller's selection */
        put_bits(&p, 1, 0); /* non linear encoder disabled */
        put_bits(&p, 1, shape); /* constellation shaping (1=expanded, as slmodem) */
        put_bits(&p, 1, do_ack); /* acknowledge bit */

        put_bits(&p, 1, 0); /* start bit */
        /* emit the mask so that frame bit 35+i == bit i of msk (the order our
           decoder reads, and the caller's own mask as we received it) */
        for (mi = 0; mi < 15; mi++) put_bits(&p, 1, (msk >> mi) & 1);
        put_bits(&p, 1, 1); /* asymetric data rate enable */
    }
    
    if (type == 1) {
        { extern int v34_dbg; static int shown = 0;
          if (v34_dbg && !shown) { shown = 1;
            fprintf(stderr, "[p4] TX: MP advertises OUR h = %.4f%+.4fj %.4f%+.4fj %.4f%+.4fj%s\n",
                    (p4_have_h?p4_hest[0][0]:0)/16384.0, (p4_have_h?p4_hest[0][1]:0)/16384.0,
                    (p4_have_h?p4_hest[1][0]:0)/16384.0, (p4_have_h?p4_hest[1][1]:0)/16384.0,
                    (p4_have_h?p4_hest[2][0]:0)/16384.0, (p4_have_h?p4_hest[2][1]:0)/16384.0,
                    p4_have_h ? "" : "  (no estimate yet -> zeros)"); } }
        for(i=0;i<3;i++) {
            for(j=0;j<2;j++) {
                int hb;
                put_bits(&p, 1, 0); /* start bit */
                /* SIPFAX: LSB-first, like the mask field a few lines above - NOT
                   put_bits()' MSB-first order. Decoding the caller's own h both ways
                   settles it: MSB-first gives |h| = 1.83, 0.50, 1.38, non-decaying, with
                   H(z) zeros at 1.865 and 1.000 (non-minimum-phase - impossible for a
                   channel estimate), while LSB-first gives 0.285, 0.167, 0.102,
                   monotonically decaying, all zeros inside the unit circle. */
                {   /* SIPFAX: advertise OUR OWN estimate, not s->h.
                       s->h holds the PEER's coefficients - we load it from peer_h (see the
                       assignments near the E transition) because we precode OUR transmit
                       with what the peer asked for, which is correct. But this field is the
                       opposite direction: it is what WE want the peer to pre-cancel, and it
                       must come from OUR receiver. Sending s->h echoed the caller's own
                       coefficients straight back, so it precoded its transmission with a
                       description of ITS receive channel - values of 0.12 to 0.29, not the
                       ~0.005 our line actually needs. Tomlinson-Harashima output is
                       deliberately off-lattice (lattice modulo a region), and our receiver
                       has no inverse, so its data arrived uniformly smeared at the right
                       scale: measured median distance to the nearest lattice point 0.508,
                       where 0.50 is no information at all, while Phase 4 through the same
                       front end runs at 3-5% EVM. */
                    s16 hv = p4_have_h ? p4_hest[i][j] : 0;
                    for (hb = 0; hb < 16; hb++)
                        put_bits(&p, 1, (hv >> hb) & 1);
                }
            }
        }
    }
                     
    put_bits(&p, 1, 0); /* start bit */
    put_bits(&p, 16, 0); /* reserved by ITU */

    put_bits(&p, 1, 0); /* start bit */

    {   /* spec CRC: over information bits only (exclude sync/start/fill) */
        u8 cov[200]; int cn = 0, bp, clen = p - (buf + 17);
        for (bp = 17; bp < 17 + clen; bp++) {
            int st = (bp==17 || bp==34) || (type ? (bp>=51 && ((bp-51)%17)==0) : (bp==51 || bp==68));
            if (!st) cov[cn++] = buf[bp];
        }
        crc = calc_crc(cov, cn);
    }
    put_bits(&p, 16, crc);

    put_bits(&p, 1, 0); /* fill bit */
    if (type == 0) {
        put_bits(&p, 2, 0); /* fill bit */
    }

    {   /* SIPFAX: SIPFAX_MPTEST -> dump the generated frame bits for offline checking */
        char *mt = getenv("SIPFAX_MPTEST");
        if (mt) {
            FILE *mf = fopen(mt, "a"); int mi2;
            if (mf) { for (mi2 = 0; mi2 < (p - buf); mi2++) fputc('0' + buf[mi2], mf);
                      fputc('\n', mf); fclose(mf); }
            return;
        }
    }
    /* now we transmit the buffer */
    V34_mod_MP(s, buf, p - buf, s->mp_16point);
}

/* send E sequence */
static void V34_send_E(V34DSPState *s)
{
    u8 buf[20];

    memset(buf, 1, 20);

    V34_mod_MP(s, buf, 20, s->is_16states);
}

/* SIPFAX: stale-J flush latch, in samples. -1 = not yet latched, 0 = done. */
int sipfax_jmute = -1;

/* J sequence */
#define J4POINTS   0x0991
#define J16POINTS  0x0D91
#define JEND       0xF991

static void V34_send_J(V34DSPState *s, int length)
{
    { extern long v34_nj; v34_nj++; }
    int i,val;
    u8 buf[16],*p;

    /* SIPFAX: our J commands the CALLER's Phase-4 constellation (10.1.3.3) - it says
       nothing about our own. slmodem sends J4POINTS while transmitting its own burst
       16-point, and our C receiver reads the caller's 4-point signals, so keep
       commanding 4-point. is_16states is now set from the caller's RECEIVED J (it may
       be 1 while our J stays 4-point), so it must not drive this choice.
       SIPFAX_TX_J16=1 forces J16POINTS for experiments. */
    {   static int txj16 = -1;
        if (txj16 < 0) { char *e = getenv("SIPFAX_TX_J16"); txj16 = (e && atoi(e)) ? 1 : 0; }
        val = txj16 ? J16POINTS : J4POINTS;
    }
    p = buf;
    put_bits(&p, 16, val);
    { extern int v34_dbg; static int once=0; if(v34_dbg && !once){once=1;
        fprintf(stderr,"[enc] J val=0x%04x buf=", val);
        int _k; for(_k=0;_k<16;_k++) fprintf(stderr,"%d",buf[_k]);
        fprintf(stderr,"  is_16states=%d\n", s->is_16states); fflush(stderr); } }
    for(i=0;i<length;i++) {
        V34_mod_MP(s, buf, 16, 0); /* use 4 state modulation */
    }
}

static void V34_send_JP(V34DSPState *s)
{
    u8 buf[16],*p;

    p = buf;
    put_bits(&p, 16, JEND);
    
    V34_mod_MP(s, buf, 16, 0);
}



/* SIPFAX: PHASE-2 -> PHASE-4 TIMING. Getting to data mode late is what the live calls die
   of: slmodem is transmitting data at t=12 and the caller follows, while we arrive at t=16 and
   the caller has already started its abandon tone at t=17. The existing hook logged only a bare
   state number at journal (one-second) resolution, which is too coarse to apportion the 16 s.
   Timestamp every transition off the transmit sample clock instead, and name the states, so a
   single call shows exactly which state each second went to and whether the wait was ours or
   the caller's. */
static const char *v34_state_name(int st)
{
    static const char *n[] = {
        "S3_S1","S3_SINV1","S3_S2","S3_SINV2","S3_PP","S3_TRN","S3_J","S3_JP","S3_WAIT_J",
        "S4_S","S4_WAIT_JP","S4_SINV","S4_TRN","S4_MP","S4_MPP","S4_E","DATA","S3_WAIT_S1" };
    return (st >= 0 && st < (int)(sizeof(n)/sizeof(n[0]))) ? n[st] : "?";
}
long g_txsamp = 0;
long g_txsym_out = 0;   /* SIPFAX: symbols the modulator has actually consumed */
static void V34_mod(V34DSPState *s, s16 *samples, unsigned int nb)
{
    int n;
    g_txsamp += nb;

    for(;;) {
        /* modulate the symbols in the TX queue */
        switch(s->state) {
        /* WAIT_J: modulate the queued J (was raw silence). The answer must SEND J
           until the caller responds (11.3.1.2.4); outer V34_process mutes once the
           caller transmits. */
#if 0
        case V34_START:
            /* 600 bps DPSK modulation */
            

#endif
        default:
            /* V34 modulation */
            while (s->tx_buf_size >= s->tx_filter_wsize && nb > 0) {
                n = V34_baseband_to_carrier(s, samples, nb);
                samples += n;
                nb -= n;
            }
            break;
        }

        if (nb == 0) break;

        /* protocol state machine */

        { extern int v34_dbg; extern long g_txsamp;
          if (v34_dbg && s->state != s->dbg_last2) {
              static long prev = 0;
              fprintf(stderr, "[phase] t=%7.3fs  %-10s -> %-10s  (held %6.3fs)\n",
                      g_txsamp/8000.0, v34_state_name(s->dbg_last2),
                      v34_state_name(s->state), (g_txsamp - prev)/8000.0);
              fflush(stderr); prev = g_txsamp; s->dbg_last2 = s->state; } }
        switch(s->state) {
#if 0
            /* phase 2 */
        case V34_START:
            V34_send_info0(s, 0);
            break;
#endif

            /* phase 3 */
        case V34_STARTUP3_S1:
            {   /* SIPFAX: the tx interpolation filter starts with a zeroed history at
                   Phase-3 entry, and its warmup eats ~20T of the burst head: wire
                   measurement shows our S at 108T in all five captured calls, vs
                   slmodem's spec-nominal 128T. Pad 24 extra S symbols in front so a
                   full 128T of clean S reaches the wire. */
                int pi_;
                s->tx_amp = CALC_AMP(S_POWER);
                for (pi_ = 0; pi_ < 12; pi_++) { put_sym(s, 128, 128); put_sym(s, -128, 128); }
            }
            V34_send_S(s);
            s->state = V34_STARTUP3_SINV1;
            break;
        case V34_STARTUP3_SINV1:
            V34_send_Sinv(s);
            /* we announce MD=0 in INFO1a: per 11.3.1.1.1 the caller expects S, S-bar,
               then PP directly (no second S/S-bar pair) */
            s->state = V34_STARTUP3_PP;
            break;
        case V34_STARTUP3_S2:
            V34_send_S(s);
            s->state = V34_STARTUP3_SINV2;
            break;
        case V34_STARTUP3_SINV2:
            V34_send_Sinv(s);
            s->state = V34_STARTUP3_PP;
            break;
        case V34_STARTUP3_PP:
            V34_send_PP(s);
            s->state = V34_STARTUP3_TRN;
            break;
        case V34_STARTUP3_TRN:
            V34_send_TRN(s);
            /* LONGER TRN: slmodem sends ~1.8-2s of TRN so the caller's equalizer fully
               converges before J; our old single 1024T (0.30s) was too short. Re-queue
               TRN ~6 rounds (~1.8s) before J. Runtime override SIPFAX_P3_TRN. */
            {
                static int trn_target = -1;
                if (trn_target < 0) { char *t = getenv("SIPFAX_P3_TRN"); trn_target = t ? atoi(t) : 6; }
                if (++s->trn_rounds < trn_target) break;
            }
            s->state = V34_STARTUP3_J;
            break;
        case V34_STARTUP3_J:
            V34_send_J(s, 10);
            if (s->calling) {
                s->state = V34_STARTUP3_JP;
            } else {
                s->state = V34_STARTUP3_WAIT_J;
            }
            break;
        case V34_STARTUP3_JP:
            V34_send_JP(s); 
            //s->state = V34_STARTUP4_TRN;
            s->state = V34_STARTUP3_S1;
            break;

        case V34_STARTUP3_WAIT_J:
            if (s->J_received) {
                s->state = V34_STARTUP4_S;
            } else {
                /* 11.3.1.2.4: keep SENDING J until the caller responds (the outer
                   V34_process mutes our output once the caller starts transmitting,
                   giving it the silence the spec requires while it trains us). */
                V34_send_J(s, 10);
            }
            break;
                
            /* phase 4 */
        case V34_STARTUP4_S:
            V34_send_S(s);                       /* 128T (11.4.1.2.1) */
            s->state = V34_STARTUP4_SINV;
            break;
        case V34_STARTUP4_WAIT_JP:               /* unused (J' handled by RX re-hunt) */
            s->state = V34_STARTUP4_SINV;
            break;
        case V34_STARTUP4_SINV:
            V34_send_Sinv(s);                    /* 16T */
            s->p4_trn_tx = 0;
            s->state = V34_STARTUP4_TRN;
            break;

        case V34_STARTUP4_TRN:
            V34_send_TRN(s);                     /* 1024T chunks; >=512T, cap ~1.8s */
            s->p4_trn_tx++;
            {   /* SIPFAX: the caller trains its receiver for OUR signal on this TRN,
                       and 16-point training converges slower - slmodem sends ~1.72 s.
                       An earlier shortcut ended TRN the moment the caller was ready to
                       receive MP (p4_mp_hunt_rx, which the bridge refreshes every block),
                       which cut the TRN to 0.9 s live. Honor the chunk count strictly;
                       default 6 x 1024T = 1.79 s, SIPFAX_P4_TRN_CHUNKS overrides. */
            }
            {
                int want = 6; char *tc = getenv("SIPFAX_P4_TRN_CHUNKS");
                if (tc) { want = atoi(tc); if (want < 1) want = 1; }
                if (s->p4_trn_tx >= want) {
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] TX: TRN done (%d chunks) -> MP\n", s->p4_trn_tx); }
                s->state = V34_STARTUP4_MP;
                }
            }
            break;
        case V34_STARTUP4_MP:
            V34_send_MP(s, 1, 0);                /* repeat until caller's MP arrives */
            if (s->p4_mp_rx) {
                /* SIPFAX: our parameters only become the negotiated ones once the caller's
                   MP has been read. Switching to MP' at that instant means the caller only
                   ever sees ack=0 frames carrying the FALLBACK parameters, and then ack=1
                   frames carrying different ones. Send a few MP(ack=0) frames with the
                   settled parameters first, so what it acknowledges is stable. */
                if (s->mp_hold == 0) {
                    char *mh = getenv("SIPFAX_MP_HOLD");
                    s->mp_hold = (mh ? atoi(mh) : 8);
                    if (s->mp_hold < 1) s->mp_hold = 1;
                    { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] TX: caller MP in -> holding MP(ack=0) for %d frames with negotiated params\n", s->mp_hold); }
                }
                if (--s->mp_hold <= 0) {
                    { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] TX: -> MP'\n"); }
                    s->state = V34_STARTUP4_MPP;
                }
            }
            break;
        case V34_STARTUP4_MPP:
            V34_send_MP(s, 1, 1);                /* MP' until caller's MP' or E */
            if (s->p4_mpp_rx || s->p4_e_rx) {
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] TX: caller MP'/E in -> E\n"); }
                s->state = V34_STARTUP4_E;
            }
            break;
        case V34_STARTUP4_E:
            V34_send_E(s);
            {   /* SIPFAX: SET tx_amp FOR THE DATA CONSTELLATION. It was left at whatever
                   Phase 4 used - CALC_AMP(S_POWER) or CALC_AMP(TRN4_POWER), both calibrated
                   for a 4-point set whose coordinates are only +-1 - and put_sym stores
                   (si * tx_amp) >> 7 into an s16. At tx_amp = 11585 that is 11585 for
                   lattice coordinate 1, but 34755 for coordinate 3 and 81095 for
                   coordinate 7: every data symbol off the innermost ring WRAPPED.

                   Phase 4 never showed it because TRN is all +-1. Data mode uses +-1,+-3 at
                   L=12 and up to +-7 at L=48, so roughly half of every data frame was
                   transmitted with the wrong sign and magnitude. Measured: audio
                   reconstructed from the transmitted symbols correlates 1.0000 with the
                   real output through Phase 4 and -0.12 the moment data starts, and a
                   receiver that tracks TRN symbol-for-symbol at 7% EVM loses all
                   correlation at exactly that boundary.

                   Scale to the constellation actually in use, the same way S_POWER and
                   TRN4_POWER do: mean |c|^2 over the quarter constellation (rotation
                   preserves magnitude, so that is the full set's mean). */
                {   /* SIPFAX: adopt the NEGOTIATED transmit rate before sizing anything.
                       'ac' is the answer-to-call rate, i.e. what WE transmit. */
                    static int cap = -1; int their_ac, Rt;
                    if (cap < 0) { char *mc = getenv("SIPFAX_MP_AC"); cap = mc ? atoi(mc) : 0; }
                    their_ac = s->p4_mp_rate_ac > 0 ? s->p4_mp_rate_ac : 4;
                    Rt = ((cap > 0 && cap < their_ac) ? cap : their_ac) * 2400;
                    if (Rt > 0 && Rt != s->R) {
                        extern int v34_dbg;
                        if (v34_dbg) fprintf(stderr, "[p4] TX: adopting negotiated ac=%d "
                                             "(was R=%d)\n", Rt, s->R);
                        v34_tx_data_params(s, Rt);
                    }
                }
                {   /* SIPFAX: HONOUR THE PEER'S PRECODER REQUEST. The caller's MP carries
                       h = 0.285 / 0.167 / 0.102 (LSB-first, monotonically decaying, all zeros
                       inside the unit circle - a plausible channel estimate) and nonlin=1, and
                       we have been ignoring both: s->h stays zero on the tx instance, so we
                       transmit unprecoded while the caller equalises expecting 9.6.2 output.
                       linmodem already implements the transmit side of 9.6.2 - it is exercised
                       in the loopback through SIPFAX_DL_H - so this is a wiring gap, not
                       missing code. SIPFAX_TX_PRECODE=0 disables.
                       NOTE this is deliberately transmit-only: our RECEIVE inverse is still the
                       known-broken 50.3% path (SIPFAX_RX_PRECODE), but that does not matter
                       here, because the party that has to invert our precoding is the caller. */
                    static int tp = -1; int hq, any = 0;
                    if (tp < 0) { char *e = getenv("SIPFAX_TX_PRECODE"); tp = e ? atoi(e) : 1; }
                    for (hq = 0; hq < 6; hq++) if (s->peer_h[hq]) any = 1;
                    if (tp && any) {
                        extern int v34_dbg;
                        for (hq = 0; hq < 3; hq++) {
                            s->h[hq][0] = s->peer_h[hq*2];
                            s->h[hq][1] = s->peer_h[hq*2+1];
                        }
                        if (v34_dbg) fprintf(stderr, "[p4] TX: precoding with the peer's h = "
                            "%.4f%+.4fj %.4f%+.4fj %.4f%+.4fj\n",
                            s->h[0][0]/16384.0, s->h[0][1]/16384.0,
                            s->h[1][0]/16384.0, s->h[1][1]/16384.0,
                            s->h[2][0]/16384.0, s->h[2][1]/16384.0);
                    }
                }
                {   /* SIPFAX: honour the peer's 9.7 request. use_non_linear was hardcoded
                       0 at every construction site and never set from the MP, so the caller
                       asked for the non-linear encoder on every call and we never even
                       entered the block. SIPFAX_TX_NONLIN=0 disables. */
                    static int nlen = -1;
                    if (nlen < 0) { char *e = getenv("SIPFAX_TX_NONLIN"); nlen = e ? atoi(e) : 1; }
                    if (nlen && s->peer_nonlin) {
                        extern int v34_dbg;
                        s->use_non_linear = 1;
                        if (v34_dbg) fprintf(stderr, "[p4] TX: non-linear encoder ON "
                                             "(peer requested it)\n");
                    }
                }
                int ci, nq = s->L / 4; double acc = 0;
                for (ci = 0; ci < nq; ci++)
                    acc += (double)s->constellation[ci][0]*s->constellation[ci][0]
                         + (double)s->constellation[ci][1]*s->constellation[ci][1];
                if (nq > 0 && acc > 0) {
                    double mp = acc / nq;
                    s->nl_meanc2 = mp;      /* SIPFAX: normalisation for the 9.7 warp */
                    s->tx_amp = CALC_AMP(mp);
                    { extern int v34_dbg; if (v34_dbg)
                        fprintf(stderr, "[p4] TX: data constellation L=%d mean|c|^2=%.2f "
                                "-> tx_amp %d (was %d)\n", s->L, mp, (int)CALC_AMP(mp),
                                s->tx_amp); }
                }
            }
            { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] TX: E sent -> DATA (B1)\n"); }
            s->state = V34_DATA;
            break;
        case V34_DATA:
            /* compute the next 8 baseband symbols */
            encode_mapping_frame(s);
            break;
        }
    }
}

static void V34_mod_init(V34DSPState *s, V34State *p)
{
    V34_init_low(s, p, 1);
    s->state = V34_STARTUP3_S1;
    s->JP_received = 1;
    //    s->state = V34_DATA;
    s->is_16states = 0; /* default 4-point; set to 1 from the caller's RECEIVED J
                           (0x0D91) via the lm-level bridge - it drives TRN, MP and E */
            {   /* SIPFAX: MP/J constellation. Default 4-point: the caller's OWN MP is
                       4-point (our decoder reads it at 2 bits/symbol with CRC OK), so
                       4-point is clearly acceptable on this link. SIPFAX_MP16=1 selects
                       16-point to test that lead without a rebuild. */
                char *m16 = getenv("SIPFAX_MP16");
                char *slc = getenv("SIPFAX_MP_SLCOMPAT");
                s->mp_16point = ((m16 && atoi(m16)) || (slc && atoi(slc))) ? 1 : 0;
            }
}

/*****************************************************/
/* Here begins the real fun ! */

static inline int tcm_decision(int level, int sample)
{
    int x, xs;

    /* find the 4x4 subset */
    x = (sample + (7 * 128) - (level << 8)) >> 10;
    /* convert back to sample */
    xs = (((x<<2) + level - 2) << 8) + 128;
    return xs;
}

static int tcm_dist(int level, int sample)
{
    int xs, e;

    xs = tcm_decision(level,sample);

    e = sample - xs;
#if 0
    fprintf(stderr, "level=%d sample=%d(%d)  xs=%d e=%d e1=%d e2=%d\n", 
           level, sample, (sample >> 8) * 2 + 1, xs, e, 
           sample - (xs + 1024), sample-(xs - 1024));
#endif
    return (e*e) >> 8;
}

/* Viterbi decoder for the V34 trellis coded modulation */

/* SIPFAX: corrected V.34 64-state 4D trellis subset table.
   linmodem's trellis_trans_16 stored only 8 coset-tuples/branch; the Wei 4D
   partition needs 16 (GF(2) dim-4). Regenerated from the validated encoder. */
u8 trellis_trans_16b[512][4] = {
  /* branch 0: trans=0 y0=0 */
  { 0, 0, 0, 0 },
  { 0, 0, 1, 2 },
  { 0, 0, 2, 2 },
  { 0, 0, 3, 0 },
  { 0, 2, 0, 2 },
  { 0, 2, 1, 0 },
  { 0, 2, 2, 0 },
  { 0, 2, 3, 2 },
  { 2, 0, 0, 2 },
  { 2, 0, 1, 0 },
  { 2, 0, 2, 0 },
  { 2, 0, 3, 2 },
  { 2, 2, 0, 0 },
  { 2, 2, 1, 2 },
  { 2, 2, 2, 2 },
  { 2, 2, 3, 0 },
  /* branch 1: trans=1 y0=0 */
  { 0, 0, 0, 3 },
  { 0, 0, 1, 1 },
  { 0, 0, 2, 1 },
  { 0, 0, 3, 3 },
  { 0, 2, 0, 1 },
  { 0, 2, 1, 3 },
  { 0, 2, 2, 3 },
  { 0, 2, 3, 1 },
  { 2, 0, 0, 1 },
  { 2, 0, 1, 3 },
  { 2, 0, 2, 3 },
  { 2, 0, 3, 1 },
  { 2, 2, 0, 3 },
  { 2, 2, 1, 1 },
  { 2, 2, 2, 1 },
  { 2, 2, 3, 3 },
  /* branch 2: trans=2 y0=0 */
  { 1, 0, 1, 0 },
  { 1, 0, 1, 3 },
  { 1, 0, 3, 1 },
  { 1, 0, 3, 2 },
  { 1, 2, 1, 1 },
  { 1, 2, 1, 2 },
  { 1, 2, 3, 0 },
  { 1, 2, 3, 3 },
  { 3, 0, 1, 1 },
  { 3, 0, 1, 2 },
  { 3, 0, 3, 0 },
  { 3, 0, 3, 3 },
  { 3, 2, 1, 0 },
  { 3, 2, 1, 3 },
  { 3, 2, 3, 1 },
  { 3, 2, 3, 2 },
  /* branch 3: trans=3 y0=0 */
  { 1, 0, 0, 1 },
  { 1, 0, 0, 2 },
  { 1, 0, 2, 0 },
  { 1, 0, 2, 3 },
  { 1, 2, 0, 0 },
  { 1, 2, 0, 3 },
  { 1, 2, 2, 1 },
  { 1, 2, 2, 2 },
  { 3, 0, 0, 0 },
  { 3, 0, 0, 3 },
  { 3, 0, 2, 1 },
  { 3, 0, 2, 2 },
  { 3, 2, 0, 1 },
  { 3, 2, 0, 2 },
  { 3, 2, 2, 0 },
  { 3, 2, 2, 3 },
  /* branch 4: trans=4 y0=0 */
  { 0, 0, 0, 2 },
  { 0, 0, 1, 0 },
  { 0, 0, 2, 0 },
  { 0, 0, 3, 2 },
  { 0, 2, 0, 0 },
  { 0, 2, 1, 2 },
  { 0, 2, 2, 2 },
  { 0, 2, 3, 0 },
  { 2, 0, 0, 0 },
  { 2, 0, 1, 2 },
  { 2, 0, 2, 2 },
  { 2, 0, 3, 0 },
  { 2, 2, 0, 2 },
  { 2, 2, 1, 0 },
  { 2, 2, 2, 0 },
  { 2, 2, 3, 2 },
  /* branch 5: trans=5 y0=0 */
  { 0, 0, 0, 1 },
  { 0, 0, 1, 3 },
  { 0, 0, 2, 3 },
  { 0, 0, 3, 1 },
  { 0, 2, 0, 3 },
  { 0, 2, 1, 1 },
  { 0, 2, 2, 1 },
  { 0, 2, 3, 3 },
  { 2, 0, 0, 3 },
  { 2, 0, 1, 1 },
  { 2, 0, 2, 1 },
  { 2, 0, 3, 3 },
  { 2, 2, 0, 1 },
  { 2, 2, 1, 3 },
  { 2, 2, 2, 3 },
  { 2, 2, 3, 1 },
  /* branch 6: trans=6 y0=0 */
  { 1, 0, 1, 1 },
  { 1, 0, 1, 2 },
  { 1, 0, 3, 0 },
  { 1, 0, 3, 3 },
  { 1, 2, 1, 0 },
  { 1, 2, 1, 3 },
  { 1, 2, 3, 1 },
  { 1, 2, 3, 2 },
  { 3, 0, 1, 0 },
  { 3, 0, 1, 3 },
  { 3, 0, 3, 1 },
  { 3, 0, 3, 2 },
  { 3, 2, 1, 1 },
  { 3, 2, 1, 2 },
  { 3, 2, 3, 0 },
  { 3, 2, 3, 3 },
  /* branch 7: trans=7 y0=0 */
  { 1, 0, 0, 0 },
  { 1, 0, 0, 3 },
  { 1, 0, 2, 1 },
  { 1, 0, 2, 2 },
  { 1, 2, 0, 1 },
  { 1, 2, 0, 2 },
  { 1, 2, 2, 0 },
  { 1, 2, 2, 3 },
  { 3, 0, 0, 1 },
  { 3, 0, 0, 2 },
  { 3, 0, 2, 0 },
  { 3, 0, 2, 3 },
  { 3, 2, 0, 0 },
  { 3, 2, 0, 3 },
  { 3, 2, 2, 1 },
  { 3, 2, 2, 2 },
  /* branch 8: trans=8 y0=0 */
  { 1, 1, 0, 3 },
  { 1, 1, 1, 1 },
  { 1, 1, 2, 1 },
  { 1, 1, 3, 3 },
  { 1, 3, 0, 1 },
  { 1, 3, 1, 3 },
  { 1, 3, 2, 3 },
  { 1, 3, 3, 1 },
  { 3, 1, 0, 1 },
  { 3, 1, 1, 3 },
  { 3, 1, 2, 3 },
  { 3, 1, 3, 1 },
  { 3, 3, 0, 3 },
  { 3, 3, 1, 1 },
  { 3, 3, 2, 1 },
  { 3, 3, 3, 3 },
  /* branch 9: trans=9 y0=0 */
  { 1, 1, 0, 0 },
  { 1, 1, 1, 2 },
  { 1, 1, 2, 2 },
  { 1, 1, 3, 0 },
  { 1, 3, 0, 2 },
  { 1, 3, 1, 0 },
  { 1, 3, 2, 0 },
  { 1, 3, 3, 2 },
  { 3, 1, 0, 2 },
  { 3, 1, 1, 0 },
  { 3, 1, 2, 0 },
  { 3, 1, 3, 2 },
  { 3, 3, 0, 0 },
  { 3, 3, 1, 2 },
  { 3, 3, 2, 2 },
  { 3, 3, 3, 0 },
  /* branch 10: trans=10 y0=0 */
  { 0, 1, 0, 1 },
  { 0, 1, 0, 2 },
  { 0, 1, 2, 0 },
  { 0, 1, 2, 3 },
  { 0, 3, 0, 0 },
  { 0, 3, 0, 3 },
  { 0, 3, 2, 1 },
  { 0, 3, 2, 2 },
  { 2, 1, 0, 0 },
  { 2, 1, 0, 3 },
  { 2, 1, 2, 1 },
  { 2, 1, 2, 2 },
  { 2, 3, 0, 1 },
  { 2, 3, 0, 2 },
  { 2, 3, 2, 0 },
  { 2, 3, 2, 3 },
  /* branch 11: trans=11 y0=0 */
  { 0, 1, 1, 0 },
  { 0, 1, 1, 3 },
  { 0, 1, 3, 1 },
  { 0, 1, 3, 2 },
  { 0, 3, 1, 1 },
  { 0, 3, 1, 2 },
  { 0, 3, 3, 0 },
  { 0, 3, 3, 3 },
  { 2, 1, 1, 1 },
  { 2, 1, 1, 2 },
  { 2, 1, 3, 0 },
  { 2, 1, 3, 3 },
  { 2, 3, 1, 0 },
  { 2, 3, 1, 3 },
  { 2, 3, 3, 1 },
  { 2, 3, 3, 2 },
  /* branch 12: trans=12 y0=0 */
  { 1, 1, 0, 1 },
  { 1, 1, 1, 3 },
  { 1, 1, 2, 3 },
  { 1, 1, 3, 1 },
  { 1, 3, 0, 3 },
  { 1, 3, 1, 1 },
  { 1, 3, 2, 1 },
  { 1, 3, 3, 3 },
  { 3, 1, 0, 3 },
  { 3, 1, 1, 1 },
  { 3, 1, 2, 1 },
  { 3, 1, 3, 3 },
  { 3, 3, 0, 1 },
  { 3, 3, 1, 3 },
  { 3, 3, 2, 3 },
  { 3, 3, 3, 1 },
  /* branch 13: trans=13 y0=0 */
  { 1, 1, 0, 2 },
  { 1, 1, 1, 0 },
  { 1, 1, 2, 0 },
  { 1, 1, 3, 2 },
  { 1, 3, 0, 0 },
  { 1, 3, 1, 2 },
  { 1, 3, 2, 2 },
  { 1, 3, 3, 0 },
  { 3, 1, 0, 0 },
  { 3, 1, 1, 2 },
  { 3, 1, 2, 2 },
  { 3, 1, 3, 0 },
  { 3, 3, 0, 2 },
  { 3, 3, 1, 0 },
  { 3, 3, 2, 0 },
  { 3, 3, 3, 2 },
  /* branch 14: trans=14 y0=0 */
  { 0, 1, 0, 0 },
  { 0, 1, 0, 3 },
  { 0, 1, 2, 1 },
  { 0, 1, 2, 2 },
  { 0, 3, 0, 1 },
  { 0, 3, 0, 2 },
  { 0, 3, 2, 0 },
  { 0, 3, 2, 3 },
  { 2, 1, 0, 1 },
  { 2, 1, 0, 2 },
  { 2, 1, 2, 0 },
  { 2, 1, 2, 3 },
  { 2, 3, 0, 0 },
  { 2, 3, 0, 3 },
  { 2, 3, 2, 1 },
  { 2, 3, 2, 2 },
  /* branch 15: trans=15 y0=0 */
  { 0, 1, 1, 1 },
  { 0, 1, 1, 2 },
  { 0, 1, 3, 0 },
  { 0, 1, 3, 3 },
  { 0, 3, 1, 0 },
  { 0, 3, 1, 3 },
  { 0, 3, 3, 1 },
  { 0, 3, 3, 2 },
  { 2, 1, 1, 0 },
  { 2, 1, 1, 3 },
  { 2, 1, 3, 1 },
  { 2, 1, 3, 2 },
  { 2, 3, 1, 1 },
  { 2, 3, 1, 2 },
  { 2, 3, 3, 0 },
  { 2, 3, 3, 3 },
  /* branch 16: trans=0 y0=1 */
  { 0, 0, 0, 0 },
  { 0, 0, 1, 2 },
  { 0, 0, 2, 2 },
  { 0, 0, 3, 0 },
  { 0, 2, 0, 2 },
  { 0, 2, 1, 0 },
  { 0, 2, 2, 0 },
  { 0, 2, 3, 2 },
  { 2, 0, 0, 2 },
  { 2, 0, 1, 0 },
  { 2, 0, 2, 0 },
  { 2, 0, 3, 2 },
  { 2, 2, 0, 0 },
  { 2, 2, 1, 2 },
  { 2, 2, 2, 2 },
  { 2, 2, 3, 0 },
  /* branch 17: trans=1 y0=1 */
  { 0, 0, 0, 3 },
  { 0, 0, 1, 1 },
  { 0, 0, 2, 1 },
  { 0, 0, 3, 3 },
  { 0, 2, 0, 1 },
  { 0, 2, 1, 3 },
  { 0, 2, 2, 3 },
  { 0, 2, 3, 1 },
  { 2, 0, 0, 1 },
  { 2, 0, 1, 3 },
  { 2, 0, 2, 3 },
  { 2, 0, 3, 1 },
  { 2, 2, 0, 3 },
  { 2, 2, 1, 1 },
  { 2, 2, 2, 1 },
  { 2, 2, 3, 3 },
  /* branch 18: trans=2 y0=1 */
  { 1, 0, 1, 0 },
  { 1, 0, 1, 3 },
  { 1, 0, 3, 1 },
  { 1, 0, 3, 2 },
  { 1, 2, 1, 1 },
  { 1, 2, 1, 2 },
  { 1, 2, 3, 0 },
  { 1, 2, 3, 3 },
  { 3, 0, 1, 1 },
  { 3, 0, 1, 2 },
  { 3, 0, 3, 0 },
  { 3, 0, 3, 3 },
  { 3, 2, 1, 0 },
  { 3, 2, 1, 3 },
  { 3, 2, 3, 1 },
  { 3, 2, 3, 2 },
  /* branch 19: trans=3 y0=1 */
  { 1, 0, 0, 1 },
  { 1, 0, 0, 2 },
  { 1, 0, 2, 0 },
  { 1, 0, 2, 3 },
  { 1, 2, 0, 0 },
  { 1, 2, 0, 3 },
  { 1, 2, 2, 1 },
  { 1, 2, 2, 2 },
  { 3, 0, 0, 0 },
  { 3, 0, 0, 3 },
  { 3, 0, 2, 1 },
  { 3, 0, 2, 2 },
  { 3, 2, 0, 1 },
  { 3, 2, 0, 2 },
  { 3, 2, 2, 0 },
  { 3, 2, 2, 3 },
  /* branch 20: trans=4 y0=1 */
  { 0, 0, 0, 2 },
  { 0, 0, 1, 0 },
  { 0, 0, 2, 0 },
  { 0, 0, 3, 2 },
  { 0, 2, 0, 0 },
  { 0, 2, 1, 2 },
  { 0, 2, 2, 2 },
  { 0, 2, 3, 0 },
  { 2, 0, 0, 0 },
  { 2, 0, 1, 2 },
  { 2, 0, 2, 2 },
  { 2, 0, 3, 0 },
  { 2, 2, 0, 2 },
  { 2, 2, 1, 0 },
  { 2, 2, 2, 0 },
  { 2, 2, 3, 2 },
  /* branch 21: trans=5 y0=1 */
  { 0, 0, 0, 1 },
  { 0, 0, 1, 3 },
  { 0, 0, 2, 3 },
  { 0, 0, 3, 1 },
  { 0, 2, 0, 3 },
  { 0, 2, 1, 1 },
  { 0, 2, 2, 1 },
  { 0, 2, 3, 3 },
  { 2, 0, 0, 3 },
  { 2, 0, 1, 1 },
  { 2, 0, 2, 1 },
  { 2, 0, 3, 3 },
  { 2, 2, 0, 1 },
  { 2, 2, 1, 3 },
  { 2, 2, 2, 3 },
  { 2, 2, 3, 1 },
  /* branch 22: trans=6 y0=1 */
  { 1, 0, 1, 1 },
  { 1, 0, 1, 2 },
  { 1, 0, 3, 0 },
  { 1, 0, 3, 3 },
  { 1, 2, 1, 0 },
  { 1, 2, 1, 3 },
  { 1, 2, 3, 1 },
  { 1, 2, 3, 2 },
  { 3, 0, 1, 0 },
  { 3, 0, 1, 3 },
  { 3, 0, 3, 1 },
  { 3, 0, 3, 2 },
  { 3, 2, 1, 1 },
  { 3, 2, 1, 2 },
  { 3, 2, 3, 0 },
  { 3, 2, 3, 3 },
  /* branch 23: trans=7 y0=1 */
  { 1, 0, 0, 0 },
  { 1, 0, 0, 3 },
  { 1, 0, 2, 1 },
  { 1, 0, 2, 2 },
  { 1, 2, 0, 1 },
  { 1, 2, 0, 2 },
  { 1, 2, 2, 0 },
  { 1, 2, 2, 3 },
  { 3, 0, 0, 1 },
  { 3, 0, 0, 2 },
  { 3, 0, 2, 0 },
  { 3, 0, 2, 3 },
  { 3, 2, 0, 0 },
  { 3, 2, 0, 3 },
  { 3, 2, 2, 1 },
  { 3, 2, 2, 2 },
  /* branch 24: trans=8 y0=1 */
  { 1, 1, 0, 3 },
  { 1, 1, 1, 1 },
  { 1, 1, 2, 1 },
  { 1, 1, 3, 3 },
  { 1, 3, 0, 1 },
  { 1, 3, 1, 3 },
  { 1, 3, 2, 3 },
  { 1, 3, 3, 1 },
  { 3, 1, 0, 1 },
  { 3, 1, 1, 3 },
  { 3, 1, 2, 3 },
  { 3, 1, 3, 1 },
  { 3, 3, 0, 3 },
  { 3, 3, 1, 1 },
  { 3, 3, 2, 1 },
  { 3, 3, 3, 3 },
  /* branch 25: trans=9 y0=1 */
  { 1, 1, 0, 0 },
  { 1, 1, 1, 2 },
  { 1, 1, 2, 2 },
  { 1, 1, 3, 0 },
  { 1, 3, 0, 2 },
  { 1, 3, 1, 0 },
  { 1, 3, 2, 0 },
  { 1, 3, 3, 2 },
  { 3, 1, 0, 2 },
  { 3, 1, 1, 0 },
  { 3, 1, 2, 0 },
  { 3, 1, 3, 2 },
  { 3, 3, 0, 0 },
  { 3, 3, 1, 2 },
  { 3, 3, 2, 2 },
  { 3, 3, 3, 0 },
  /* branch 26: trans=10 y0=1 */
  { 0, 1, 0, 1 },
  { 0, 1, 0, 2 },
  { 0, 1, 2, 0 },
  { 0, 1, 2, 3 },
  { 0, 3, 0, 0 },
  { 0, 3, 0, 3 },
  { 0, 3, 2, 1 },
  { 0, 3, 2, 2 },
  { 2, 1, 0, 0 },
  { 2, 1, 0, 3 },
  { 2, 1, 2, 1 },
  { 2, 1, 2, 2 },
  { 2, 3, 0, 1 },
  { 2, 3, 0, 2 },
  { 2, 3, 2, 0 },
  { 2, 3, 2, 3 },
  /* branch 27: trans=11 y0=1 */
  { 0, 1, 1, 0 },
  { 0, 1, 1, 3 },
  { 0, 1, 3, 1 },
  { 0, 1, 3, 2 },
  { 0, 3, 1, 1 },
  { 0, 3, 1, 2 },
  { 0, 3, 3, 0 },
  { 0, 3, 3, 3 },
  { 2, 1, 1, 1 },
  { 2, 1, 1, 2 },
  { 2, 1, 3, 0 },
  { 2, 1, 3, 3 },
  { 2, 3, 1, 0 },
  { 2, 3, 1, 3 },
  { 2, 3, 3, 1 },
  { 2, 3, 3, 2 },
  /* branch 28: trans=12 y0=1 */
  { 1, 1, 0, 1 },
  { 1, 1, 1, 3 },
  { 1, 1, 2, 3 },
  { 1, 1, 3, 1 },
  { 1, 3, 0, 3 },
  { 1, 3, 1, 1 },
  { 1, 3, 2, 1 },
  { 1, 3, 3, 3 },
  { 3, 1, 0, 3 },
  { 3, 1, 1, 1 },
  { 3, 1, 2, 1 },
  { 3, 1, 3, 3 },
  { 3, 3, 0, 1 },
  { 3, 3, 1, 3 },
  { 3, 3, 2, 3 },
  { 3, 3, 3, 1 },
  /* branch 29: trans=13 y0=1 */
  { 1, 1, 0, 2 },
  { 1, 1, 1, 0 },
  { 1, 1, 2, 0 },
  { 1, 1, 3, 2 },
  { 1, 3, 0, 0 },
  { 1, 3, 1, 2 },
  { 1, 3, 2, 2 },
  { 1, 3, 3, 0 },
  { 3, 1, 0, 0 },
  { 3, 1, 1, 2 },
  { 3, 1, 2, 2 },
  { 3, 1, 3, 0 },
  { 3, 3, 0, 2 },
  { 3, 3, 1, 0 },
  { 3, 3, 2, 0 },
  { 3, 3, 3, 2 },
  /* branch 30: trans=14 y0=1 */
  { 0, 1, 0, 0 },
  { 0, 1, 0, 3 },
  { 0, 1, 2, 1 },
  { 0, 1, 2, 2 },
  { 0, 3, 0, 1 },
  { 0, 3, 0, 2 },
  { 0, 3, 2, 0 },
  { 0, 3, 2, 3 },
  { 2, 1, 0, 1 },
  { 2, 1, 0, 2 },
  { 2, 1, 2, 0 },
  { 2, 1, 2, 3 },
  { 2, 3, 0, 0 },
  { 2, 3, 0, 3 },
  { 2, 3, 2, 1 },
  { 2, 3, 2, 2 },
  /* branch 31: trans=15 y0=1 */
  { 0, 1, 1, 1 },
  { 0, 1, 1, 2 },
  { 0, 1, 3, 0 },
  { 0, 1, 3, 3 },
  { 0, 3, 1, 0 },
  { 0, 3, 1, 3 },
  { 0, 3, 3, 1 },
  { 0, 3, 3, 2 },
  { 2, 1, 1, 0 },
  { 2, 1, 1, 3 },
  { 2, 1, 3, 1 },
  { 2, 1, 3, 2 },
  { 2, 3, 1, 1 },
  { 2, 3, 1, 2 },
  { 2, 3, 3, 0 },
  { 2, 3, 3, 3 },
};

static void trellis_decoder(V34DSPState *s, s16 yout[2][2], s16 yy[2][2], 
                             int *mse)
{
    int u0_thresh = 128;   /* SIPFAX: see nb_trans below */
    int i, j, k, n, nbbt, nb_trans, state, next_state, error, trellis_ptr;
    int error_table[32],decision_table[32],emin,jmin,u0,x,y;
    u8 *p,*q;

    trellis_ptr = s->trellis_ptr;
    v0_base_init();

    /* compute the number of bits used in the transitions from each state */
    switch(s->conv_nb_states) {
    case 16:
        nbbt = 2;
        p = &trellis_trans_4[0][0];
        break;
    case 32:
        nbbt = 3;
        p = &trellis_trans_8[0][0];
        break;
    default:
        nbbt = 4;
        /* SIPFAX: table selection. 1 (DEFAULT) is linmodem's original trellis_trans_16
           (256 rows, 8 coset-tuples per branch, n = 128>>nbbt); 0 is the regenerated
           trellis_trans_16b (512 rows, 16 per branch); 2/3 build it at runtime.
           MEASURED, by scoring the Viterbi survivor path against the encoder's own state
           sequence (SIPFAX_SURVDUMP vs SIPFAX_ENCDUMP, exhaustive over all delays, scored
           permutation-invariantly against a random-data floor of 3.76%):

               table 1 (upstream)      99.79% state tracking
               table 0 (regenerated)    4.12%  - chance, not even a bijection
               tables 2/3 (built)       4.09% / 4.05%

           So the 64-state Wei decoder works, and the regenerated table is what broke it.
           The regenerated table was introduced on the argument that the Wei 4D partition
           needs 16 tuples per branch rather than 8; that argument is refuted above.
           Under table 1 the decoder's state numbering differs from the encoder's by a
           single inverted bit (pi(s) = s ^ 32), a perfect bijection with 99.8% row purity.
           NOTE bit counts cannot detect any of this: table 0 scores 100% of bits with a
           dead trellis because the slicer carries the decode. */
        { static int torig = -1;
          if (torig < 0) { char *e = getenv("SIPFAX_TRELLIS_ORIG"); torig = e ? atoi(e) : 1; }
          if (torig == 2 || torig == 3) {
              /* SIPFAX: BUILD the subset table from the encoder's own coset->trans logic,
                 so it is correct by construction. Measured against the encoder, the stored
                 trellis_trans_16b puts the emitted tuple in the block the ACS scores for
                 that branch only 26.3% of the time - its row order disagrees with
                 trans = (Y[3]<<3)|(Y[4]<<2)|(Y[2]<<1)|Y[1], so every branch metric is
                 attached to the wrong branch. Each coset 4-tuple determines trans uniquely
                 while y0 is free (it is the state's LSB), so the two y0 halves must hold
                 the SAME 16 tuples, and 256 tuples over 16 trans values gives exactly 16
                 per block. */
              static u8 built[512][4]; static int done = 0;
              if (!done) {
                  int c[4], cnt[16], t9, q9;
                  for (t9 = 0; t9 < 16; t9++) cnt[t9] = 0;
                  for (c[0]=0;c[0]<4;c[0]++) for (c[1]=0;c[1]<4;c[1]++)
                  for (c[2]=0;c[2]<4;c[2]++) for (c[3]=0;c[3]<4;c[3]++) {
                      int ss[2][3], YY[5], tr, i9;
                      for (i9 = 0; i9 < 2; i9++) {
                          int x = c[i9*2], y = c[i9*2+1];
                          int x0 = x & 1, x1 = (x & 2) >> 1;
                          int y0 = y & 1, y1 = (y & 2) >> 1;
                          ss[i9][2] = x1 ^ y1 ^ y0 ^ x0;
                          ss[i9][1] = y0;
                          ss[i9][0] = y0 ^ x0;
                      }
                      YY[4] = ss[0][2] ^ ss[1][2];
                      YY[3] = ss[0][1];
                      YY[2] = ss[0][0];
                      YY[1] = (ss[0][0] & ~ss[1][0] & 1) ^ ss[0][1] ^ ss[1][1];
                      tr = (YY[3]<<3) | (YY[4]<<2) | (YY[2]<<1) | YY[1];
                      if (cnt[tr] < 16) {
                          for (q9 = 0; q9 < 4; q9++) {
                              built[tr*16 + cnt[tr]][q9]        = (u8)c[q9];
                              built[(tr+16)*16 + cnt[tr]][q9]   = (u8)c[q9];
                          }
                          cnt[tr]++;
                      }
                  }
                  { extern int v34_dbg; if (v34_dbg) {
                      int bad = 0;
                      for (t9 = 0; t9 < 16; t9++) if (cnt[t9] != 16) bad++;
                      fprintf(stderr, "[acs] built subset table: %d of 16 branches did NOT "
                              "get exactly 16 tuples\n", bad); } }
                  done = 1;
              }
              p = &built[0][0];
          } else
          p = torig ? &trellis_trans_16[0][0] : &trellis_trans_16b[0][0]; }
        break;
    }
    nb_trans = 1 << nbbt;
    {   /* SIPFAX: the u0/rotation flag is "i >= nb_trans" packed into
           decision_table[i] = i*n + jmin, so its threshold is nb_trans*n. Upstream had
           n = 128>>nbbt, giving 16*8 = 128 = 2^7 at 64 states - hence the >>7 tests below.
           This tree sets n = 16 for the 64-state code (16 coset-tuples per branch), which
           moves the threshold to 16*16 = 256 = 2^8, and state_decision was widened to s16
           precisely because the values now reach 511. The >>7 tests were not updated, so on
           a 64-state connection they evaluate to i>>3 (0..3) instead of a 0/1 flag: the
           first condition fires for every i >= 8 and the second XORs a 0/1 u0 against a
           0..3 value. Invisible in loopback - at infinite SNR the surviving branch and
           u0_memory agree, so the two rotations cancel and (2*I0+U0)>>1 == I0 either way -
           but it bites at real SNR, which is exactly where we are. Derive the threshold
           instead of hardcoding a shift. */
        int ndec = 128 >> nbbt;
        static int torig3 = -1;
        if (torig3 < 0) { char *e = getenv("SIPFAX_TRELLIS_ORIG"); torig3 = e ? atoi(e) : 1; }
        if (!torig3 && s->conv_nb_states >= 64) ndec = 16;
        u0_thresh = nb_trans * ndec;
    }

    /* write a previous decoded symbol : extract a decoded bit from
       the beginning of a path */

    k = trellis_ptr;
    k--;
    if (k < 0) k = TRELLIS_LENGTH-1;
    /* start traceback from the MINIMUM-metric survivor (not arbitrary j=0):
       survivors have not necessarily merged, so the wrong start gives wrong bits */
    { int bs=0, be=s->state_error[0], st; for(st=1;st<s->conv_nb_states;st++) if(s->state_error[st]<be){be=s->state_error[st];bs=st;} j=bs; }
    { extern int g_surv_bs; g_surv_bs = j; }
    for(i=0;i<(TRELLIS_LENGTH-1);i++) {
        j = s->state_path[j][k];
        k--;
        if (k < 0) k = TRELLIS_LENGTH-1;
    }
    {   /* SIPFAX: SURVIVOR-PATH SCORE. The existing TRUESTATE probe scores the
           INSTANTANEOUS argmin; the Viterbi's actual output comes from the traceback,
           which merges. Dump both: g_surv_bs is the min-metric state at time t (the
           traceback start), j is the endpoint after walking back TRELLIS_LENGTH-1
           through state_path. Scored offline against SIPFAX_ENCDUMP field 7 over a
           delay sweep, so no in-C alignment assumption is baked in. */
        extern FILE *g_survf; extern int g_surv_bs;
        if (!g_survf) { char *e = getenv("SIPFAX_SURVDUMP"); if (e) g_survf = fopen(e,"w"); }
        if (g_survf) fprintf(g_survf, "%d %d\n", g_surv_bs, j);
    }
    {   /* SIPFAX: u0_memory is WRITTEN at trellis_ptr (the current symbol) but the symbol
           being emitted here is the traceback endpoint, TRELLIS_LENGTH-1 symbols earlier at
           index k. Pairing this u0 with state_decision[j][k] therefore crosses a 29-symbol
           gap. The existing note above says the two rotations cancel when the surviving
           branch and u0_memory agree - true at the same instant, not across the traceback.
           SIPFAX_U0IDX=1 reads u0 at k instead; 0 keeps the old behaviour. */
        static int u0idx = -1;
        if (u0idx < 0) { char *e = getenv("SIPFAX_U0IDX"); u0idx = e ? atoi(e) : 0; }
        u0 = u0idx ? s->u0_memory[k] : s->u0_memory[trellis_ptr];
    }
    /* SIPFAX: Y0 of the symbol being emitted now is the LSB of the survivor state at the
       traceback point - the same state the encoder's conv_reg held when it produced it. */
    {   /* SIPFAX: COMPUTE Y[0] the way the encoder does, rather than reading a stored
           state's LSB. The encoder does Y[0] = conv_reg & 1 AFTER
           conv_reg = trellis_next_state(nb, conv_reg, trans), so recover both inputs here:
           the predecessor state from state_path, and the branch from state_decision, which
           the ACS wrote as decision_table[trans + n] = (trans+n)*ndec + jmin with n either
           0 or nb_trans. SIPFAX_Y0MODE=0 keeps the old j&1 for comparison.
           NOTE the algebra says these are the same bit - trellis_next_state(prev,trans) IS
           the arrival state j, since state_decision/state_path are indexed by next_state -
           so this is expected to change nothing. Measuring rather than asserting, because
           that reasoning is what has been wrong five times. */
        static int y0mode = -1;
        if (y0mode < 0) { char *em = getenv("SIPFAX_Y0MODE"); y0mode = em ? atoi(em) : 1; }
        if (y0mode) {
            int ndec = 128 >> nbbt;
            int prev, tr, ns;
            /* SIPFAX: must match the ndec the ACS used when it WROTE decision_table
               (see the u0_thresh block above) - that site forces 16 only for the stored
               table (torig3 == 0). Reading it back with a different divisor yields a
               garbage branch index, which is why Y0 was uncorrelated on every table but
               the default. */
            { static int tz = -1;
              if (tz < 0) { char *ez = getenv("SIPFAX_TRELLIS_ORIG"); tz = ez ? atoi(ez) : 1; }
              if (!tz && s->conv_nb_states >= 64) ndec = 16; }
            prev = s->state_path[j][k];
            tr   = (s->state_decision[j][k] / ndec) % nb_trans;
            ns   = trellis_next_state(s->conv_nb_states, prev, tr);
            s->y0_out = ns & 1;
            /* SIPFAX: carry the states themselves so the decoder's numbering can be
               compared against the encoder's conv_reg sequence directly. The symbols
               decode 100% correctly, so the surviving path must track the encoder's
               states - if these sequences disagree, the numbering does. */
            s->st_arr = j; s->st_prev = prev;
            {   /* SIPFAX: v0 estimate for the symbol emitted here (index g_sym-(TL-1)).
                   u0_memory[k] is U0 at that time and (j & 1) is its conv_reg parity. */
                extern long g_sym, g_v0hn; extern int g_v0h[]; extern int g_v0lock;
                extern int g_u0hist[];
                long ie = g_sym - (TRELLIS_LENGTH - 1);
                if (!g_v0lock && ie >= 0) {
                    int est = g_u0hist[ie & (U0H-1)] ^ (j & 1);
                    if (est) g_v0h[(int)(ie % 480)]++;
                    g_v0hn++;
                }
            }
            { extern long g_y0same, g_y0tot; g_y0tot++; if ((ns & 1) == (j & 1)) g_y0same++; }
        } else {
            s->y0_out = j & 1;
        }
    }
    {   /* SIPFAX: recover the superframe sync bit. The encoder returns
           U0 = Y[0] ^ c0 ^ v0, with Y[0] = conv_reg & 1 and c0 = 0 when not precoding, so
           v0 = U0 ^ Y0 is available here: u0 above, and Y0 as the LSB of the current
           minimum-metric survivor. v0 is the SYNC_PATTERN bit at sync_count == 0, which is
           what V.34 provides for mapping-frame alignment and what this decoder currently
           computes and discards. */
        extern int g_v0est, g_v0have;
        int bs2 = 0, be2 = s->state_error[0], st2;
        for (st2 = 1; st2 < s->conv_nb_states; st2++)
            if (s->state_error[st2] < be2) { be2 = s->state_error[st2]; bs2 = st2; }
        g_v0est = u0 ^ (bs2 & 1);
        g_v0have = 1;
    }
    q = p + (s->state_decision[j][k] * 4);
#if 1
    { char *nt=getenv("SIPFAX_NOTRELLIS");
      if (nt && atoi(nt)) {
        yout[0][0] = s->state_memory[trellis_ptr][0]; yout[0][1] = s->state_memory[trellis_ptr][1];
        yout[1][0] = s->state_memory[trellis_ptr][2]; yout[1][1] = s->state_memory[trellis_ptr][3];
      } else {
        yout[0][0] = tcm_decision(q[0], s->state_memory[trellis_ptr][0]);
        yout[0][1] = tcm_decision(q[1], s->state_memory[trellis_ptr][1]);
        yout[1][0] = tcm_decision(q[2], s->state_memory[trellis_ptr][2]);
        yout[1][1] = tcm_decision(q[3], s->state_memory[trellis_ptr][3]);
        {   /* SIPFAX: COSET CORRECTNESS. In a noise-free symbol loopback the unconstrained
               nearest point IS the transmitted point, so the level that minimises tcm_dist
               over l = 0..3 is the level the encoder actually sent. Comparing that against
               the level the trellis chose (q[]) measures directly whether the branch is
               selecting the right coset - no ground-truth file or alignment needed. */
            extern long g_cs_tot, g_cs_ok, g_cs_all4;
            static int cschk = -1; int ci4, ok4 = 0;
            if (cschk < 0) { char *e = getenv("SIPFAX_COSETCHK"); cschk = e ? atoi(e) : 0; }
            if (cschk) {
                for (ci4 = 0; ci4 < 4; ci4++) {
                    s16 sm = s->state_memory[trellis_ptr][ci4];
                    int l4, bl = 0, bd = 0x7fffffff, d4;
                    for (l4 = 0; l4 < 4; l4++) {
                        d4 = tcm_dist(l4, sm);
                        if (d4 < bd) { bd = d4; bl = l4; }
                    }
                    g_cs_tot++;
                    if (bl == q[ci4]) { g_cs_ok++; ok4++; }
                }
                if (ok4 == 4) g_cs_all4++;
            }
        }
      } }
    /* undo the rotation */    
    if (s->state_decision[j][k] >= u0_thresh) {
        x = yout[1][1];
        y = - yout[1][0];
        yout[1][0] = x;
        yout[1][1] = y;
    }
    /* rotate only if u0 is set */
    if (u0 ^ (s->state_decision[j][k] >= u0_thresh)) {
        x = - yout[1][1];
        y = yout[1][0];
        yout[1][0] = x;
        yout[1][1] = y;
    }
#else
    /* no trellis */
    yout[0][0] = s->state_memory[trellis_ptr][0];
    yout[0][1] = s->state_memory[trellis_ptr][1];
    yout[1][0] = s->state_memory[trellis_ptr][2];
    yout[1][1] = s->state_memory[trellis_ptr][3];
#endif
    /* compute the mean square error (normalized to 2^7) */
    *mse = (dsp_sqr(yout[0][0] - s->state_memory[trellis_ptr][0]) + 
        dsp_sqr(yout[0][1] - s->state_memory[trellis_ptr][1]) + 
        dsp_sqr(yout[1][0] - s->state_memory[trellis_ptr][2]) + 
        dsp_sqr(yout[1][1] - s->state_memory[trellis_ptr][3])) >> 7;

    s->state_memory[trellis_ptr][0] = yy[0][0];
    s->state_memory[trellis_ptr][1] = yy[0][1];
    s->state_memory[trellis_ptr][2] = yy[1][0];
    s->state_memory[trellis_ptr][3] = yy[1][1];

    /* compute the error table */
    /* XXX: may be optimized by using the algebraic properties of the mapping */    
    n = 128 >> nbbt;
    {   /* SIPFAX: the tuple count must match the table selected above. */
        static int torig2 = -1;
        if (torig2 < 0) { char *e = getenv("SIPFAX_TRELLIS_ORIG"); torig2 = e ? atoi(e) : 1; }
        if (!torig2 && s->conv_nb_states >= 64) n = 16;
    }
    { extern int g_ntup; g_ntup = n; }
    jmin = 0; /* no warning */
    for(i=0;i<(nb_trans*2);i++) {
        emin = 0x7fffffff;
        for(j=0;j<n;j++) {
            int e;
            e = tcm_dist(p[0], yy[0][0]) + 
                tcm_dist(p[1], yy[0][1]) +
                tcm_dist(p[2], yy[1][0]) +
                tcm_dist(p[3], yy[1][1]);
            if (e < emin) {
                emin = e;
                jmin = j;
            }
            p+=4;
        }
        error_table[i] = emin;
        decision_table[i] = i * n + jmin;
        {   /* SIPFAX: do the branch metrics discriminate at all? The ACS reports every one
               of the 64 states tied for minimum (rank 0, gap 0) while picking the true one
               only 1/64 of the time, which means error_table carries no information. With
               16 coset-tuples per branch and only 4 cosets per 2D coordinate, every branch
               may contain a tuple at the nearest point, so all branches tie and the trellis
               degenerates to the slicer. */
            extern long g_et_n; extern double g_et_min, g_et_max, g_et_sum;
            if (i == 0) { g_et_min = 1e30; g_et_max = -1e30; }
            if (emin < g_et_min) g_et_min = emin;
            if (emin > g_et_max) g_et_max = emin;
            if (i == nb_trans*2 - 1) {
                extern double g_et_spread; extern long g_et_zero;
                g_et_spread += (g_et_max - g_et_min); g_et_n++;
                if (g_et_max == g_et_min) g_et_zero++;
            }
        }
    }

    {   /* SIPFAX: is the per-symbol branch information unambiguous? trellis_trans_16 is a
           PARTITION of the 256 coset tuples into 32 blocks of 8, so in a noise-free loopback
           exactly one branch can contain the transmitted tuple and its error_table entry must
           be exactly 0, every other branch strictly positive. If that holds, the ACS is being
           handed perfect information and any wrong branch is an ACS/traceback fault. If the
           minimum is NOT 0, the transmitted tuple is in no scored block - a level-mapping or
           half-selection fault instead. */
        extern long g_z_n, g_z_min0, g_z_cnt, g_z_hist[8]; extern double g_z_minv;
        static int zchk = -1; int zi, zc = 0, zmin = 0x7fffffff;
        if (zchk < 0) { char *e = getenv("SIPFAX_ZEROCHK"); zchk = e ? atoi(e) : 0; }
        if (zchk) {
            for (zi = 0; zi < nb_trans*2; zi++) {
                if (error_table[zi] < zmin) zmin = error_table[zi];
                if (error_table[zi] == 0) zc++;
            }
            g_z_n++; g_z_cnt += zc; g_z_minv += zmin;
            if (zmin == 0) g_z_min0++;
            g_z_hist[zc < 7 ? zc : 7]++;
        }
    }
    /* we compute the bit u0 (needed for synchronization & c0 estimation) */
    jmin = 0;
    emin = 0x7fffffff;
    for(i=0;i<nb_trans*2;i++) {
        if (error_table[i] < emin) {
            jmin = i;
            emin = error_table[i];
        }
    }
    s->u0_memory[trellis_ptr] = (jmin >= nb_trans);
    { extern int g_u0hist[]; extern long g_sym; g_u0hist[g_sym & (U0H-1)] = (jmin >= nb_trans); }

    /* init the error table to +infinity */
    for(state=0;state<s->conv_nb_states;state++) {
        s->state_error1[state] = 0x7fffffff;
    }

    for(state=0;state<s->conv_nb_states;state++) {
        /* select the value of y0 depending on the current state */
        /* for each state, we update the next state entry by selecting
           the shortest path */
        /* XXX: should handle error overflow */
        {   /* SIPFAX: with the subset depending only on trans - the reading in which the
               encoder derives trans from the coset tuple alone and uses Y0 solely inside
               trellis_next_state - the y0 halves of the table are identical and this offset
               carries no information, since the y0 dependence is already expressed by
               trellis_next_state(state, j). SIPFAX_TRELLIS_ORIG=3 drops it, paired with the
               runtime-built table whose halves ARE identical. Keeping the offset while the
               halves match is the worst combination and is what made mode 2 degrade. */
            static int noff = -1, half = -1;
            if (noff < 0) { char *e = getenv("SIPFAX_TRELLIS_ORIG"); noff = (e && atoi(e)==3); }
            if (half < 0) { char *e = getenv("SIPFAX_HALF"); half = e ? atoi(e) : 3; }   /* 1 = recovered U0 (no oracle, 100% bits, no sequence gain); 2 = (state&1)^v0, full trellis, needs v0 */
            /* SIPFAX: ROOT CAUSE of the coset-selection defect. The y0 half of the subset
               table is indexed by the bit the encoder actually folded into the transmitted
               point, which is U0 = Y[0] ^ c0 ^ v0 - NOT Y[0] alone. v0 is the superframe
               sync bit, nonzero at sync_count == 0 for the one-bits of SYNC_PATTERN, i.e.
               on 2.5% of symbols. Measured in a noise-free loopback: the half containing the
               transmitted tuple equals the encoder's U0 on 100.00% of symbols and the state
               parity on only 97.50%. On the v0-flipped symbols the zero-cost branch sits in
               the half this loop cannot reach, so the true path is forced onto a non-zero
               branch and is overtaken. Invisible with trellis_trans_16b because its two y0
               halves are identical.
               jmin/emin above already recover U0 as the half of the GLOBAL minimum over all
               2*nb_trans branches - that is exactly what u0_memory stores. Use it. */
            /* SIPFAX: HALF=2 is the PRINCIPLED form, half = (state & 1) ^ v0, with v0
               supplied by SIPFAX_V0ORACLE (field 4 of an ENCDUMP, one line per 4D symbol).
               HALF=1 (global recovered U0) fixes the bits but makes the half a scalar, which
               removes the per-state Y0 constraint and destroys state tracking. HALF=2 keeps
               the constraint AND accounts for v0; it is what a decoder with superframe sync
               would do. Acquiring v0 without the oracle is the remaining work. */
            if (noff)            n = 0;
            else if (half == 3) {
                /* SIPFAX: AUTO. Before lock, behave as half=0 (source parity) - that keeps
                   the survivor tracking the encoder, which is what the v0 estimator needs.
                   After lock, generate v0 forward from the locked phase, which is half=2 and
                   gives 100% of bits with the state constraint intact. */
                extern int g_v0lock, g_v0ph, g_v0base[]; extern long g_sym;
                int v0h = 0;
                if (!g_v0lock) {
                    /* SIPFAX: safety net. Pre-lock we need the survivor to track, so the half
                       comes from the source parity (half=0 behaviour) - but that costs 5% of
                       bits, and on a line where v0 never locks it would cost it forever. If
                       acquisition has not succeeded after SIPFAX_V0FALL symbols, fall back to
                       the recovered U0 (half=1), which gives full bits without the sequence
                       gain rather than leaving both on the table. */
                    static long fall = -1;
                    if (fall < 0) { char *e = getenv("SIPFAX_V0FALL"); fall = e ? atol(e) : 20000; }
                    if (fall > 0 && g_sym > fall) {
                        n = (jmin >= nb_trans) ? nb_trans : 0;
                        goto half_done;
                    }
                }
                if (g_v0lock) {
                    /* SIPFAX: the estimate accumulated at index ie is v0[ie-1] (it is
                       U0[ie] ^ conv_reg_parity[ie]), so the histogram - and hence the locked
                       phase - is shifted by one relative to v0's own index. The true phase is
                       g_v0ph + 1. SIPFAX_V0GEN exposes the offset so it stays measurable. */
                    static int gen = -99; long ix;
                    if (gen == -99) { char *e = getenv("SIPFAX_V0GEN"); gen = e ? atoi(e) : 1; }
                    ix = ((g_sym - 1) + g_v0ph + gen) % 480; if (ix < 0) ix += 480;
                    v0h = g_v0base[ix];
                }
                n = (((state & 1) ^ v0h) ? nb_trans : 0);
                half_done: ;
            }
            else if (half == 2) {
                extern int *g_v0orc; extern long g_v0orn, g_v0ori;
                static int voff = -99; long vi;
                int v0h = 0;
                if (voff == -99) { char *e = getenv("SIPFAX_V0OFF"); voff = e ? atoi(e) : -1; }
                /* SIPFAX: measured exactly - half == (conv_reg[t]&1) ^ v0[t-1] == U0[t],
                   100.00% over 16000 noise-free symbols. The source-state parity is right;
                   it needs the PREVIOUS symbol,s v0, hence the default offset of -1. */
                vi = g_v0ori + voff;
                if (g_v0orc && vi >= 0 && vi < g_v0orn) v0h = g_v0orc[vi];
                n = (((state & 1) ^ v0h) ? nb_trans : 0);
            }
            else if (half)       n = (jmin >= nb_trans) ? nb_trans : 0;
            else if (state & 1)  n = nb_trans;
            else                 n = 0;
        }
        for(j=0;j<nb_trans;j++) {
            int nn = n;
            next_state = trellis_next_state(s->conv_nb_states, state, j);
            {   /* SIPFAX: the y0 half of the subset table is Y[0], and the ENCODER sets
                   Y[0] = conv_reg & 1 AFTER conv_reg = trellis_next_state(...) - i.e. the
                   DESTINATION state's LSB. This loop picked the half from the SOURCE state's
                   LSB, outside the j loop. With trellis_trans_16 that matters: its two halves
                   are different partitions (each tuple lives in exactly one of them), so the
                   wrong half scores the wrong 8 tuples. With trellis_trans_16b the halves are
                   identical, which is why it never showed. SIPFAX_YN=0 restores the old
                   source-state behaviour. */
                static int yn = -1;
                if (yn < 0) { char *e = getenv("SIPFAX_YN"); yn = e ? atoi(e) : 0; }   /* REFUTED: destination-state y0 scores 64.4% vs 94.6%; the encoder Y[0] computed at symbol t is folded into t+1, so it arrives as the SOURCE state of the next symbol */
                if (yn) nn = (next_state & 1) ? nb_trans : 0;
            }
            error = s->state_error[state] + error_table[j + nn];
            if (error < s->state_error1[next_state]) {
                s->state_error1[next_state] = error;
                s->state_decision[next_state][trellis_ptr] = decision_table[j + nn];
                s->state_path[next_state][trellis_ptr] = state;
            }
        }
    }

    {   /* SIPFAX: load the v0 oracle (field 4 of an ENCDUMP), one entry per 4D symbol. */
        extern int *g_v0orc; extern long g_v0orn, g_v0ori;
        static int loaded2 = 0;
        if (!loaded2) { char *fn = getenv("SIPFAX_V0ORACLE"); loaded2 = 1;
            if (fn) { FILE *f = fopen(fn,"r"); char ln[256];
                if (f) { long cap = 1<<20; g_v0orc = (int*)malloc(cap*sizeof(int));
                    while (g_v0orn < cap && fgets(ln,sizeof(ln),f)) {
                        int f1,f2,f3,f4;
                        if (sscanf(ln,"%d %d %d %d",&f1,&f2,&f3,&f4) == 4)
                            g_v0orc[g_v0orn++] = f4 & 1;
                    }
                    fclose(f);
                    fprintf(stderr,"[acs] v0 oracle: %ld entries\n", g_v0orn); } } }
    }
    {   /* SIPFAX: the branch information is unambiguous (exactly one zero-cost branch per
           symbol with trellis_trans_16), so ask whether the ACS actually FOLLOWS it. For
           source state `state` the loop can only reach indices j + 16*(state&1) - one half is
           unreachable. If the unique zero branch lies in the half the true state cannot
           reach, the true path is forced onto a non-zero branch and can be overtaken. */
        extern long g_az_n, g_az_ok, g_az_half, g_az_reach; extern int g_ntup;
        static int azchk = -1;
        if (azchk < 0) { char *e = getenv("SIPFAX_ACSZERO"); azchk = e ? atoi(e) : 0; }
        if (azchk && g_ntup > 0) {
            int zi, z = -1, bs5 = 0, st5, d5, br5, par;
            int be5 = s->state_error1[0];
            for (zi = 0; zi < nb_trans*2; zi++) if (error_table[zi] == 0) { z = zi; break; }
            for (st5 = 1; st5 < s->conv_nb_states; st5++)
                if (s->state_error1[st5] < be5) { be5 = s->state_error1[st5]; bs5 = st5; }
            d5  = s->state_decision[bs5][trellis_ptr];
            br5 = d5 / g_ntup;
            g_az_n++;
            if (z >= 0 && br5 == z) g_az_ok++;
            {   /* SIPFAX: dump the half the zero-cost branch REQUIRES, alongside the parity
                   of the surviving predecessor. Compared offline against the encoder's own
                   conv_reg parity (SIPFAX_ENCDUMP field 7) this separates "the half
                   convention is wrong" from "the survivor was already lost". */
                extern FILE *g_zf;
                if (!g_zf) { char *e = getenv("SIPFAX_ZDUMP"); if (e) g_zf = fopen(e,"w"); }
                if (g_zf) fprintf(g_zf, "%d %d %d %d\n", z, (z >= nb_trans) ? 1 : 0,
                                  s->state_path[bs5][trellis_ptr] & 1, bs5 & 1);
            }
            /* how many states can even reach the zero branch's half? */
            if (z >= 0) {
                par = (z >= nb_trans) ? 1 : 0;
                for (st5 = 0; st5 < s->conv_nb_states; st5++)
                    if ((st5 & 1) == par) g_az_reach++;
                if ((s->state_path[bs5][trellis_ptr] & 1) == par) g_az_half++;
            }
        }
    }
    {   /* SIPFAX: drive the ACS against the ENCODER'S KNOWN STATE SEQUENCE. The survivor
           states are uncorrelated with the encoder's at every delay (1.6-1.9%, i.e. chance)
           while the symbols still decode 100% off the slicer, so the question is whether
           the correct next state even survives this loop and what metric it is given.
           SIPFAX_TRUESTATE=<file> is one encoder conv_reg per line (field 7 of the
           SIPFAX_ENCDUMP dump); SIPFAX_TRUESTATE_OFF aligns it. */
        extern int *g_truest; extern long g_truen, g_truei;
        extern long g_tr_argmin, g_tr_tot, g_tr_rank, g_tr_gap, g_tr_inf;
        static int loaded = 0;
        if (!loaded) {
            char *fn = getenv("SIPFAX_TRUESTATE");
            loaded = 1;
            if (fn) { FILE *f = fopen(fn,"r");
                if (f) { int v; long cap = 1<<20, k = 0;
                    g_truest = (int*)malloc(cap*sizeof(int));
                    while (k < cap && fscanf(f,"%d",&v) == 1) g_truest[k++] = v;
                    fclose(f); g_truen = k;
                    { char *o2 = getenv("SIPFAX_TRUESTATE_OFF"); g_truei = o2 ? atol(o2) : 0; }
                    fprintf(stderr,"[acs] loaded %ld true states, start %ld\n", g_truen, g_truei);
                } }
        }
        {   /* SIPFAX: branch metrics discriminate (spread ~700) yet every state comes out
               tied. Measure the spread of the ACCUMULATED state metrics, and how many
               states share the minimum - that is where the information is being lost. */
            extern long g_ss_n, g_ss_tied; extern double g_ss_spread;
            int st3, mn3 = 0x7fffffff, mx3 = -0x7fffffff, cnt3 = 0;
            for (st3 = 0; st3 < s->conv_nb_states; st3++) {
                if (s->state_error1[st3] == 0x7fffffff) continue;
                if (s->state_error1[st3] < mn3) mn3 = s->state_error1[st3];
                if (s->state_error1[st3] > mx3) mx3 = s->state_error1[st3];
            }
            for (st3 = 0; st3 < s->conv_nb_states; st3++)
                if (s->state_error1[st3] == mn3) cnt3++;
            g_ss_n++; g_ss_spread += (double)(mx3 - mn3); g_ss_tied += cnt3;
        }
        if (g_truest && g_truei + 1 < g_truen) {
            int tn = g_truest[g_truei + 1] % s->conv_nb_states;
            int st2, rank = 0, mn = 0x7fffffff, am = 0;
            for (st2 = 0; st2 < s->conv_nb_states; st2++)
                if (s->state_error1[st2] < mn) { mn = s->state_error1[st2]; am = st2; }
            for (st2 = 0; st2 < s->conv_nb_states; st2++)
                if (s->state_error1[st2] < s->state_error1[tn]) rank++;
            g_tr_tot++;
            if (am == tn) g_tr_argmin++;
            g_tr_rank += rank;
            if (s->state_error1[tn] == 0x7fffffff) g_tr_inf++;
            else g_tr_gap += (s->state_error1[tn] - mn);
            g_truei++;
        }
    }
    {   /* SIPFAX: normalise - subtract the minimum from every survivor. Differences are
           all that matter to the ACS, and without this the metrics grow without bound (the
           code's own "XXX: should handle error overflow"). Also keeps the spread
           measurable. */
        int st4, mn4 = 0x7fffffff;
        for (st4 = 0; st4 < s->conv_nb_states; st4++)
            if (s->state_error1[st4] < mn4) mn4 = s->state_error1[st4];
        if (mn4 != 0x7fffffff && mn4 > 0)
            for (st4 = 0; st4 < s->conv_nb_states; st4++)
                if (s->state_error1[st4] != 0x7fffffff) s->state_error1[st4] -= mn4;
    }
    /* XXX: this copy is not needed. Permute the two tables */
    memcpy(s->state_error, s->state_error1, sizeof(s->state_error));

    { extern long g_v0ori; g_v0ori++; }
    {   extern long g_sym, g_v0hn; extern int g_v0lock;
        static long need = -1;
        if (need < 0) { char *e = getenv("SIPFAX_V0ACQ"); need = e ? atol(e) : 480; }   /* one full v0 period; locks after ~931 symbols */
        g_sym++;
        /* the correlation is 480x480; run it once per period, not per symbol */
        if (!g_v0lock && (g_sym % V0_PER) == 0) v0_try_lock(need);
    }
    trellis_ptr = (trellis_ptr + 1) % TRELLIS_LENGTH;
    s->trellis_ptr = trellis_ptr;
}

static void put_bit(V34DSPState *s, int b)
{
    int poly;

    if (!s->calling)
        poly = V34_GPC;
    else
        poly = V34_GPA;
    b = unscramble_bit(s, b, poly);
    //    fprintf(stderr, "recv: %d\n", b);
    if (s->put_bit) s->put_bit(s->opaque, b);   /* SIPFAX: offline harnesses have no sink */
}

/* auxilary channel bit */
static void aux_put_bit(V34DSPState *s, int b)
{
    /* not used now */
}

static void decode_mapping_frame(V34DSPState *s, s16 rx_mapping_frame[8][2])
{
  int m[4][2], r0; /* rings */
  u8 I[3][4];
  int Q[4][2], Z[2];
  u8 *ptr;
  int t,i,j,x,y,n,mp_size,xout_ptr;
  u8 data[MAX_MAPPING_FRAME_SIZE];

  xout_ptr = 0;
  for(j=0;j<4;j++) {
    /* for each 4D symbol */

    for(i=0;i<2;i++) {
      x = rx_mapping_frame[xout_ptr][0];
      y = rx_mapping_frame[xout_ptr][1];
      xout_ptr++;

      /* decision */
      x = (x >> 8) * 2 + 1;
      x = clamp(x, C_RADIUS);
      y = (y >> 8) * 2 + 1;
      y = clamp(y, C_RADIUS);

      /* SIPFAX: (9.6.2) PRECODER INVERSE.
         x,y are now the DECIDED transmitted coordinate Y. The transmitter formed
         Y = u + c from the mapper output u and a coset correction c derived from its own
         past, so the receiver must subtract the same c before looking u up in the
         constellation. It can, because the transmitter's state update depends only on Y
         and on p - both of which the receiver reconstructs from its own decisions:

             p     = sum_{k=0..2} x[k]*h[k]      >> 14   (h: 14 fractional bits)
             c     = round(p / 2^(7+w)) << w             (w = 1 for b<56, else 2;
                                                          c is even, so u stays odd)
             u     = Y - c
             x[0]' = (Y << 7) - p                        (x: 7 fractional bits)

         This mirrors encode_mapping_frame exactly. Gated on rx_precode so a peer that
         does not precode is unaffected; the caller demonstrably does - the loopback with
         its MP coefficients reproduces the trellis metric measured on the wire (178.7 vs
         172) where an unprecoded loopback sits at 8.6. */
      if (s->rx_precode) {
          int px = 0, py = 0, k2, p_re, p_im, c_re, c_im, xr, xi;
          int w2 = (s->b < 56) ? 1 : 2;
          for (k2 = 0; k2 < 3; k2++) {
              px += s->x[k2][0]*s->h[k2][0] - s->x[k2][1]*s->h[k2][1];
              py += s->x[k2][1]*s->h[k2][0] + s->x[k2][0]*s->h[k2][1];
          }
          p_re = shr_round0(px, 14);
          p_im = shr_round0(py, 14);
          c_re = shr_round0(p_re, 7 + w2) << w2;
          c_im = shr_round0(p_im, 7 + w2) << w2;
          xr = (x << 7) - p_re;
          xi = (y << 7) - p_im;
          { static int sgn = -2; if (sgn == -2) { char *e = getenv("SIPFAX_PC_SIGN"); sgn = e ? atoi(e) : -1; }
            x = clamp(x + sgn*c_re, C_RADIUS);
            y = clamp(y + sgn*c_im, C_RADIUS); }
          for (k2 = 2; k2 >= 1; k2--) {
              s->x[k2][0] = s->x[k2-1][0];
              s->x[k2][1] = s->x[k2-1][1];
          }
          s->x[0][0] = xr;
          s->x[0][1] = xi;
      }

      {   /* SIPFAX: what coordinates is the trellis decoder actually producing, and does
             the table know them? m came out 0 on every one of 29456 symbols, and a table
             MISS also returns 0 (the cell stores i | j<<14, and i=0,j=0 is the zeroed
             value), so a miss is indistinguishable from a hit on the innermost point. */
          extern long g_dh[8]; extern long g_dmiss, g_dn;
          int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
          int mx = ax > ay ? ax : ay;
          g_dn++;
          if (mx <= 7) g_dh[(mx-1)/2 & 7]++; else g_dh[7]++;
          if (s->constellation_to_code[(x+C_RADIUS) >> 1][(y+C_RADIUS) >> 1] == 0
              && !(x == s->constellation[0][0] && y == s->constellation[0][1]))
              g_dmiss++;
      }
      {   /* SIPFAX: dump the DECIDED coordinate sequence. Its distribution was verified
             against the transmitted set (54.5% inner vs 54.7%), but a distribution match
             says nothing about whether the right symbol is decided at the right time -
             which is what every downstream stage depends on. */
          extern FILE *g_decf;
          if (!g_decf) { char *e = getenv("SIPFAX_DECDUMP"); if (e) g_decf = fopen(e,"w"); }
          if (g_decf) fprintf(g_decf, "%d %d\n", x, y);
      }
      t = s->constellation_to_code[(x+C_RADIUS) >> 1][(y+C_RADIUS) >> 1];
      /* mapping to the symbol */
      /* SIPFAX: quadrant handedness. rotate_clockwise() is really CCW - case 1 is
         (x,y)=(-y1,x1), i.e. multiplication by +j, and V34_baseband_to_carrier emits
         Re{(si+j*sq)e^{+j phi}}. Phase 4 negates it to get spec CW (10.1.3.3), and that
         negation was validated on a real caller (FINDINGS: TRN decodes to 0.998 ones with
         CW, 0.32-0.51 the other way). Data mode's 9.6.1 mapper and the decoder's
         constellation_to_code table were left UN-negated, so the two directions disagree
         about handedness. That cannot move the trellis metric - Z is read long after mse
         is computed, and data_slice scans all four rotations - but it does corrupt the
         extracted BITS: with Z mirrored, I1 stays correct while I2 flips whenever I1=1 and
         I0 flips whenever U0=1. Metric 23.3 with 50% ones is exactly that signature.
         SIPFAX_Z_SIGN=1 negates on decode so the two arms can be compared. */
      { static int zs = -1;
        if (zs < 0) { char *ez = getenv("SIPFAX_Z_SIGN"); zs = ez ? atoi(ez) : 0; }
        Z[i] = zs ? ((4 - ((t >> 14) & 3)) & 3) : (t >> 14); }
      /* SIPFAX: the constellation_to_code cell packs i | (j << 14), so the index field is
         FOURTEEN bits, not eight. Masking to 0xff truncated the quarter-constellation index
         to 0..255. Harmless at every rate whose L/4 fits in 256 - 128 at 28800, 208 at
         31200 - but R=33600 has L=1408, so L/4 = 352 and every point from 256 up lost bit
         8. Q is the low q bits and survived the mask, while m = t >> q did not, which is
         exactly what the mapping-frame dump showed: ring-index bits wrong on ~41% of frames
         and the I and Q fields bit-exact. */
      t = t & 0x3fff;

      Q[j][i] = t & ((1 << s->q)-1);
      m[j][i] = t >> s->q;
      {   /* SIPFAX: the slicer above clamps to C_RADIUS - the FULL lattice - not to the
             negotiated constellation, so a noisy symbol can decide to a point outside the
             L-point set and yield m >= M. rings_to_index then sums those into indices for
             g2_tab, which is only built to 8*(M-1)+1 entries (17 at M=3), so one bad
             decision corrupts the whole mapping frame's bit extraction and can read the
             table past its initialised range. Exact loopback symbols never leave the set,
             which is why the loopback scores 100% while noisy input gives coin flips.
             Count it. */
          extern long g_moob, g_mtot, g_mmax;
          g_mtot++;
          if (m[j][i] >= s->M) { g_moob++; if (m[j][i] > g_mmax) g_mmax = m[j][i]; }
      }
    }

    t = (Z[0] - s->Z_1) & 3;
    s->Z_1 = Z[0];
    I[1][j] = t & 1;
    I[2][j] = t >> 1;
    
    t = (Z[1] - Z[0]) & 3;
    I[0][j] = t >> 1;
    {   /* SIPFAX: U0 is the low bit the encoder folded in as
           Z[1] = (Z[0] + 2*I0 + U0) & 3, so it is free here - no traceback alignment
           needed, unlike reading u0_memory out of the trellis. Combined with the buffered
           Y0 it gives the superframe sync bit v0 = U0 ^ Y0 (c0 = 0 with no precoding). */
        extern FILE *g_v0f2;
        /* SIPFAX: trellis_encoder returns s->U0 at the END of a 4D symbol and the encoder
           folds it into the NEXT symbol's Z[1] = (Z[0] + 2*I0 + s->U0) & 3, so the U0
           recovered here at symbol j belongs with Y[0] of symbol j-1 - carried across the
           frame boundary in y0_prev. SIPFAX_Y0SHIFT makes the offset measurable rather than
           assumed. Acceptance test: v0 is nonzero only at sync_count == 0 (1 in 2*P = 30)
           and then only for the one-bits of SYNC_PATTERN, so a CORRECT pairing gives
           U0 ^ Y0 == 0 in about 97.5% of samples; a wrong one sits at 50%. */
        static int y0sh = -2;
        int u0b = t & 1, y0b, jj;
        if (y0sh == -2) { char *ez = getenv("SIPFAX_Y0SHIFT"); y0sh = ez ? atoi(ez) : -1; }
        jj = j + y0sh;
        y0b = (jj >= 0 && jj < 4) ? s->y0_buf[jj] : s->y0_prev;
        if (!g_v0f2) { char *e = getenv("SIPFAX_V0DUMP2"); if (e) g_v0f2 = fopen(e,"w"); }
        if (g_v0f2) fprintf(g_v0f2, "%d %d %d %d %d %d %d %d %d\n", u0b ^ y0b, u0b, y0b,
                            s->sync_count, s->half_data_frame_count, Z[0], Z[1],
                            s->st_arr, s->st_prev);
    }
  }

  s->y0_prev = s->y0_buf[3];   /* SIPFAX: carry Y0 across the frame boundary for j-1 */

  /* compute mapping frame size */
  s->rcnt += s->r;
  if (s->rcnt < s->P) {
      mp_size = s->b - 1;
  } else {
      s->rcnt -= s->P;
      mp_size = s->b;
  }
    
  /* now everything is "decoded", we can write the data */
  ptr = data;
  if (s->b <= 12) {
    for(i=0;i<s->b;i++) *ptr++ = ((u8 *)I)[i];
  } else {
    r0 = rings_to_index(s, m);

    n = s->K;
    if (mp_size < s->b) n--;
    for(i=0;i<n;i++) *ptr++ = (r0 >> i) & 1;
    
    for(j=0;j<4;j++) {
      ptr[0] = I[0][j];
      ptr[1] = I[1][j];
      ptr[2] = I[2][j];
      ptr += 3;
  
      t=Q[j][0];
      for(i=0;i<s->q;i++) *ptr++ = (t >> i) & 1;

      t=Q[j][1];
      for(i=0;i<s->q;i++) *ptr++ = (t >> i) & 1;
    }
  }

  /* send an auxilary channel bit if needed */
  s->acnt += s->W;
  if (s->acnt < s->P) {
      put_bit(s, data[0]);
  } else {
      s->acnt -= s->P;
      aux_put_bit(s, data[0]); 
  }

  { extern FILE *g_mfrx; if (!g_mfrx) { char *e = getenv("SIPFAX_MFDUMP_RX");
      if (e) g_mfrx = fopen(e,"w"); }
    if (g_mfrx) { int z; for (z=0;z<mp_size;z++) fputc('0'+(data[z]&1), g_mfrx); fputc('\n', g_mfrx); } }
  /* send all the decoded bits */
  for(i=1;i<mp_size;i++) put_bit(s, data[i]); 
  //  print_bit_vector("recv", data, mp_size);

  if (++s->mapping_frame >= s->P) {
      /* new data frame */
      s->mapping_frame = 0;
      s->rcnt = 0;
      s->acnt = 0;
  }
}

void baseband_decode_pub(V34DSPState *s, int si, int sq) { extern void baseband_decode_impl(V34DSPState*,int,int); baseband_decode_impl(s,si,sq); }
void baseband_decode_impl(V34DSPState *s, int si, int sq)
{
    s16 y[2][2];
    static int delay = 0;
    int mse,v0;

    lm_dump_qam(si / (10.0 * 128.0), sq / (10.0 * 128.0));

    s->yy[s->phase_4d][0] = si;
    s->yy[s->phase_4d][1] = sq;
    
    if (++s->phase_4d == 2) {

        trellis_decoder(s, y, s->yy , &mse);
        /* SIPFAX: long-run mean of the trellis branch metric. This decoder has no
           sync-lock flag (the sync bit is generated open-loop), so the metric is the only
           quantitative handle on whether the symbols we feed it are decodable. Calibrated
           against the loopback at known SNR. */
        s->data_mse_acc += mse; s->data_mse_n++;
        s->phase_mse += mse;
        s->phase_mse_cnt++;
        if (s->phase_mse_cnt >= 8) {
            s->phase_mse_cnt = 0;
            s->phase_mse = 0;
        }

        {   /* SIPFAX: ADOPT THE FRAME PHASE FROM THE v0 LOCK, retiring SIPFAX_DATA_SKIP.
               v0 is deterministic in (sync_count, half_data_frame_count) with period
               2*P * 2*J = 30*16 = 480 4D symbols, and the acquisition already recovers that
               phase. The decoder's own counters simply start at zero at data-mode entry,
               wherever in the superframe that happens to be, which is what the hand-set
               SIPFAX_DATA_SKIP was compensating for by dropping symbols at the feed.
               Given the locked phase, the true counters are available directly. Applied once,
               on the first symbol after lock. SIPFAX_V0FRAME=0 disables. */
            extern int g_v0lock, g_v0ph, g_v0applied; extern long g_sym;
            static int en = -1;
            if (en < 0) { char *e = getenv("SIPFAX_V0FRAME"); en = e ? atoi(e) : 0; }   /* measured: NO effect - the counters say WHICH frame we are in, not where frames START */
            if (en && g_v0lock && !g_v0applied) {
                long k = (((g_sym + g_v0ph) % 480) + 480) % 480;
                int sc = (int)(k % (2*s->P));
                int hd = (int)((k / (2*s->P)) % (2*s->J));
                extern int v34_dbg;
                if (v34_dbg) fprintf(stderr, "[v0] frame phase adopted: sync_count %d -> %d, "
                                     "half_data_frame_count %d -> %d\n",
                                     s->sync_count, sc, s->half_data_frame_count, hd);
                s->sync_count = sc; s->half_data_frame_count = hd;
                s->rcnt = (int)(((k / 4) * (long)s->r) % s->P);
                g_v0applied = 1;
            }
        }
        /* synchronization bit */
        if (s->sync_count == 0) {
            v0 = (SYNC_PATTERN >> (15 - s->half_data_frame_count)) & 1;
        } else {
            v0 = 0;
        }

        {   extern int g_v0est, g_v0have; extern FILE *g_v0f;
            if (!g_v0f) { char *e = getenv("SIPFAX_V0DUMP"); if (e) g_v0f = fopen(e,"w"); }
            if (g_v0f && g_v0have)
                fprintf(g_v0f, "%d %d %d %d\n", g_v0est, v0, s->sync_count,
                        s->half_data_frame_count);
        }
        /* synchronization bit */
        if (++s->sync_count == 2*s->P) {
            s->sync_count = 0;
            if (++s->half_data_frame_count == 2*s->J) {
                s->half_data_frame_count = 0;
            }
        }

        /* SIPFAX: buffer Y0 into the SAME slot, at the SAME moment, as the symbols it
           belongs to. Buffering it on its own counter was wrong: rx_mapping_frame only
           advances once delay > TRELLIS_LENGTH, so an independently-incremented y0 index
           drifts ahead and pairs each frame's U0 with a later symbol's Y0. The memcpy below
           writes 4 s16 = two 2D symbols into slots [count] and [count+1], i.e. one 4D
           symbol, so the matching Y0 index is count >> 1. */
        {   /* SIPFAX: automatic alignment - see the note by g_v0_realign. Runs only when
               SIPFAX_DATA_SKIP is not set, so the hand-set path is untouched. */
            extern int g_v0lock, g_v0ph, g_v0_pairtry, g_v0_pairdrop, g_v0applied;
            extern long g_v0_realign, g_sym, g_v0hn; extern int g_v0_aligned;
            extern int g_v0h[]; extern int v34_dbg;
            static int en = -1;
            if (en < 0) { char *e = getenv("SIPFAX_V0ALIGN"); en = e ? atoi(e) : 1; }
            if (en) {
                if (g_v0lock && !g_v0_aligned) {
                    g_v0_aligned = 1;
                    g_v0_realign = ((19 - (long)g_v0ph) % 20 + 20) % 20;
                    if (v34_dbg) fprintf(stderr, "[v0] align: phi=%d (mod20=%d) -> drop %ld "
                                         "4D symbols\n", g_v0ph, g_v0ph % 20, g_v0_realign);
                }
                /* no lock in twice the acquisition window: the 4D pairing is off by one */
                {   /* SIPFAX: how long to wait before concluding the 4D pairing is wrong. v0_try_lock
                       evaluates once per 480-symbol period once it has a period of data, so a
                       few evaluations are enough to decide; every symbol spent waiting is a
                       symbol decoded on the wrong alignment. SIPFAX_V0PAIRWAIT overrides. */
                    static long pw = -1;
                    if (pw < 0) { char *e = getenv("SIPFAX_V0PAIRWAIT"); pw = e ? atol(e) : 1440; }
                if (!g_v0lock && !g_v0_pairtry && g_v0hn > pw) {
                    int q; for (q = 0; q < 480; q++) g_v0h[q] = 0;
                    g_v0hn = 0; g_v0_pairtry = 1; g_v0_pairdrop = 1;
                    if (v34_dbg) fprintf(stderr, "[v0] no lock - shifting the 4D pairing by "
                                         "one 2D symbol and retrying\n");
                }
                }
                if (g_v0_realign > 0) { g_v0_realign--; goto feed_skip; }
            }
        }
        memcpy(&s->rx_mapping_frame[s->rx_mapping_frame_count][0], 
               &y[0][0], 4 * sizeof(s16));
        s->y0_buf[(s->rx_mapping_frame_count >> 1) & 3] = s->y0_out;
        delay++;
        if (delay > TRELLIS_LENGTH) {

            s->rx_mapping_frame_count += 2;
            if (s->rx_mapping_frame_count == 8) {
                /* a complete mapping frame was read */
                decode_mapping_frame(s, s->rx_mapping_frame); 
                s->rx_mapping_frame_count = 0;
            }
        }
        feed_skip: ;   /* SIPFAX: automatic alignment drops a 4D symbol here */
        s->phase_4d = 0;
    }
}


/* AGC */

#define AGC_COEF 0.99
#define AGC_BITS 24

static void agc_init(V34DSPState *s)
{
    s->agc_coef = (int) (AGC_COEF * (1 << AGC_BITS));
    s->agc_mem = 0;
}

static void agc_estimate(V34DSPState *s, int sample)
{
    float power;
    
    s->agc_mem = s->agc_mem * AGC_COEF + (sample * sample);

    power = (float) s->agc_mem * (1.0 - AGC_COEF);
    /* XXX: the constant here depends on the modulation parameters */
    // s->agc_gain = (16384.0/2 * 1495) / sqrt(power);
    s->agc_gain = 16384.0 * 0.80;

    //    lm_dump_agc(power / 16384.0);
    lm_dump_agc(sqrt(power));
}

#if 0
static void test_nyq(float *a_ptr, float *b_ptr, float f, float val)
{
    float a,b,c,d;

    a = *a_ptr;
    b = *b_ptr;
    c = 0.99 * cos(f);
    d = 0.99 * sin(f);
    
    *a_ptr = a * c - b * d + val;
    *b_ptr = a * d + b * c;
}

static float al, bl, ah, bh;
static float f_low, f_high;
static float low_mem[2], low_coef[2];
#endif

#define SYNC_THR 32

/* Fast symbol timing recovery */
static void v34_symbol_sync(V34DSPState *s, int spl)
{
    int a, b, c, v;
#if 0
    int tmp0, tmp1;
    static s16 dc_filter[2];
    static s16 ac_filter[3];
#endif

#if 0
    {
        static int k = 0;
        float v;

        f_low = 2 * M_PI * (s->carrier_freq - s->symbol_rate / 2.0) / (3.0 * s->symbol_rate);
        f_high = 2 * M_PI * (s->carrier_freq + s->symbol_rate / 2.0) / (3.0 * s->symbol_rate);
        test_nyq(&al, &bl, f_low, spl);
        test_nyq(&ah, &bh, f_high, spl);
    }
#endif
    /* sync_low_mem has an amplitude of about spl / ( 1 - a), so we
       store it with a scaling of 2^6 to reduce its amplitude */

    v = ((spl << 8) + s->sync_low_mem[0] * s->sync_low_coef[0] + 
         s->sync_low_mem[1] * s->sync_low_coef[1]) >> 14;
    s->sync_low_mem[1] = s->sync_low_mem[0];
    s->sync_low_mem[0] = v;
    
    v = ((spl << 8) + s->sync_high_mem[0] * s->sync_high_coef[0] + 
         s->sync_high_mem[1] * s->sync_high_coef[1]) >> 14;
    s->sync_high_mem[1] = s->sync_high_mem[0];
    s->sync_high_mem[0] = v;

    if (s->baud3_phase == 0) {
        /* high & low nyquist filters for symbol timing recovery */
        
        /* XXX: the shift should adapt to the power */
        a = (s->sync_low_mem[1] * s->sync_high_mem[1]) >> 14;
        b = (s->sync_high_mem[1] * s->sync_low_mem[0]) >> 14;
        c = (s->sync_low_mem[1] * s->sync_high_mem[0]) >> 14;
        
        v = (a * s->sync_A + b * s->sync_B + c * s->sync_C) >> 14;
        fprintf(stderr, "v=%d\n", v);
        s->baud_phase -= v << 3;

#if 0
        /* DC filter h(z) = z - z^-2 */
        tmp0 = v - dc_filter[1];
        dc_filter[1] = dc_filter[0];
        dc_filter[0] = v;

        /* AC filter h(z) = z + z^-3 */
        tmp1 = tmp0 + ac_filter[2];
        ac_filter[2] = ac_filter[1];
        ac_filter[1] = ac_filter[0];
        ac_filter[0] = tmp0;

        if (tmp1 < -SYNC_THR)
            tmp1 = -SYNC_THR;
        else if (tmp1 > SYNC_THR) 
            tmp1 = SYNC_THR;
        else
            tmp1 =0;
#endif
#if 1
        //        lm_dump_sample(CHANNEL_SAMPLE, v);

        //        fprintf(stderr, "corr=%0.0f\n", 
        //               corr * 32.0 / 100.0);
#else
        fprintf(stderr, "al=%0.1f ah=%0.1f al1=%0.1f ah1=%0.1f\n",
               ah / 100.0, bh / 100.0, 
               (s->sync_high_mem[0] - s->sync_high_mem[1] * 0.99 * cos(f_high)) * 32.0 / 100.0,
               (s->sync_high_mem[1] * 0.99 * sin(f_high)) * 32.0 / 100.0);
#endif        
    }
}


/* equalize & adapt the equalizer */
static int v34_equalize(V34DSPState *s, 
                        int *ri_ptr, int *rq_ptr, int spl)
{
    int p,q,i;
    int ri, rq, fi, fq, q_ri, q_rq, ei, eq, ei1, eq1, si, sq;
    int cosw, sinw, dphi, norm;

    /* add the sample in the equalizer ring buffer */
    p = s->eq_buf_ptr;

    s->eq_buf[p] = spl;

    if (++p == EQ_SIZE)
        p = 0;
    s->eq_buf_ptr = p;

    if (s->baud3_phase != 0)
        return 0;

    /* apply the equalizer filter to the data */
    ri = rq = 0;
    q = p;
    for(i=0;i<EQ_SIZE;i++) {
        fi = s->eq_filter[i][0] >> 16;
        fq = s->eq_filter[i][1] >> 16;
        
        ri += fi * s->eq_buf[q];
        rq += fq * s->eq_buf[q];

        q++;
        if (q == EQ_SIZE)
            q = 0;
    }
    si = ri >> 14;
    sq = rq >> 14;
    /* rotate by the carrier phase */
    /* translate back to baseband */

    cosw = dsp_cos(s->carrier_phase);
    sinw = - dsp_cos((PHASE_BASE/4) - s->carrier_phase);
    ri = ( si * cosw - sq * sinw ) >> COS_BITS;
    rq = ( si * sinw + sq * cosw ) >> COS_BITS;
    
    *ri_ptr = ri;
    *rq_ptr = rq;

    /* compute the error */

    /* quantification */
    q_ri = ((ri >> 8) * 2 + 1) << 7;
    q_rq = ((rq >> 8) * 2 + 1) << 7;

    /* error computation */
    ei1 = - (ri - q_ri);
    eq1 = - (rq - q_rq);

    /**** phase tracking */

    /* normalized derivative of the phase shift */
    /* XXX: avoid sqrt : slow !!! */
    norm = (int) sqrt(ri * ri + rq * rq );
    if (norm > 0) {
        dphi = (ri * eq1 - rq * ei1) / norm;
    } else {
        dphi = 0;
    }
    { extern int eq_notrack; s->carrier_phase += s->carrier_incr - (eq_notrack ? 0 : dphi); }

    /* remodulate (because the equalizer is done before converting to
       baseband) */
    ei = ( ei1 * cosw + eq1 * sinw ) >> COS_BITS;
    eq = ( - ei1 * sinw + eq1 * cosw ) >> COS_BITS;

#if 1
    /* update the coefficients with the error */
    { extern int eq_freeze; if (!eq_freeze) {
    q = p;
    for(i=0;i<EQ_SIZE;i++) {
        int di, dq;
        di = ei * s->eq_buf[q];
        dq = eq * s->eq_buf[q];
        
        s->eq_filter[i][0] += (di >> s->eq_shift) * 16;
        s->eq_filter[i][1] += (dq >> s->eq_shift) * 16;

        q++;
        if (q == EQ_SIZE)
            q = 0;
    }
    } }
#endif

    lm_dump_equalizer(s->eq_filter, 1 << 30, EQ_SIZE);

    return 1;
}

static void V34_demod(V34DSPState *s, 
                      const s16 *samples, unsigned int nb)
{
    int si, sq, i, j, k , ph, spl;
    int v, frac, ph1;

    for(i=0;i<nb;i++) {
        /* Automatic Gain Control */
        spl = samples[i];

        if (v34_dbg) s->dbg_n++;
        agc_estimate(s, spl);
        spl = (spl * s->agc_gain) >> 14;

        /* insert the new sample in the ring buffer */
        s->rx_buf1[s->rx_buf1_ptr] = spl;
        s->rx_buf1_ptr = (s->rx_buf1_ptr + 1) & (RX_BUF1_SIZE-1);

        /* sample rate convertion, timing correction & matched filter
           (root raised cosine) */
        s->baud_phase += s->baud_num;
        while (s->baud_phase >= s->baud_denom) {
            s->baud_phase -= s->baud_denom;
            
            ph = s->baud_phase;
            si = 0;
            for(j=0;j<s->rx_filter_wsize;j++) {
                k = (s->rx_buf1_ptr - s->rx_filter_wsize + j) & (RX_BUF1_SIZE-1);
                /* XXX: verify that there is no overflow */

                /* interpolation of the filter coefficient */
                ph1 = ph >> 16;
                frac = ph & 0xffff;
                v = ((0x10000 - frac) * s->rx_filter[ph1] + 
                     frac * s->rx_filter[ph1+1]) >> 16;
                si += v * s->rx_buf1[k];
                ph += s->baud_num;
            }
            si = (si >> 14);
            lm_dump_sample(CHANNEL_SAMPLESYNC, si / 32768.0);

            /* we have here EQ_FRAC = 3 symbols per baud */

            if (v34_dbg && s->state != s->dbg_last) { fprintf(stderr, "[dec] demod state %d -> %d (si=%d) at %ld ms\n", s->dbg_last, s->state, si, s->dbg_n/8); fflush(stderr); s->dbg_last = s->state; }
            switch(s->state) {
            case V34_STARTUP3_WAIT_S1:
                /* wait for the S signal */
                fprintf(stderr, "waiting S1 %d\n", si);
                /* XXX: find a better test ! */
                if (abs(si) > 13000) {
                    s->state = V34_STARTUP3_S1;
                    s->sym_count = 0;
                }
                break;

            case V34_STARTUP3_S1:
                /* S signals are mainly used to recover the symbol clock */
                v34_symbol_sync(s, si);
                if (++s->sym_count >= 128 * EQ_FRAC) {
                    s->state = V34_STARTUP3_SINV1;
                    s->sym_count = 0;
                }
                break;
            case V34_STARTUP3_SINV1:
                v34_symbol_sync(s, si);
                if (++s->sym_count >= 16 * EQ_FRAC) {
                    s->state = V34_STARTUP3_S2;
                    s->sym_count = 0;
                }
                break;

            case V34_STARTUP3_S2:
                v34_symbol_sync(s, si);
                if (++s->sym_count >= 128 * EQ_FRAC) {
                    s->state = V34_STARTUP3_SINV2;
                    s->sym_count = 0;
                }
                break;

            case V34_STARTUP3_SINV2:
                v34_symbol_sync(s, si);
                if (++s->sym_count >= 100 * EQ_FRAC) {
                    s->state = V34_STARTUP3_PP;
                    s->sym_count = 0;
                }
                break;

            case V34_STARTUP3_PP:
#if 1
                /* PP is used to fast train the equalizer */

                /* store the 144 samples at the middle of the PP
                   frame. We do this because we suppose in the fast
                   equalizer that the sequence is periodic */
                if (s->sym_count >= (120) * EQ_FRAC && 
                    s->sym_count < (168) * EQ_FRAC) {
                    s->eq_buf[s->sym_count - (120) * EQ_FRAC] = si;
                }

                if (s->sym_count == (168) * EQ_FRAC) {
                    if (v34_dbg) { int _k; fprintf(stderr,"[eq] PP eq_buf[0..7]="); for(_k=0;_k<8;_k++) fprintf(stderr,"%d ",s->eq_buf[_k]); fprintf(stderr,"\n"); }
                    V34_fast_equalize(s, s->eq_buf);
                    if (v34_dbg) { int _k,nz=0; for(_k=0;_k<EQ_SIZE;_k++) if(s->eq_filter[_k][0]||s->eq_filter[_k][1])nz++;
                        fprintf(stderr,"[eq] after train: %d/%d nonzero eq_filter taps; [0]=(%d,%d) [1]=(%d,%d)\n",nz,EQ_SIZE,s->eq_filter[0][0],s->eq_filter[0][1],s->eq_filter[1][0],s->eq_filter[1][1]); }
                    /* reset eq_buf to avoid potential problems when the
                       adaptive is started */
                    memset(s->eq_buf, 0, sizeof(s->eq_buf));
                }
                
                if (++s->sym_count == 288 * EQ_FRAC) {
                    s->state = V34_STARTUP3_TRN;
                }
#else
                memmove(s->eq_buf, &s->eq_buf[1], 2 * EQ_SIZE);
                s->eq_buf[EQ_SIZE - 1] = si;
                
                if ((++s->sym_count % EQ_SIZE) == 0) {
                    V34_fast_equalize(s, s->eq_buf);
                    lm_dump_equalizer(s->eq_filter, 1 << 30, EQ_SIZE);
                }
#endif

                break;

            case V34_STARTUP3_TRN:
                si = (float)si * 128.0 / CALC_AMP(TRN4_POWER);
                if (v34_equalize(s, &si, &sq, si)) {
                    static int ptr = 0;
                    
                    if (v34_dbg && v34_symdump_n < 40000) {
                        double ang = atan2((double)sq, (double)si);
                        int qd = ((int)floor((ang + M_PI/4) / (M_PI/2))) & 3;
                        v34_softi[v34_symdump_n] = si; v34_softq[v34_symdump_n] = sq;
            v34_symdump[v34_symdump_n++] = qd;
                        if (v34_symdump_n>=200 && v34_symdump_n<212)
                            fprintf(stderr,"[eq] sym%d si=%d sq=%d ang=%.2f qd=%d\n",v34_symdump_n,si,sq,ang,qd);
                    }
                    if (!v34_dbg && ++ptr > (28 * 2)) {
                        baseband_decode_impl(s, si, sq);
                    }
                }
                break;
            }


            
            if (++s->baud3_phase == EQ_FRAC)
                s->baud3_phase = 0;
        }
    }
}

static void V34_demod_init(V34DSPState *s, V34State *p)
{
    memset(s, 0, sizeof(V34DSPState));

    V34_init_low(s, p, 0);
    s->state = V34_STARTUP3_WAIT_S1;
}


/* ---- offline Phase-3 decode harness ---- */
void V34_decode_file(const char *path, int calling)
{
    extern int v34_dbg;
    V34State p; V34DSPState rx; s16 buf[512]; FILE *f;
    int n, i;
    memset(&p, 0, sizeof(p));
    p.S = V34_S3429; p.R = 33600; p.conv_nb_states = 16;
    p.use_high_carrier = 1; p.calling = calling;
    { extern void dsp_init(void); dsp_init(); }
    V34_static_init();
    V34_demod_init(&rx, &p);
    v34_dbg = 1; rx.dbg_last = -1; rx.dbg_n = 0;
    { extern int eq_notrack, eq_freeze; char *a=getenv("SIPFAX_EQ_NOTRACK"), *b=getenv("SIPFAX_EQ_FREEZE");
      eq_notrack = a?atoi(a):0; eq_freeze = b?atoi(b):0;
      fprintf(stderr, "[eq] knobs notrack=%d freeze=%d\n", eq_notrack, eq_freeze); }
    f = fopen(path, "rb");
    if (!f) { perror(path); return; }
    fprintf(stderr, "[dec] decoding %s as %s-role demod (S=3429), input x5\n",
            path, calling ? "CALLER" : "ANSWER");
    while ((n = fread(buf, 2, 512, f)) > 0) {
        for (i = 0; i < n; i++) {
            int v = buf[i] * 5;
            if (v > 32767) v = 32767; if (v < -32768) v = -32768;
            buf[i] = (s16)v;
        }
        V34_demod(&rx, buf, n);
    }
    fclose(f);
    fprintf(stderr, "[dec] END: final demod state=%d (WAIT_S1=%d PP=%d TRN=%d)\n",
            rx.state, V34_STARTUP3_WAIT_S1, V34_STARTUP3_PP, V34_STARTUP3_TRN);
    {
        extern int v34_symdump[40000], v34_symdump_n;
        int qi; FILE *df = fopen("/tmp/symdump.txt", "w");
        fprintf(stderr, "[dec] dumped %d equalized TRN/J quadrants -> /tmp/symdump.txt\n", v34_symdump_n);
        for (qi = 0; qi < v34_symdump_n; qi++) fprintf(df, "%d", v34_symdump[qi]);
        fprintf(df, "\n"); fclose(df);
        { extern int v34_softi[40000], v34_softq[40000]; int si2;
          FILE *sf = fopen("/tmp/soft.txt", "w");
          for (si2 = 0; si2 < v34_symdump_n; si2++) fprintf(sf, "%d %d\n", v34_softi[si2], v34_softq[si2]);
          fclose(sf); fprintf(stderr, "[dec] wrote %d soft symbols -> /tmp/soft.txt\n", v34_symdump_n); }
    }
}


/* ---- offline Phase-3 encode harness ---- */
static int enc_get_bit(void *o){ return 1; }
/* SIPFAX: the shell mapper is a SHAPING code - it picks inner constellation points far
   more often than outer ones, so the mean |c|^2 actually transmitted is well below the
   uniform average over the constellation (measured 24.3 against 122.3 at R=16800). The
   receiver needs that shaped mean to set its gain, and it cannot be derived from the
   constellation geometry alone.

   Searching for the gain instead does NOT work, and the failure is not a tuning problem.
   The only objective available to the receiver is distance to the odd-integer lattice,
   and that is MINIMISED by collapsing every symbol onto the innermost ring - a collapsed
   constellation genuinely does sit on the lattice. Measured: a flattering lattice-rms of
   0.109 with 4 of 12 points used, all four rotations of the single point (1,1). An
   anti-collapse power floor cannot rescue it either: at 0.25x the uniform mean the floor
   sat ABOVE the true shaped mean and excluded the right answer, and at the corrected
   value it sits BELOW |c|^2 = 2 and admits the collapse. There is no threshold between.

   So remove the freedom rather than constrain it: run OUR OWN shell mapper at the
   negotiated rate - the same code the peer is running - and measure what it emits. */
static double v34_shaped_meanc2(int R, int nb_states)
{
    V34State p;
    static V34DSPState tx;          /* static: far too big for the stack */
    long i;
    memset(&p, 0, sizeof(p));
    p.S = V34_S3429; p.R = R; p.conv_nb_states = nb_states;
    p.use_high_carrier = 1; p.calling = 0;
    memset(&tx, 0, sizeof(tx));
    V34_mod_init(&tx, &p);
    tx.get_bit = enc_get_bit; tx.opaque = 0;
    shp_acc = 0; shp_n = 0; shp_on = 1;
    for (i = 0; i < 4000; i++) encode_mapping_frame(&tx);
    shp_on = 0;
    return shp_n ? shp_acc / (double)shp_n / (128.0*128.0) : 0.0;
}

/* SIPFAX: generate REAL data-mode AUDIO from our own encoder, so the live receive chain
   can be scored against a signal we know is a valid V.34 data signal.

   This gap needed closing. The dataloop harness that reports "100% bit match" installs
   g_symtap and hands the receiver SYMBOLS directly - it never runs V34_mod or
   V34_demod_cma at all. So the live audio path has never once been validated end to end,
   and every conclusion of the form "our receiver resolves our own signal but not the
   caller's" rested on a test that skipped the receiver being blamed.

   The file starts with 4-point Phase-4 material, which CMA can acquire, and then switches
   to data - mirroring a real call, and letting SIPFAX_FORCE_DATA_AT line the receiver's
   switch up with the transmitter's. */
/* SIPFAX: round-trip the shell mapper. index_to_rings() maps a K-bit index to four ring
   pairs; rings_to_index() is meant to be its exact inverse, and it carries n = K of the
   b bits in every mapping frame (11 of 22 at R=9600). The loopback only ever exercises it
   on exact integer symbols, so an inverse that is wrong for part of its domain would pass
   there and corrupt half of every frame on a real signal. Enumerate the whole domain. */
void V34_ringtest(void)
{
    V34State p; static V34DSPState s;
    int R = 9600, i, k, bad = 0, first = -1;
    char *e = getenv("SIPFAX_RINGTEST_R"); if (e && atoi(e) > 0) R = atoi(e);
    memset(&p, 0, sizeof(p));
    p.S = V34_S3429; p.R = R; p.conv_nb_states = 64;
    p.use_high_carrier = 1; p.calling = 0;
    V34_static_init(); { extern void dsp_init(void); dsp_init(); }
    memset(&s, 0, sizeof(s));
    V34_init_low(&s, &p, 1);
    fprintf(stderr, "[ring] R=%d  b=%d K=%d q=%d M=%d L=%d  -> enumerating %d indices\n",
            R, s.b, s.K, s.q, s.M, s.L, 1 << s.K);
    for (i = 0; i < (1 << s.K); i++) {
        int m[4][2], back;
        index_to_rings(&s, m, i);
        back = rings_to_index(&s, m);
        if (back != i) {
            bad++;
            if (first < 0) {
                first = i;
                fprintf(stderr, "[ring] FIRST MISMATCH index %d -> rings "
                        "(%d,%d)(%d,%d)(%d,%d)(%d,%d) -> %d\n", i,
                        m[0][0],m[0][1],m[1][0],m[1][1],
                        m[2][0],m[2][1],m[3][0],m[3][1], back);
            }
        }
        for (k = 0; k < 4; k++) {
            if (m[k][0] < 0 || m[k][0] >= s.M || m[k][1] < 0 || m[k][1] >= s.M) {
                fprintf(stderr, "[ring] index %d produced ring out of [0,M): (%d,%d)\n",
                        i, m[k][0], m[k][1]);
                i = (1 << s.K); break;
            }
        }
    }
    fprintf(stderr, "[ring] round-trip mismatches: %d of %d  (%.2f%%)\n",
            bad, 1 << s.K, 100.0*bad/(double)(1 << s.K));
}

void V34_datagen_test(const char *path)
{
    V34State p; static V34DSPState tx; s16 out[512]; FILE *f; int b, nb4, nball;
    int R = 9600, trel = 64; double p4s = 4.0, total = 16.0;
    double lvl = 10.0, acc = 0, pk = 0; long nac = 0;
    char *e;
    e = getenv("SIPFAX_GEN_R");    if (e) R = atoi(e);
    e = getenv("SIPFAX_GEN_P4S");  if (e) p4s = atof(e);
    e = getenv("SIPFAX_GEN_SEC");  if (e) total = atof(e);
    e = getenv("SIPFAX_GEN_TREL"); if (e) trel = atoi(e);
    e = getenv("SIPFAX_GEN_LEVEL"); if (e) lvl = atof(e);
    memset(&p, 0, sizeof(p));
    p.S = V34_S3429; p.R = R; p.conv_nb_states = trel;
    p.use_high_carrier = 1;
    /* SIPFAX: generate as the CALLER. The scrambler polarity is role-dependent -
       get_bit() uses calling ? GPC : GPA and put_bit() uses !calling ? GPC : GPA - so an
       answer-side generator scrambles with GPA while our answer-side receiver descrambles
       with GPC, and the recovered bits come out 50% ones however well the symbols decode.
       That is a property of the test rig, not of the modem: on a real call the caller
       scrambles with GPC and we descramble with GPC. Generating as the caller makes the
       loop consistent, so the decoded bits are directly checkable - the source is constant
       1, so a correct end-to-end path yields 100% ones. SIPFAX_GEN_CALLING overrides. */
    { char *ec = getenv("SIPFAX_GEN_CALLING"); p.calling = ec ? atoi(ec) : 1; }
    V34_static_init(); { extern void dsp_init(void); dsp_init(); }
    memset(&tx, 0, sizeof(tx));
    V34_mod_init(&tx, &p);
    tx.get_bit = enc_get_bit; tx.opaque = 0;
    tx.J_received = 1; tx.is_16states = 0; tx.mp_16point = 0;
    tx.state = V34_STARTUP4_S;
    nb4   = (int)(p4s   * 8000 / 512);
    nball = (int)(total * 8000 / 512);
    { extern FILE *g_txsymf; char *ge = getenv("SIPFAX_GEN_GT");
      if (ge) { g_txsymf = fopen(ge, "w");
                fprintf(stderr, "[gen] transmit-symbol ground truth -> %s\n", ge); } }
    f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fprintf(stderr, "[gen] R=%d trellis=%d : %.1fs of 4-point Phase 4, then DATA to %.1fs -> %s\n",
            R, trel, p4s, total, path);
    for (b = 0; b < nball; b++) {
        if (b == nb4) {
            {   /* SIPFAX: same tx_amp fix as the V34_STARTUP4_E path - this harness jumps
                   straight to V34_DATA and would otherwise inherit the 4-point amplitude. */
                int ci, nq = tx.L / 4; double acc = 0;
                for (ci = 0; ci < nq; ci++)
                    acc += (double)tx.constellation[ci][0]*tx.constellation[ci][0]
                         + (double)tx.constellation[ci][1]*tx.constellation[ci][1];
                if (nq > 0 && acc > 0) {
                    double mp = acc / nq;
                    fprintf(stderr, "[gen] data constellation L=%d mean|c|^2=%.2f -> tx_amp %d (was %d)\n",
                            tx.L, mp, (int)CALC_AMP(mp), tx.tx_amp);
                    tx.tx_amp = CALC_AMP(mp);
                }
            }
            tx.state = V34_DATA;
            fprintf(stderr, "[gen] switching to DATA at t=%.2f s\n", b*512/8000.0);
        }
        int k;
        V34_mod(&tx, out, 512);
        /* SIPFAX: SCALE DOWN. tx_amp is CALC_AMP(S_POWER) = 11585 and tx_buf is
           (si*tx_amp)>>7, so lattice coordinate 1 alone already reaches 11585 - against a
           real line signal measured at rms 1714. Unscaled, the generated data mode ran at
           rms 17152 with the peak pinned at 32767 on every block: continuous clipping.
           That matters more than tidiness, because clipping is a NON-LINEARITY - it
           scrambles symbol phase while roughly preserving the amplitude distribution, and
           it hits large excursions hardest. A ground-truth test fed with a clipped signal
           measures the clipping, not the receiver, and reproduces exactly the
           amplitude-dependent phase error we were chasing. */
        for (k = 0; k < 512; k++) {
            double v = out[k] / lvl;
            if (v >  32000) v =  32000;
            if (v < -32000) v = -32000;
            out[k] = (s16)lrint(v);
            acc += v*v; nac++;
            if (fabs(v) > pk) pk = fabs(v);
        }
        fwrite(out, 2, 512, f);
    }
    fclose(f);
    { extern FILE *g_txsymf; if (g_txsymf) { fclose(g_txsymf); g_txsymf = 0; } }
    fprintf(stderr, "[gen] wrote %.1f s at level/%.1f: rms %.0f peak %.0f"
            "  (a real line measures rms ~1714 peak ~4600)\n",
            nball*512/8000.0, lvl, nac ? sqrt(acc/nac) : 0.0, pk);
    if (pk > 31000) fprintf(stderr, "[gen] WARNING: still clipping - raise SIPFAX_GEN_LEVEL\n");
}

void V34_encode_test(const char *path)
{
    extern int v34_dbg; extern long v34_ntrn, v34_nj;
    V34State p; V34DSPState tx; s16 out[512]; FILE *f; int b;
    memset(&p, 0, sizeof(p));
    p.S = V34_S3429; p.R = 33600; p.conv_nb_states = 16;
    p.use_high_carrier = 1; p.calling = 0;
    V34_static_init();
    { extern void dsp_init(void); dsp_init(); }
    memset(&tx, 0, sizeof(tx));
    V34_mod_init(&tx, &p);
    tx.get_bit = enc_get_bit; tx.opaque = 0;
    v34_dbg = 1; tx.dbg_last2 = -1; v34_ntrn = 0; v34_nj = 0;
    {   /* SIPFAX_ENC_P4=16|4: skip straight to Phase 4 with the given constellation,
           as if the caller's J had commanded it - validates the 16-point TRN/MP TX
           offline (decode the output with trn_score/mp_decode). */
        char *p4 = getenv("SIPFAX_ENC_P4");
        if (p4) {
            tx.J_received = 1;
            tx.is_16states = (atoi(p4) == 16) ? 1 : 0;
            tx.mp_16point = tx.is_16states;
            tx.state = V34_STARTUP4_S;
            fprintf(stderr, "[enc] Phase-4 mode: %s-point TRN/MP\n", tx.is_16states ? "16" : "4");
        }
    }
    f = fopen(path, "wb");
    fprintf(stderr, "[enc] encoding answer TX (S=3429) -> %s\n", path);
    for (b = 0; b < (int)(8.0 * 8000 / 512); b++) {   /* ~8 s */
        V34_mod(&tx, out, 512);
        { int _i, mx=0; for(_i=0;_i<512;_i++){int a=out[_i]<0?-out[_i]:out[_i]; if(a>mx)mx=a;}
          if (b<4 || (b%15)==0) fprintf(stderr, "[enc] blk %d state=%d tx_amp=%d bufsz=%d max|out|=%d\n",
              b, tx.state, tx.tx_amp, tx.tx_buf_size, mx); }
        fwrite(out, 2, 512, f);
    }
    fclose(f);
    fprintf(stderr, "[enc] done: V34_send_TRN called %ld times, V34_send_J %ld times, final state=%d\n",
            v34_ntrn, v34_nj, tx.state);
}


/* ---- Stage-1a CMA/DD complex-baseband equalizer (validated design, float) ---- */
void V34_cma_decode_file(const char *path)
{
    static s16 raw[512000];
    static double bi[512000], bq[512000], yi[600000], yq[600000];
    static double sym_i[600000], sym_q[600000];
    FILE *f = fopen(path, "rb");
    int N, n, m, i, M, start, phase, ncma, cnt, nsym;
    double FCarr = 24000.0/7.0*4.0/7.0, FS = 8000.0, BAUD = 24000.0/7.0;
    double SPS = FS / BAUD, psum, nrm, tskip, mu_cma, mu_dd, R;
    #define CMANT 32
    double wi[CMANT], wq[CMANT], bufi[CMANT], bufq[CMANT];
    char *e;
    if (!f) { perror(path); return; }
    N = fread(raw, 2, 512000, f); fclose(f);
    fprintf(stderr, "[cma] read %d samples from %s\n", N, path);
    /* downconvert to complex baseband */
    for (n = 0; n < N; n++) {
        double ph = -2.0*M_PI*FCarr*(double)n/FS;
        bi[n] = (double)raw[n]*cos(ph);
        bq[n] = (double)raw[n]*sin(ph);
    }
    /* resample to T/2 (2 samples/symbol), linear interpolation */
    M = (int)((double)N / SPS * 2.0);
    if (M > 600000) M = 600000;
    psum = 0.0;
    for (m = 0; m < M; m++) {
        double idx = (double)m * SPS / 2.0; int i0 = (int)idx; double fr = idx - i0;
        if (i0+1 >= N) { yi[m]=yq[m]=0; continue; }
        yi[m] = bi[i0]*(1-fr) + bi[i0+1]*fr;
        yq[m] = bq[i0]*(1-fr) + bq[i0+1]*fr;
        psum += yi[m]*yi[m] + yq[m]*yq[m];
    }
    nrm = sqrt(psum / (double)M);
    if (nrm < 1e-12) nrm = 1.0;
    for (m = 0; m < M; m++) { yi[m] /= nrm; yq[m] /= nrm; }
    /* params (env-tunable for offline sweeps) */
    e = getenv("SIPFAX_CMA_TSKIP"); tskip = e ? atof(e) : 0.30;
    e = getenv("SIPFAX_CMA_PHASE"); phase = e ? atoi(e) : 0;
    e = getenv("SIPFAX_CMA_MU");    mu_cma = e ? atof(e) : 2e-3;
    e = getenv("SIPFAX_DD_MU");     mu_dd  = e ? atof(e) : 3e-3;
    e = getenv("SIPFAX_CMA_N");     ncma   = e ? atoi(e) : 1500;
    start = (int)(tskip * FS / SPS * 2.0) + phase;
    R = 1.0/sqrt(2.0);
    for (i=0;i<CMANT;i++){wi[i]=wq[i]=bufi[i]=bufq[i]=0;} wi[CMANT/2]=1.0;
    cnt = 0; nsym = 0;
    for (m = start; m < M; m++) {
        for (i=CMANT-1;i>0;i--){bufi[i]=bufi[i-1];bufq[i]=bufq[i-1];}
        bufi[0]=yi[m]; bufq[0]=yq[m];
        if (((m-start)&1)==0) {
            double oyi=0, oyq=0, ei, eqv, mu, di, dq, mod2, g;
            for (i=0;i<CMANT;i++){ oyi += wi[i]*bufi[i]-wq[i]*bufq[i]; oyq += wi[i]*bufq[i]+wq[i]*bufi[i]; }
            if (cnt < ncma) { mod2=oyi*oyi+oyq*oyq; g=(1.0-mod2); ei=g*oyi; eqv=g*oyq; mu=mu_cma; }
            else { di=(oyi>=0?R:-R); dq=(oyq>=0?R:-R); ei=di-oyi; eqv=dq-oyq; mu=mu_dd; }
            for (i=0;i<CMANT;i++){
                double gi = ei*bufi[i] + eqv*bufq[i];
                double gq = eqv*bufi[i] - ei*bufq[i];
                wi[i] += mu*gi; wq[i] += mu*gq;
            }
            sym_i[nsym]=oyi; sym_q[nsym]=oyq; cnt++; nsym++;
        }
    }
    /* dump + tail SER (convention-free) */
    {
        FILE *of = fopen("/tmp/cma-soft.txt","w");
        int t0 = nsym>3000 ? nsym-3000 : 0, errc=0, tc=0;
        double tp=0, ts2;
        for (i=0;i<nsym;i++) fprintf(of, "%.4f %.4f\n", sym_i[i], sym_q[i]);
        fclose(of);
        for (i=t0;i<nsym;i++) tp += sym_i[i]*sym_i[i]+sym_q[i]*sym_q[i];
        ts2 = sqrt(tp/(double)(nsym-t0)); if (ts2<1e-12) ts2=1.0;
        for (i=t0;i<nsym;i++){
            double a=sym_i[i]/ts2, b=sym_q[i]/ts2;
            double da=fabs(a)-R, db=fabs(b)-R, d=sqrt(da*da+db*db);
            tc++; if (d>0.3) errc++;
        }
        fprintf(stderr, "[cma] %d symbols; tail SER(>0.3)=%.2f%% (%d/%d)  [ncma=%d mu_cma=%.4f mu_dd=%.4f tskip=%.2f phase=%d]\n",
                nsym, 100.0*errc/(tc?tc:1), errc, tc, ncma, mu_cma, mu_dd, tskip, phase);
    }
}


FILE *cma_dumpf = 0;
FILE *cma_t2df = 0;
FILE *p4bitf = 0;
int cma_t1 = 18, cma_t2 = 23;   /* caller GPC default; GPA=5,23 */
/* ---- Stage 1c: streaming CMA/DD + carrier-recovery demod (validated design) ---- */
#define CMANT 32
static double cma_maxrot_ones(int *q, int W)
{
    /* absolute-decode at 4 rotations, GPC descramble FIR x[n]=y[n]^y[n-18]^y[n-23],
       return max ones-fraction after descrambler warmup (caller scrambles with GPC). */
    static int y[256], x[256];
    double best = 0.0; int rot, i;
    for (rot = 0; rot < 4; rot++) {
        int nb = 0, ones = 0, cnt = 0; double f;
        for (i = 0; i < W; i++) { int z = ((q[i]-rot) & 3); y[nb++] = z&1; y[nb++] = (z>>1)&1; }
        for (i = 0; i < nb; i++) { int v = y[i]; if (i>=cma_t1) v ^= y[i-cma_t1]; if (i>=cma_t2) v ^= y[i-cma_t2]; x[i] = v; }
        for (i = 48; i < nb; i++) { ones += x[i]; cnt++; }
        f = cnt ? (double)ones/cnt : 0.0; if (f < 0.5) f = 1.0 - f;
        if (f > best) best = f;
    }
    return best;
}
static void V34_cma_t2sample(V34DSPState *s, double yi, double yq)
{
    if (cma_t2df) fprintf(cma_t2df, "%.3f %.3f\n", yi, yq);
    double R = 0.70710678, g, oi, oq, ei, eq, mu; int i;
    static double mu_cma = -1, mu_dd; static int ncma1, ncma2;
    if (mu_cma < 0) { char *e;
        e = getenv("SIPFAX_CMA_MU"); mu_cma = e ? atof(e) : 2e-3;
        e = getenv("SIPFAX_DD_MU");  mu_dd  = e ? atof(e) : 3e-3;
        e = getenv("SIPFAX_CMA_N1"); ncma1 = e ? atoi(e) : 800;
        e = getenv("SIPFAX_CMA_N2"); ncma2 = e ? atoi(e) : 1600;
    }
    { double p = yi*yi + yq*yq; static double gate = -1;
      if (gate < 0) { char *e = getenv("SIPFAX_CMA_GATE"); gate = e ? atof(e) : 50000.0; }
      if (s->cma_pow < 1e-9) s->cma_pow = p + 1.0;
      if (!s->cma_started) s->cma_pow = 0.98*s->cma_pow + 0.02*p;   /* gain FROZEN after settle (batch parity) */
      if (!s->cma_started) {
          if (s->cma_pow > gate) s->cma_warm++; else s->cma_warm = 0;
          if (s->cma_warm >= 400) { s->cma_started = 1;
              { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] signal locked, power=%.0f\n", s->cma_pow); } }
          else return;
      }
    }
    { double p2 = yi*yi + yq*yq;                      /* squelch: hold everything in silence */
      s->cma_sq = 0.99*s->cma_sq + 0.01*p2;
      if (s->cma_started && s->cma_sq < 0.05*s->cma_pow) return; }
    g = (s->cma_pow > 1e-12) ? 1.0/sqrt(s->cma_pow) : 1.0;
    yi *= g; yq *= g;
    for (i = CMANT-1; i > 0; i--) { s->cma_bufi[i] = s->cma_bufi[i-1]; s->cma_bufq[i] = s->cma_bufq[i-1]; }
    s->cma_bufi[0] = yi; s->cma_bufq[0] = yq;
    s->cma_t2++;
    oi = oq = 0;
    for (i = 0; i < CMANT; i++) { oi += s->cma_wi[i]*s->cma_bufi[i] - s->cma_wq[i]*s->cma_bufq[i];
                                  oq += s->cma_wi[i]*s->cma_bufq[i] + s->cma_wq[i]*s->cma_bufi[i]; }
    /* SIPFAX: the equaliser output used to be computed only on symbol instants - the odd
       T/2 phase returned before this convolution. Gardner needs the midpoint, so compute
       both and spend one extra CMANT-tap convolution per symbol (a few hundred kMAC/s). */
    if (s->cma_t2 & 1) { s->cma_gmi = oi; s->cma_gmq = oq; return; }
    {   /* Gardner TED -> the resampler's phase and rate. See FINDINGS 38: without this
           the sampling instant walks with the ~20 ppm offset across the RTP path and the
           EVM traces a V across every burst (37.6% / 3.6% / 15.6%), which is what has
           been capping the link at ~16 dB on a line measured at 28.9 dB. */
        static double dkp = -1, dki, dsg;
        double ted, pwn;
        if (dkp < 0) { char *e;
            /* SIPFAX: swept - 0.10/0.002 was carried over from the block path, where the
               loop re-converges on every pass over buffered audio. The live loop runs once,
               continuously, and those gains made the tracked clock hunt over 24-48 ppm
               instead of holding. 0.01/3e-7 holds. */
            e = getenv("SIPFAX_DTED_KP");   dkp = e ? atof(e) : 0.01;
            e = getenv("SIPFAX_DTED_KI");   dki = e ? atof(e) : 3e-7;
            e = getenv("SIPFAX_DTED_SIGN"); dsg = e ? atof(e) : -1.0;
        }
        ted = (oi - s->cma_gpi)*s->cma_gmi + (oq - s->cma_gpq)*s->cma_gmq;
        pwn = oi*oi + oq*oq + s->cma_gpi*s->cma_gpi + s->cma_gpq*s->cma_gpq + 1e-9;
        ted = dsg * ted / pwn;
        { extern double g_tedacc; extern long g_tedn;      /* SIPFAX: lock indicator */
          g_tedacc += fabs(ted); g_tedn++; }
        {   /* SIPFAX: FREEZE the symbol clock at data-mode entry, keeping the rate the
               loop converged to during Phase 4 - the same treatment the taps get, and for
               the same reason.

               Gardner's error is unbiased only on a constant-modulus signal; its
               self-noise scales with the amplitude variance of the constellation, so on a
               shaped 12/48-point set it is largely noise. Measured on the caller's data:
               mean|TED| is ~0.35 whether this loop runs or is switched off entirely
               (0.339-0.358 on, 0.346-0.398 off), i.e. it reduces the timing error by
               nothing - while dragging the tracked clock from 31 to 48 ppm. That walk is
               the loop chasing its own noise, not a real drift. Phase-4 TRN IS constant
               modulus, so the rate it converged to there is the trustworthy one; hold it.
               SIPFAX_DATA_TED=1 keeps the loop running in data mode for comparison. */
            /* SIPFAX 2026-08-29: THIS NOW DEFAULTS ON. The reasoning above is sound only
               for our OWN signal, where the loopback and stream harnesses share a clock with
               the transmitter, so there is no real drift for the loop to track and anything
               it does is noise. A LIVE CALLER has an independent clock, and freezing ours
               guarantees the sampling phase walks. Measured on 6.3 s of a real caller's data
               (call 4, 2026-08-29), scored by lattice-rms - constellation-aware, unlike the
               4-point EVM print below, which measures QPSK-ness and would happily reward a
               flattened constellation:

                   frozen  : lattice 0.489, pre-EQ kurtosis 1.137, and the 4-point EVM decays
                             25 -> 4.3 -> 9.1 -> 23 -> 40 -> 53 -> 60% across the window
                   tracking: lattice 0.310, pre-EQ kurtosis 1.317, EVM holds 3-4% throughout

               The kurtosis is the honest half: 1.137 -> 1.317 means the equaliser is now fed
               a properly sampled multi-ring constellation instead of a smeared one. Our own
               signal is unaffected either way (loopback 99.9%/99.8%, stream 99.8% with the
               loop on or off), so the freeze cost the live case and bought nothing. */
            static int ted_in_data = -1;
            if (ted_in_data < 0) { char *e = getenv("SIPFAX_DATA_TED");
                                   ted_in_data = e ? atoi(e) : 1; }
            if (s->p4_e_rx && !ted_in_data) goto ted_done;
        }
        {   /* SIPFAX: SMOOTH THE TED BEFORE THE LOOP. The loop must keep tracking - the far
               end has an independent clock - but the Gardner error is noisy at symbol rate,
               and the resulting phase dither smears a multi-ring constellation. Measured on
               the caller's data: near-freezing the loop lifts envelope kurtosis from 1.32 to
               1.63 (matching a fixed-phase reference chain) while lattice-rms collapses to
               0.508 because the phase stops tracking; lowering the gains fails the same way.
               Reducing the loop GAIN trades tracking for jitter. A pre-loop EMA does not: its
               DC gain is 1, so the response to slow clock drift is unchanged, while noise
               above the loop bandwidth - which cannot be real clock drift - is attenuated.
               SIPFAX_TED_AVG=N sets the averaging length (1 = old behaviour).
               MEASURED: NO EFFECT. N = 1/4/8/16/32/64/128 gives lattice-rms 0.310 on call4
               and 0.321 on call3, unchanged to three decimals (one 0.235 at N=64 on call4
               alone did not reproduce on call3). Left in place, defaulting to 1, purely so
               the null is on the record - the jitter hypothesis it was built to test is dead,
               and for the real reason see the commit that follows: the caller was never in
               data mode at all, so none of this front-end work was measuring what it
               claimed to. */
            static int tavg = -1;
            if (tavg < 0) { char *e = getenv("SIPFAX_TED_AVG"); tavg = e ? atoi(e) : 1;
                            if (tavg < 1) tavg = 1; }
            if (tavg > 1) {
                double aa = 1.0 / (double)tavg;
                s->ted_ema += aa * (ted - s->ted_ema);
                ted = s->ted_ema;
            }
        }
        s->cma_pos  += dkp * ted;
        s->cma_tinc += dki * ted;
        if (s->cma_tinc >  0.005) s->cma_tinc =  0.005;   /* ~4000 ppm: past any real clock */
        if (s->cma_tinc < -0.005) s->cma_tinc = -0.005;
        ted_done:
        s->cma_gpi = oi; s->cma_gpq = oq;
    }
    { static int skip = -1; if (skip < 0) { char *e = getenv("SIPFAX_CMA_SKIP"); skip = e ? atoi(e) : 300; }
      if (s->cma_skipn < skip) { s->cma_skipn++; return; } }
    {   /* running 4-point quality (|EMA of u^4|) drives phase transitions:
           A->B when the eye opens, B->C when tight. Self-timed for any channel
           (fixed counters either starved real-channel convergence or ate the
           caller's TRN). ncma1/ncma2 remain as hard caps. */
        double m2 = oi*oi + oq*oq;
        if (m2 > 1e-12) {
            double iv = 1.0/sqrt(m2), ui = oi*iv, uq = oq*iv;
            double u2i = ui*ui - uq*uq, u2q = 2.0*ui*uq;
            double u4i = u2i*u2i - u2q*u2q, u4q = 2.0*u2i*u2q;
            /* DIFFERENTIAL u4 quality: u4[n]*conj(u4[n-1]) cancels any constant
               rotation rate (caller carrier offset), which used to make a good
               eye read as ~0.1 and starve the phase transitions */
            double di_ = u4i*s->cma_pu4i + u4q*s->cma_pu4q;
            double dq_ = u4q*s->cma_pu4i - u4i*s->cma_pu4q;
            s->cma_c4i += (di_ - s->cma_c4i)/128.0; s->cma_c4q += (dq_ - s->cma_c4q)/128.0;
            s->cma_pu4i = u4i; s->cma_pu4q = u4q;
        }
        s->cma_phn++;
        if (s->cma_phase == 0) {
            double cq = sqrt(s->cma_c4i*s->cma_c4i + s->cma_c4q*s->cma_c4q);
            if (cq > (srx_rx16() && s->p4_mode ? 0.30*0.359 : 0.30) && s->cma_phn >= 500) {
                s->cma_phase = 1; s->cma_phn = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] eye open (q=%.2f) -> DD at sym %d\n", cq, s->cma_cnt); }
            } else if (s->cma_phn >= 1500) {
                /* eye never opened (likely trained on non-CM prelude e.g. PP):
                   re-seed and retry until we land on TRN */
                int rj; for (rj = 0; rj < CMANT; rj++) { s->cma_wi[rj] = 0; s->cma_wq[rj] = 0; }
                s->cma_wi[CMANT/2] = 1.0; s->cma_phn = 0; s->cma_c4i = s->cma_c4q = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] eye stuck (q=%.2f), reseed+retry at sym %d\n", cq, s->cma_cnt); }
            }
        } else if (s->cma_phase == 1) {
            double cq = sqrt(s->cma_c4i*s->cma_c4i + s->cma_c4q*s->cma_c4q);
            if (cq > (srx_rx16() && s->p4_mode ? 0.65*0.359 : 0.65) && s->cma_phn >= 300) {
                s->cma_phase = 2; s->cma_phn = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] tight (q=%.2f) -> track at sym %d\n", cq, s->cma_cnt); }
            } else if (s->cma_phn >= 2500) {
                int rj; for (rj = 0; rj < CMANT; rj++) { s->cma_wi[rj] = 0; s->cma_wq[rj] = 0; }
                s->cma_wi[CMANT/2] = 1.0; s->cma_phase = 0; s->cma_phn = 0; s->cma_c4i = s->cma_c4q = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] DD stuck (q=%.2f), back to CMA at sym %d\n", cq, s->cma_cnt); }
            }
        }
    }
    if (s->data_on && s->cma_phase != 2) {
        /* SIPFAX: DATA MODE - stop adapting, for the same reason the 16-point Phase-4 guard
           below exists, only more so. Blind CMA drives its output to a CONSTANT MODULUS, and
           a shaped data constellation carries a large part of its information in amplitude:
           at R=16800 the caller sends L=56 with shell mapping. Measured on a live call, the
           caller's data arrives with per-symbol |z|^2 kurtosis 1.471 (shaped) and reaches our
           slicer at 1.001 (constant envelope) - CMA had flattened every ring onto one. That
           is why acquisition scored lattice-rms 0.512 against a 0.577 no-lock floor and why
           an exhaustive gain/phase sweep could not do better than 0.490: the amplitude
           information was already gone before the fit. The Phase-3/4 taps already equalise
           this channel; keep them. */
        s->cma_phase = 2; s->cma_phn = 0;
        { extern int v34_dbg; static int once = 0; if (v34_dbg && !once) { once = 1;
            fprintf(stderr, "[cma] data mode: freezing taps (blind CMA flattens shaped data)\n"); } }
    }
    if (srx_rx16() && s->p4_mode != 0 && s->cma_phase != 2) {
        /* SIPFAX: 16-point Phase 4 - stop adapting. Blind CMA flattens the amplitudes the
           16-point constellation carries its data in (measured kurtosis 1.001 = constant
           modulus). The Phase-3 taps already equalise this channel. */
        s->cma_phase = 2; s->cma_phn = 0;
        { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] 16-point Phase 4: freezing taps (no blind CMA)\n"); }
    }
    if (s->cma_phase == 0) {                         /* Phase A: CMA blind (open eye) */
        double r2t = (srx_rx16() && s->p4_mode != 0) ? 1.32 : 1.0;   /* SIPFAX: 16-pt Godard radius */
        double m2 = oi*oi + oq*oq, gg = r2t - m2; ei = gg*oi; eq = gg*oq; mu = mu_cma;
        if (cma_dumpf) fprintf(cma_dumpf, "%.4f %.4f 0\n", oi, oq);
    } else if (s->cma_phase == 1) {                  /* Phase B: DD refine (FSE adapts, tightens) */
        double di, dq;
        if (srx_rx16() && s->p4_mode != 0) {   /* SIPFAX: real 16-point decision */
            int q16d, z16d;
            srx_slice16(oi, oq, s->rx16_rms, &q16d, &z16d, &di, &dq);
        } else { di = (oi>=0?R:-R); dq = (oq>=0?R:-R); }
        ei = di - oi; eq = dq - oq; mu = mu_dd;
        if (cma_dumpf) fprintf(cma_dumpf, "%.4f %.4f 0\n", oi, oq);
    } else {   /* Phase C v3: frozen taps + feedforward carrier + 4-candidate scrambler tracking */
        { static int tapdumped = 0;
          if (!tapdumped && cma_dumpf) { FILE *tf = fopen("/tmp/taps.txt","w"); int ti2;
            for (ti2 = 0; ti2 < CMANT; ti2++) fprintf(tf, "%.8f %.8f\n", s->cma_wi[ti2], s->cma_wq[ti2]);
            fclose(tf); tapdumped = 1; } }
        static u8 jpat[16]; static u8 jppat[16]; static int jpat_init = 0;
        unsigned int poly = (cma_t1 == 5) ? (1u|(1u<<18)) : (1u|(1u<<5));  /* GPA : GPC */
        double ct, st_, pi_, pq_, o2i, o2q, o4i, o4q; int qd, r, k, j, w, wm, b2s[4][2]; unsigned int regsnap[4];
        static u8 jpat16[16];
        if (!jpat_init) { for (k = 0; k < 16; k++) { jpat[k] = (0x0991 >> (15-k)) & 1;
                                             jpat16[k] = (0x0D91 >> (15-k)) & 1;
                                             jppat[k] = (0xF991 >> (15-k)) & 1; } jpat_init = 1; }
        o2i = oi*oi - oq*oq; o2q = 2*oi*oq;                 /* o^2 */
        o4i = o2i*o2i - o2q*o2q; o4q = 2*o2i*o2q;          /* o^4 */
        s->srx_s4i += o4i; s->srx_s4q += o4q;
        if (++s->srx_blkn >= 64) {
            if (s->srx_s4i != 0 || s->srx_s4q != 0) {
                /* UNWRAP across blocks: the raw estimate is only known mod 90 deg;
                   choose the +k*90 branch closest to the previous theta so the
                   derotation tracks TOTAL phase continuously. Without this, the
                   o->p quadrant mapping dithers per-symbol whenever accumulated
                   drift nears a quadrant boundary (the corruption we chased). */
                double raw = (atan2(s->srx_s4q, s->srx_s4i) - M_PI) / 4.0;
                double halfpi = M_PI / 2.0;
                double k = floor((s->srx_th - raw) / halfpi + 0.5);
                s->srx_th = raw + k * halfpi;
            }
            s->srx_s4i = s->srx_s4q = 0; s->srx_blkn = 0;
        }
        ct = cos(-s->srx_th); st_ = sin(-s->srx_th);
        pi_ = oi*ct - oq*st_; pq_ = oi*st_ + oq*ct;
        ei = 0; eq = 0; mu = 0;                             /* taps frozen */
        if (cma_dumpf) fprintf(cma_dumpf, "%.4f %.4f 1 %.4f %.4f %.4f\n", pi_, pq_, oi, oq, s->srx_th);
        /* sign-based slicer: boundaries on the AXES (max margin for the diagonal
           lattice). The old floor((ang+45)/90) slicer had boundaries ON the
           diagonals - i.e. through the constellation points themselves. */
        qd = (pi_ >= 0) ? (pq_ >= 0 ? 1 : 0) : (pq_ >= 0 ? 2 : 3);
        {   /* SIPFAX: the data-mode cloud is constant-envelope - kurtosis 1.001, every
               symbol in one radius bin, rho 0.002. A LINEAR filter cannot turn QAM into
               constant envelope, so either the caller is not sending QAM or something
               here is normalising per-symbol amplitude. Kurtosis of |x|^2 immediately
               BEFORE the equaliser (yi/yq, post matched filter) against immediately AFTER
               (oi/oq) settles which: 1.5-ish before and 1.0 after means we did it,
               1.0 before means the caller did. Also report whether the taps are still
               moving, since CMA's whole objective IS constant modulus. */
            static double k2a, k4a, k2b, k4b, tapsum; static long kn;
            double pa = yi*yi + yq*yq, pb = oi*oi + oq*oq;
            k2a += pa; k4a += pa*pa; k2b += pb; k4b += pb*pb;
            if (++kn % 4000 == 0) {
                extern int v34_dbg; double ts = 0; int ti;
                for (ti = 0; ti < CMANT; ti++)
                    ts += s->cma_wi[ti]*s->cma_wi[ti] + s->cma_wq[ti]*s->cma_wq[ti];
                {   /* SIPFAX: does the receiver LOSE LOCK at data-mode entry, or does the
                       signal simply stop being 4-point? 4-point EVM cannot tell them apart
                       once the constellation changes, so report indicators that do not
                       depend on the constellation at all: the mean |Gardner TED|, which is
                       small and steady while symbol timing is locked and grows when it is
                       not, and the tracked clock rate, which should hold near a constant
                       ppm. Three things change at E - the block receiver stops (4909), the
                       data path starts (3474), and CMA stops adapting (4302, added today) -
                       and the last of those is a change we made, so it has to be ruled in
                       or out by measurement rather than argument. */
                    extern double g_tedacc; extern long g_tedn;
                    if (v34_dbg)
                        fprintf(stderr, "[lock] %s  mean|TED| %.4f  clock %+.1f ppm  "
                                "rx16_rms %.4f  cma_pow %.3e\n",
                                s->p4_e_rx ? "DATA  " : "phase4",
                                g_tedn ? g_tedacc/g_tedn : 0.0,
                                s->cma_tinc/(7.0/6.0)*1e6, s->rx16_rms, s->cma_pow);
                    g_tedacc = 0; g_tedn = 0;
                }
                if (v34_dbg)
                    fprintf(stderr, "[kurt] %s pre-EQ %.3f  post-EQ %.3f  |w|^2 %.5f"
                            " (d %+.2e)  cma_phase=%d\n",
                            s->p4_e_rx ? "DATA  " : "phase4",
                            k2a > 0 ? (k4a/kn)/((k2a/kn)*(k2a/kn)) : 0.0,
                            k2b > 0 ? (k4b/kn)/((k2b/kn)*(k2b/kn)) : 0.0,
                            ts, ts - tapsum, s->cma_phase);
                tapsum = ts; k2a = k4a = k2b = k4b = 0; kn = 0;
            }
        }
        {   /* SIPFAX: how good is the LIVE receiver on a signal it certainly should
               handle? The block receiver reaches 3.4-4.7% EVM on the caller's Phase-4
               TRN; the live receiver's quality on the SAME signal has never been
               measured, so there is no way to tell whether data mode fails because it is
               data or because this receiver is simply worse. 4-point EVM against a
               decision at the tracked radius. */
            static double eacc, pacc; static long en;
            double A = sqrt((s->rx16_rms > 1e-12 ? s->rx16_rms : 1.0)/2.0);
            double dxr = (pi_ >= 0 ? A : -A), dyr = (pq_ >= 0 ? A : -A);
            eacc += (pi_-dxr)*(pi_-dxr) + (pq_-dyr)*(pq_-dyr);
            pacc += dxr*dxr + dyr*dyr;
            if (++en % 2000 == 0 && pacc > 0) {
                extern int v34_dbg;
                if (v34_dbg) fprintf(stderr, "[live] 4pt EVM %.1f%% over %ld syms (%s)\n",
                                     100.0*sqrt(eacc/pacc), en,
                                     s->p4_e_rx ? "DATA" : "phase4");
                eacc = 0; pacc = 0;
            }
        }
        s->cma_q[s->cma_qn & 127] = qd; s->cma_qn++;
        {   /* SIPFAX: track mean power so the 16-point slicer has a scale reference */
            double pw = pi_*pi_ + pq_*pq_;
            s->rx16_rms = (s->rx16_rms <= 0) ? pw : (0.995*s->rx16_rms + 0.005*pw);
        }
        if (s->p4_e_rx) {
            /* ---- DATA MODE ----------------------------------------------------
               Phase 4 is over; hand every symbol to Bellard's decoder. The taps,
               symbol timing and carrier phase carried here are the ones acquired on
               Phase-4 TRN - V.34 provides no training signal in data mode, so
               re-acquiring is not an option (and CMA cannot converge on a shaped
               constellation anyway).

               Scale: baseband_decode_impl wants lattice coordinate * 128 - verified
               against the encoder, where coordinates (1,5) are emitted as (128,640),
               and matching tcm_decision(), which reads a coordinate back as
               (sample>>8)*2+1. rx16_rms tracks mean symbol POWER, and the target mean
               power is <|c|^2>*128^2 over the negotiated constellation. */
            if (!s->data_on) {
                /* SIPFAX: V.34 resolves each direction's rate as the MINIMUM of the two
                   MP proposals. We were configuring the receiver from the CALLER's
                   proposal alone, so when we asked it for 9600 and it offered 16800 it
                   duly transmitted 9600 while we decoded as if it were 16800 (L=56 rather
                   than L=12). SIPFAX_MP_CA is the same value our MP advertised. */
                {   /* SIPFAX: freeze the AVERAGED taps, not the instantaneous ones.
                       On a channel-free signal the ideal equaliser is a delta, yet the
                       frozen taps measured 96.8% of energy in the peak with 3.2% spread
                       across the neighbours - pure CMA misadjustment at mu=2e-3, giving a
                       ~15 dB ISI ceiling when a shaped 12-point set needs about 17 dB.
                       Averaging over the adaptation window cuts that noise by roughly the
                       number of samples averaged. SIPFAX_TAPAVG=0 keeps the instantaneous
                       taps. */
                    static int tavg = -1; int q;
                    if (tavg < 0) { char *e = getenv("SIPFAX_TAPAVG"); tavg = e ? atoi(e) : 1; }
                    { extern int v34_dbg; if (v34_dbg)
                        fprintf(stderr, "[data] tap-average window: cma_an=%ld cma_phase=%d "
                                "p4_mode=%d cma_cnt=%d\n", s->cma_an, s->cma_phase,
                                s->p4_mode, s->cma_cnt); }
                    if (tavg && s->cma_an > 200) {
                        double d = 0, t = 0;
                        for (q = 0; q < CMANT; q++) {
                            double ai = s->cma_ai[q];      /* EMA, already normalised */
                            double aq = s->cma_aq[q];
                            d += (s->cma_wi[q]-ai)*(s->cma_wi[q]-ai)
                               + (s->cma_wq[q]-aq)*(s->cma_wq[q]-aq);
                            t += ai*ai + aq*aq;
                            s->cma_wi[q] = ai; s->cma_wq[q] = aq;
                        }
                        { extern int v34_dbg; if (v34_dbg)
                            fprintf(stderr, "[data] taps averaged over %ld samples; "
                                    "instantaneous deviated %.2f%% in energy\n",
                                    s->cma_an, t > 0 ? 100.0*d/t : 0.0); }
                    }
                }
                {   /* SIPFAX: dump the frozen equaliser taps at data-mode entry. For our
                       own generated signal there is NO channel, and TX sqrt-RC(0.1) against
                       RX sqrt-RC(0.1) is a Nyquist raised cosine, so the ideal equaliser is
                       exactly a delta. Any deviation is ISI the equaliser is ADDING - and a
                       modulus-based metric like the 4-point TRN EVM cannot see it, because
                       CMA's cost penalises only modulus variation. */
                    char *ed = getenv("SIPFAX_TAPDUMP");
                    if (ed) { FILE *tf = fopen(ed, "w"); int q;
                              if (tf) { for (q = 0; q < CMANT; q++)
                                            fprintf(tf, "%.9f %.9f\n", s->cma_wi[q], s->cma_wq[q]);
                                        fclose(tf);
                                        fprintf(stderr, "[data] equaliser taps -> %s (%d taps, centre %d)\n",
                                                ed, CMANT, CMANT/2); } }
                }
                {   /* SIPFAX: our own generated signal passes through NO channel, so the
                       correct equaliser for it is a DELTA. The taps carried into data mode
                       are whatever CMA converged to on 4-point Phase-4 TRN, and their
                       energy measures |w|^2 = 1.537 rather than 1.0 - on a channel-free
                       signal anything other than a delta can only ADD ISI. CMA is also
                       phase-blind and its cost function has its minimum in the wrong place
                       for a multi-ring set, so there is no reason its solution should be
                       right here. SIPFAX_EQ_DELTA=1 replaces the taps with a delta at
                       data-mode entry: if the score collapses, the equaliser is the fault. */
                    char *e = getenv("SIPFAX_EQ_DELTA");
                    if (e && atoi(e)) {
                        int q; double e2 = 0;
                        for (q = 0; q < CMANT; q++) e2 += s->cma_wi[q]*s->cma_wi[q]
                                                        + s->cma_wq[q]*s->cma_wq[q];
                        for (q = 0; q < CMANT; q++) { s->cma_wi[q] = 0; s->cma_wq[q] = 0; }
                        s->cma_wi[CMANT/2] = 1.0;
                        fprintf(stderr, "[data] equaliser replaced by a DELTA "
                                "(discarded taps had |w|^2 = %.4f)\n", e2);
                    }
                }
                {   /* SIPFAX: is the receiver sampling at the wrong point WITHIN the
                       symbol? Ground truth says the integer symbol alignment is exact (the
                       lag peak is sharp: 0.676 against 0.007 either side) but the magnitude
                       correlation is only 0.68 and the phase is random. A fractional timing
                       error does exactly that - each received symbol becomes a mix of two
                       adjacent transmitted symbols, whose phases are independent, so phase
                       scrambles while magnitude partly survives and the integer lag stays
                       sharp. One symbol is 7/3 = 2.333 input samples; sweep the offset. */
                    char *e = getenv("SIPFAX_DATA_TOFF");
                    if (e) { double t = atof(e); s->cma_pos += t;
                             fprintf(stderr, "[data] symbol-timing offset %+.3f input samples"
                                     " (%.3f symbol)\n", t, t/(7.0/3.0)); }
                }
                int our_ca = 0, their_ca, R;   /* 0 = follow the caller, no cap */
                { char *mc = getenv("SIPFAX_MP_CA"); if (mc) our_ca = atoi(mc); }
                their_ca = s->p4_mp_rate_ca > 0 ? s->p4_mp_rate_ca : 7;
                /* SIPFAX: 'ca' is the CALL-TO-ANSWER rate - what the caller TRANSMITS and
                   therefore what we must RECEIVE. our_ca defaulted to 4, hard-capping the
                   receiver at 9600 in every path; meanwhile our MP echoes the caller's own
                   r_ca straight back (see the MP builder), so on 2026-08-29 we told a caller
                   to transmit at ca=16800 and then demodulated it against a 9600, L=12
                   constellation. A receive cap is meaningless anyway: capping what we DECODE
                   cannot slow the far transmitter, only what we ADVERTISE can. Follow the
                   caller by default; SIPFAX_MP_CA still caps for experiments. */
                R = ((our_ca > 0 && our_ca < their_ca) ? our_ca : their_ca) * 2400;
                fprintf(stderr, "[data] rx rate: caller ca=%d, our cap=%s -> R=%d\n",
                        their_ca*2400, our_ca ? "set" : "none", R);
                s->conv_nb_states = (s->p4_trellis == 0) ? 16 : (s->p4_trellis == 1) ? 32 : 64;
                {   /* SIPFAX: does the caller precode even though our MP advertises
                       h = 0,0,0? Two comments in this file disagree about that, and it is
                       the one mechanism that fits the measured symptom exactly - correct
                       amplitude statistics with NO lattice structure at any rotation,
                       because Tomlinson-Harashima output is not lattice-valued until the
                       receiver applies the modulo inverse. The coefficients to invert with
                       are the ones the caller ADVERTISED (peer_h, decoded from its MP);
                       the 9.6.2 inverse at decode_mapping_frame reads s->h, so load them
                       there. SIPFAX_RX_PRECODE=1 to try it. */
                    char *ep = getenv("SIPFAX_RX_PRECODE");
                    if (ep && atoi(ep)) {
                        int hi5, hj5;
                        for (hi5 = 0; hi5 < 3; hi5++)
                            for (hj5 = 0; hj5 < 2; hj5++)
                                s->h[hi5][hj5] = s->peer_h[hi5*2+hj5];
                        s->rx_precode = 1;
                        fprintf(stderr, "[data] RX precoder inverse ON, peer h = "
                                "%.4f%+.4fj %.4f%+.4fj %.4f%+.4fj\n",
                                s->h[0][0]/16384.0, s->h[0][1]/16384.0,
                                s->h[1][0]/16384.0, s->h[1][1]/16384.0,
                                s->h[2][0]/16384.0, s->h[2][1]/16384.0);
                    }
                }
                v34_rx_data_params(s, R);
                {   /* mean |c|^2 of the negotiated constellation, in lattice units.
                       SIPFAX: s->L is the size of the FULL constellation, but
                       s->constellation[] only ever holds the QUARTER of it -
                       build_constellation() fills the whole (4x+1, 4y+1) coset sorted by
                       energy and every other user indexes it with < L_MAX/4 (see the
                       assert in the 9.6.1 mapper, which reaches the other three quadrants
                       by rotate_clockwise). Averaging over s->L entries therefore reached
                       nine points BEYOND the negotiated set: at R=9600 it averaged
                       energies {2,10,10,18,26,26,34,34,50,50,50,58} = 30.7 instead of the
                       true {2,10,10} = 7.33, inflating the mean 4.2x. Rotation preserves
                       magnitude, so the quarter's mean IS the full constellation's mean. */
                    int nq = s->L / 4, ci; double acc = 0;
                    for (ci = 0; ci < nq; ci++)
                        acc += (double)s->constellation[ci][0]*s->constellation[ci][0]
                             + (double)s->constellation[ci][1]*s->constellation[ci][1];
                    s->data_pw_target = (nq > 0 ? acc / nq : 30.0) * (128.0*128.0);
                    {   /* the UNIFORM mean above is only a fallback; measure the SHAPED
                           mean the shell mapper actually produces at this rate. */
                        double uni = (nq > 0 ? acc / nq : 30.0);
                        double shp = v34_shaped_meanc2(R, s->conv_nb_states);
                        if (shp > 0.05 * uni && shp < 4.0 * uni) {
                            s->data_pw_target = shp * (128.0*128.0);
                            fprintf(stderr, "[data] shaped mean |c|^2 = %.2f (uniform %.2f,"
                                    " ratio %.3f, amplitude %.3fx)\n",
                                    shp, uni, shp/uni, sqrt(shp/uni));
                        } else {
                            fprintf(stderr, "[data] shaped-mean measurement %.2f rejected"
                                    " against uniform %.2f - keeping uniform\n", shp, uni);
                        }
                    }
                    /* SIPFAX: that is the UNIFORM average over the constellation, but the
                       shell mapper is a SHAPING code - it uses inner points far more
                       often, so the transmitted mean power is much lower. Measured on the
                       encoder's own output at R=16800: mean |c|^2 = 24.3 against the
                       uniform 122.3, so the receive gain was sqrt(122.3/24.3) = 2.24x too
                       big and every symbol landed well outside its true point. That alone
                       accounts for the metric sitting at 175 on a PERFECT signal.
                       SIPFAX_DATA_PW overrides while the shaped mean is derived properly. */
                    { char *e6 = getenv("SIPFAX_DATA_PW");
                      if (e6) s->data_pw_target = atof(e6); }
                }
                s->phase_4d = 0; s->sync_count = 0; s->half_data_frame_count = 0;
                s->phase_mse = 0; s->phase_mse_cnt = 0;
                s->data_meanc2 = s->data_pw_target / (128.0*128.0);
                { if (g_data_srx < 0) { char *e = getenv("SIPFAX_DATA_SRX");
                                         g_data_srx = e ? atoi(e) : 0; }
                  /* with SRX tracking, data_th holds only the RESIDUAL on top of srx_th */
                  s->data_th = g_data_srx ? 0.0 : s->srx_th; }
                s->data_frq = 0.0;
                s->data_on = 1; s->data_n = 0;
                { extern int v34_dbg; if (v34_dbg)
                    fprintf(stderr, "[data] entering data mode: target mean power %.0f (rx16_rms %.0f)\n",
                            s->data_pw_target, s->rx16_rms); }
            }
            if (s->rx16_rms > 0) {
                /* SIPFAX: DECISION-DIRECTED carrier loop.
                   Phase C derotates with a 4th-power estimator, which is well matched to
                   the 4- and 16-point Phase-3/4 signals but nearly blind on a dense data
                   constellation - measured |E[x^4]|/E|x|^4 is 0.355 on the caller's TRN
                   and 0.006-0.014 in its data mode, a 25-50x weaker phase reference. So
                   in data mode we derotate with our own loop, seeded from the Phase-4
                   phase and driven by decisions against the negotiated constellation.
                   SIPFAX_DD_KP / SIPFAX_DD_KI override the loop constants. */
                static double kp = -1, ki = -1;
                double gc, ct2, st2, xi, xq, di, dq, pe, nrm2;
                /* SIPFAX: the DD carrier loop is now ON by default (kp 0.01, ki 1e-5).
                   It was defaulted OFF because it "could never bootstrap and dragged the
                   phase back off after acquisition" - true when that was written, but only
                   because every data symbol off the inner ring was WRAPPING in the
                   transmitter (put_sym stored (si*tx_amp)>>7 into an s16 with tx_amp left
                   at the 4-point Phase-4 value), so the decisions it fed on were noise.
                   With that fixed the symbols resolve to lattice-rms 0.166 and the loop
                   works: trellis metric 161 -> 115. Same story as the srx_th tracker,
                   which now improves the symbols to 0.147. */
                if (kp < 0) { char *e1 = getenv("SIPFAX_DD_KP"); kp = e1 ? atof(e1) : 0.01;
                              char *e2 = getenv("SIPFAX_DD_KI"); ki = e2 ? atof(e2) : 1e-5; }
                /* SIPFAX: DECISION-DIRECTED AGC. The open-loop gain used the UNIFORM
                   mean over the constellation, but the shell mapper is a shaping code
                   that favours inner points: measured on the encoder's own output at
                   R=16800 the transmitted mean |c|^2 is 24.3 against the uniform 122.3,
                   so the gain was sqrt(122.3/24.3) = 2.24x too big and every symbol
                   landed outside its true point (verified against encoder ground truth:
                   correlation 0.999 but gain 2.244). The shaped mean is not something
                   the receiver can know a priori, so track it: when the gain is too big
                   the nearest point sits inside the received sample, |d| < |y|, and the
                   loop pulls down. SIPFAX_DATA_AGC=0 restores the open-loop gain. */
                if (s->data_agc <= 0) s->data_agc = sqrt(s->data_meanc2 / s->rx16_rms);
                gc = s->data_agc;
                if (!s->data_acq_done) {
                    /* SIPFAX: the gain is no longer searched - v34_shaped_meanc2() measures
                       it from our own mapper, because the lattice objective is minimised by
                       collapsing the constellation. SIPFAX_ACQ_GAIN=1 restores the search
                       for comparison. Only the carrier phase is searched. */
                    static int acq_search_gain = -1;
                    static double acq_gain_win = 3.0;      /* +- dB around the measured gain */
                    if (acq_search_gain < 0) { char *e7 = getenv("SIPFAX_ACQ_GAIN");
                                               acq_search_gain = e7 ? atoi(e7) : 1;
                                               { char *e8 = getenv("SIPFAX_ACQ_GAIN_DB");
                                                 if (e8) acq_gain_win = atof(e8); } }
                    /* SIPFAX: ACQUIRE gain and carrier phase before decoding anything.
                       Two static errors otherwise wreck the whole of data mode, and both
                       were measured against encoder ground truth on a noiseless signal:
                         - gain 2.244x too large, because the open-loop scale above uses
                           the UNIFORM mean over the constellation (|c|^2 = 122.3) while
                           the shell mapper is a shaping code whose transmitted mean is
                           24.3 - sqrt of the ratio is 2.243;
                         - a constant carrier rotation (measured +106.8 deg, stable to
                           0.1 deg rms with zero drift), of which the 90 deg part is
                           transparent to V.34's differential Z but the remainder is not.
                       Uncorrected the symbols score 0.583 against the lattice (= the
                       uniform floor, i.e. no information); corrected they score 0.132.
                       Search gain first, then phase over 0..90 deg at that gain - a joint
                       coarse search finds false minima. Both are static here, so this is
                       one-shot; the DD loop then only has to track drift. */
                    static int acqn = -1;
                    if (acqn < 0) { char *e = getenv("SIPFAX_ACQ_N");
                                    /* SIPFAX: 400, not 2000. The fit solves for ONE gain and
                                       ONE phase, but the caller's carrier walks: the per-window
                                       diagnostic below shows the best-fit phase moving between
                                       200-symbol windows (62, 23, 21, 19, 54 deg ...), so a long
                                       window asks the fit an impossible question. Measured on a
                                       live capture, lattice-rms against window length:
                                       2000 -> 0.309, 800 -> 0.170, 400 -> 0.118, 256 -> 0.104,
                                       where under 0.2 is a lock. 400 takes the lock with more
                                       samples behind the gain estimate than 256. */
                                    acqn = e ? atoi(e) : DATA_ACQ_N;
                                    if (acqn < 64) acqn = 64;
                                    if (acqn > DATA_ACQ_N) acqn = DATA_ACQ_N; }
                    if (s->data_acq_n < acqn) {
                        double dth0 = data_carrier(s);
                        double ct0 = cos(-dth0), st0 = sin(-dth0);
                        /* SIPFAX: the acquisition window starts the instant E is detected,
                           which is not necessarily where the caller's DATA starts. The
                           measured constellation there is four points 90 deg apart at
                           radius sqrt(10) - a 4-point set, not a smeared 12-point one -
                           and 4-point is exactly what Phase 4 transmits. SIPFAX_ACQ_DELAY
                           moves the window further into data mode so the two can be told
                           apart. */
                        /* SIPFAX: default 6000 symbols (1.75 s), not 0. Data does NOT start
                           at E - the caller sends another 3000-4000 symbols of 4-point
                           Phase-4 material first, measured by the live 4-point EVM staying
                           at 4.9-5.2% for two blocks after p4_e_rx goes true. With the old
                           default of 0 every LIVE call acquired its gain and carrier phase
                           on that tail: the first call with the tx_amp fix reported
                           rho = 0.78 (strong 4-fold structure, i.e. a 4-point set) and
                           8 of 12 points with the collapse flag, so its data-mode
                           acquisition was fitted to the wrong signal entirely. */
                        static long acqdly = -1, acqseen = 0;
                        if (acqdly < 0) { char *ea = getenv("SIPFAX_ACQ_DELAY");
                                          /* SIPFAX: was 6000. The kurtosis prefix test below
                                             discriminates training from data directly, which
                                             is what the fixed delay was approximating - and
                                             1.75 s is a third of the caller's whole data
                                             window, so guessing costs more than it buys. */
                                          acqdly = ea ? atol(ea) : 0; }
                        if (acqseen < acqdly) { acqseen++; }
                        else {
                        s->data_acq_i[s->data_acq_n] = (oi*ct0 - oq*st0) * gc;
                        s->data_acq_q[s->data_acq_n] = (oi*st0 + oq*ct0) * gc;
                        s->data_acq_n++;
                        {   /* SIPFAX: REJECT A BAD WINDOW EARLY, AND SLIDE. The fixed
                               SIPFAX_ACQ_DELAY above is a guess at where the caller's data
                               starts; when it guesses wrong the whole 2000-symbol window is
                               training and has to be thrown away, and each retry costs
                               another 2000 symbols. Measured on a live call that is fatal:
                               the caller's shaped data lasts about 5 s, of which the fixed
                               delay eats 1.75 s, so acquisition kept landing either on the
                               4-point Phase-4 tail before the data or on the Tone B after
                               it - it accepted a window of kurtosis 1.001 while the data
                               either side of it measured 1.39-1.59. Testing a short prefix
                               instead rejects a wrong window in 256 symbols (75 ms) rather
                               than 2000, so the collector slides forward until it is
                               genuinely on shaped data. */
                            static int npre = -1; static double kpre = -1;
                            if (npre < 0) { char *e = getenv("SIPFAX_ACQ_PRE");
                                            npre = e ? atoi(e) : 256; }
                            if (kpre < 0) { char *e = getenv("SIPFAX_ACQ_KURT");
                                            kpre = e ? atof(e) : 1.25; }
                            if (npre > 0 && s->data_acq_n == npre) {
                                double a2 = 0, a4 = 0; int jj;
                                for (jj = 0; jj < npre; jj++) {
                                    double p = s->data_acq_i[jj]*s->data_acq_i[jj]
                                             + s->data_acq_q[jj]*s->data_acq_q[jj];
                                    a2 += p; a4 += p*p;
                                }
                                a2 /= npre; a4 /= npre;
                                if (a2 > 0 && a4/(a2*a2) < kpre) s->data_acq_n = 0;
                            }
                        }
                        }
                    } else
                    {
                        double bg = 1.0, be = 1e30, bp = 0.0, gdb, th2;
                        double pw0 = 0, pwmin; int qi;
                        {   /* SIPFAX: MEASURE (and optionally remove) A DC OFFSET on the
                               acquisition window. A coherent component added to the data -
                               carrier leakage through the mixer, or any residual at the
                               carrier - shows up in baseband as a constant vector, and it
                               explains both symptoms at once: E[z^4] picks up a large DC^4
                               term (our rho is 0.81 where the caller's raw signal measures
                               0.061), and every point is displaced toward the same corner,
                               so the slicer lands them on far fewer lattice cells than exist
                               (8-16 of 56 used). SIPFAX_ACQ_DC=0 measures without removing. */
                            static int dcrm = -1;
                            double mi = 0, mq = 0; int jd;
                            if (dcrm < 0) { char *e = getenv("SIPFAX_ACQ_DC");
                                            dcrm = e ? atoi(e) : 1; }
                            for (jd = 0; jd < s->data_acq_n; jd++) {
                                mi += s->data_acq_i[jd]; mq += s->data_acq_q[jd];
                            }
                            if (s->data_acq_n > 0) { mi /= s->data_acq_n; mq /= s->data_acq_n; }
                            {   double p = 0;
                                for (jd = 0; jd < s->data_acq_n; jd++)
                                    p += s->data_acq_i[jd]*s->data_acq_i[jd]
                                       + s->data_acq_q[jd]*s->data_acq_q[jd];
                                if (s->data_acq_n > 0) p /= s->data_acq_n;
                                { extern int v34_dbg; if (v34_dbg)
                                    fprintf(stderr, "[data] acq DC = (%+.4f,%+.4f)  |DC|^2/P = "
                                            "%.4f  %s\n", mi, mq,
                                            p > 0 ? (mi*mi+mq*mq)/p : 0.0,
                                            dcrm ? "(removing)" : "(measuring only)"); }
                            }
                            if (dcrm) {
                                for (jd = 0; jd < s->data_acq_n; jd++) {
                                    s->data_acq_i[jd] -= mi; s->data_acq_q[jd] -= mq;
                                }
                            }
                        }
                        for (qi = 0; qi < s->data_acq_n; qi++)
                            pw0 += s->data_acq_i[qi]*s->data_acq_i[qi]
                                 + s->data_acq_q[qi]*s->data_acq_q[qi];
                        pw0 /= s->data_acq_n;
                        /* SIPFAX: the lattice score alone is minimised by SHRINKING - squash
                           every symbol onto the innermost ring (+-1,+-1) and the distance to
                           the nearest lattice point goes to zero while all information is
                           destroyed. That is exactly what happened: the search picked a gain
                           that put 100% of symbols on just 4 points of a 12-point
                           constellation. Constrain the gain so the scaled mean power stays
                           at least a quarter of the constellation's uniform mean - shaping
                           lowers it (measured 24.3 against a uniform 122.3 at R=16800, i.e.
                           0.2x) but never collapses it to a single ring. */
                        pwmin = 0.25 * s->data_meanc2;
                        /* SIPFAX: the gain is now MEASURED, not searched - see
                           v34_shaped_meanc2(). This search is degenerate: it is minimised
                           by collapsing the constellation onto its innermost ring, and no
                           power floor separates the two cases (0.25x the uniform mean sat
                           above the true shaped mean and excluded the right answer; the
                           corrected value sits below |c|^2 = 2 and admits the collapse).
                           SIPFAX_ACQ_GAIN=1 restores it for comparison. */
                        /* SIPFAX: search the gain again, but in a NARROW window around the
                           measured shaped mean instead of the old -14..+6 dB range.
                           v34_shaped_meanc2() fixes the scale from our own mapper, which is
                           right in principle, but on a live signal the channel gain and the
                           AGC leave it a little off - measured on the first call with the
                           tx_amp fix, an exhaustive 2-D sweep found lattice-rms 0.124 at
                           gain x0.792 while the acquisition, with the search disabled,
                           settled for 0.359. That is the difference between a lock and no
                           lock, thrown away by removing the search entirely.
                           +-3 dB is wide enough for that -2.0 dB correction and far too
                           narrow for the degenerate collapse, which needed -4.5 dB to squash
                           the constellation onto its inner ring; the anti-collapse power
                           floor still guards underneath. SIPFAX_ACQ_GAIN=0 disables. */
                        for (gdb = -acq_gain_win; acq_search_gain && gdb <= acq_gain_win + 1e-9;
                             gdb += 0.05) {
                            double g2 = pow(10.0, gdb/20.0), em = 1e30, t2;
                            if (pw0*g2*g2 < pwmin) continue;
                            for (t2 = 0; t2 < 90.0; t2 += 3.0) {
                                double e2 = data_lattice_rms(s->data_acq_i, s->data_acq_q,
                                                             s->data_acq_n, g2,
                                                             cos(-t2*M_PI/180.0), sin(-t2*M_PI/180.0));
                                if (e2 < em) em = e2;
                            }
                            if (em < be) { be = em; bg = g2; }
                        }
                        be = 1e30;
                        for (th2 = 0; th2 < 90.0; th2 += 0.25) {
                            double e2 = data_lattice_rms(s->data_acq_i, s->data_acq_q,
                                                         s->data_acq_n, bg,
                                                         cos(-th2*M_PI/180.0), sin(-th2*M_PI/180.0));
                            if (e2 < be) { be = e2; bp = th2; }
                        }
                        {   /* SIPFAX: DO NOT ACQUIRE ON A SIGNAL THAT IS NOT THE CALLER'S
                               DATA. The acquisition window opens the moment WE enter data
                               mode, but the caller is still finishing Phase 4 - so on a live
                               call we were fitting a gain and a carrier phase to its 4/16-point
                               TRAINING constellation and then decoding its data with them.
                               The evidence was in our own diagnostics: amplitude kurtosis
                               1.001, where 1.00 is a CONSTANT ENVELOPE and a shaped L=56 data
                               constellation measures ~1.5-1.8 (this caller's data has been
                               measured at 1.766); and the point-usage histogram used only 14
                               of 56 points, the first 12 never touched. The result was
                               lattice-rms 0.512 against a 0.577 no-lock floor - and an
                               exhaustive 2-D gain/phase sweep reached only 0.490, i.e. NO
                               setting worked, because the fault was the WINDOW, not the fit.
                               Shaped data has a non-constant envelope; training does not. */
                            static int retries = -1, kdbg = 0;
                            static double kmin = -1;
                            double k2 = 0, k4 = 0, kurt; int jk;
                            if (retries < 0) { char *e = getenv("SIPFAX_ACQ_RETRY");
                                               retries = e ? atoi(e) : 40; }
                            if (kmin < 0)    { char *e = getenv("SIPFAX_ACQ_KURT");
                                               kmin = e ? atof(e) : 1.25; }
                            for (jk = 0; jk < s->data_acq_n; jk++) {
                                double p = s->data_acq_i[jk]*s->data_acq_i[jk]
                                         + s->data_acq_q[jk]*s->data_acq_q[jk];
                                k2 += p; k4 += p*p;
                            }
                            if (s->data_acq_n > 0) { k2 /= s->data_acq_n; k4 /= s->data_acq_n; }
                            kurt = (k2 > 0) ? k4/(k2*k2) : 0.0;
                            if (kurt < kmin && retries > 0) {
                                retries--;
                                s->data_acq_n = 0;      /* discard the window, listen again */
                                { extern int v34_dbg; if (v34_dbg && (kdbg++ % 8) == 0)
                                    fprintf(stderr, "[data] acq kurtosis %.3f < %.2f (training,"
                                            " not data) - rewinding, %d left\n",
                                            kurt, kmin, retries); }
                            } else {
                                { extern int v34_dbg; if (v34_dbg)
                                    fprintf(stderr, "[data] acq kurtosis %.3f -> accepting\n",
                                            kurt); }
                                s->data_agc *= bg;
                                s->data_th  += bp*M_PI/180.0;
                                s->data_acq_done = 1;
                            }
                        }
                        {   /* SIPFAX: a low lattice score is NOT on its own evidence of a
                               decode - the acquisition minimises distance to the lattice,
                               and squashing every symbol onto the innermost ring minimises
                               it trivially. Print the point distribution so the claim is
                               checkable rather than assumed.
                               Units: data_acq_* are already in LATTICE units (gc applied on
                               the way in, and data_lattice_rms snaps to odd integers), so
                               they go into data_slice as-is. An earlier version of this
                               check divided by 128 and re-applied data_agc, which drove
                               every symbol to the origin and reported a collapse that was
                               purely its own doing. Score exactly what the acquisition
                               scored: the buffer scaled by bg and rotated by bp. */
                            int hc[64], hi2, nq2 = s->L/4, used = 0;
                            double cth = cos(-bp*M_PI/180.0), sth = sin(-bp*M_PI/180.0);
                            for (hi2 = 0; hi2 < 64; hi2++) hc[hi2] = 0;
                            for (hi2 = 0; hi2 < s->data_acq_n; hi2++) {
                                double bi2 = s->data_acq_i[hi2], bq2 = s->data_acq_q[hi2];
                                double xi2 = (bi2*cth - bq2*sth) * bg;
                                double xq2 = (bi2*sth + bq2*cth) * bg;
                                double d1, d2; int b2, qd2, fi;
                                b2 = data_slice(s, xi2, xq2, &d1, &d2);
                                qd2 = (d1 >= 0) ? (d2 >= 0 ? 0 : 3) : (d2 >= 0 ? 1 : 2);
                                fi = b2*4 + qd2;
                                if (fi >= 0 && fi < 64) hc[fi]++;
                            }
                        {   /* SIPFAX: is the constellation SPINNING? The odd-integer set
                               has 90-degree symmetry, so the mean of y^4 has a stable angle
                               for a phase-locked signal and a walking one for a signal with
                               residual carrier offset. Report it per block: a linear walk
                               IS a frequency error, and its slope gives the offset in Hz.
                               Needed because data mode derotates by a STATIC data_th seeded
                               once at entry - nothing tracks carrier after that. */
                            int bn, nb3 = s->data_acq_n/200, dn = 0;
                            double prev = 0, dsum = 0; int havep = 0;
                            static int seed_frq = -1;
                            if (seed_frq < 0) { char *e9 = getenv("SIPFAX_SEED_FRQ");
                                                seed_frq = e9 ? atoi(e9) : 1; }
                            double cth4 = cos(-bp*M_PI/180.0), sth4 = sin(-bp*M_PI/180.0);
                            fprintf(stderr, "[data] 4th-power angle per 200 syms:");
                            for (bn = 0; bn < nb3 && bn < 12; bn++) {
                                double a4 = 0, b4 = 0; int j4;
                                for (j4 = bn*200; j4 < (bn+1)*200; j4++) {
                                    double bi3 = s->data_acq_i[j4], bq3 = s->data_acq_q[j4];
                                    double xr = (bi3*cth4 - bq3*sth4) * bg;
                                    double xq3 = (bi3*sth4 + bq3*cth4) * bg;
                                    double r2 = xr*xr - xq3*xq3, i2 = 2*xr*xq3;
                                    a4 += r2*r2 - i2*i2; b4 += 2*r2*i2;
                                }
                                {   double ang = atan2(b4, a4)/4.0*180.0/M_PI, d;
                                    if (havep) { d = ang - prev;
                                                 while (d >  45.0) d -= 90.0;
                                                 while (d < -45.0) d += 90.0;
                                                 fprintf(stderr, " %+.1f(d%+.1f)", ang, d); }
                                    else fprintf(stderr, " %+.1f", ang);
                                    if (havep) { dsum += d; dn++; }
                                    prev = ang; havep = 1; }
                            }
                            fprintf(stderr, "\n");
                            if (dn >= 4 && seed_frq) {
                                /* SIPFAX: that walk IS a residual carrier offset - the far
                                   end's clock is off, so its carrier is off by the same
                                   ratio, and data mode derotates by a STATIC angle seeded
                                   once at entry. Measured on the caller: -1.95 deg per 200
                                   symbols, dead linear, = -0.093 Hz = 47 ppm of the 1959 Hz
                                   carrier - the same offset the Gardner loop sees in the
                                   symbol clock, as physics requires. Over the 20000-symbol
                                   scoring window that is 195 degrees, which is why no
                                   carrier-loop gain made any difference: there was nothing
                                   for a phase loop to hold on to. Seed the rate directly
                                   from the measurement; data_th advances by data_frq every
                                   symbol even with the DD gains at zero. */
                                double dps = (dsum/dn) / 200.0 * M_PI / 180.0;
                                s->data_frq = dps;
                                {   /* SIPFAX: DE-DRIFT THE WINDOW BEFORE FITTING IT. The
                                       gain/phase fit below solves for ONE constant rotation
                                       over the whole 2000-symbol window, but the measurement
                                       just above says the carrier walks -1.9 deg per 200
                                       symbols - about 19 degrees across that window. No
                                       single phase fits a constellation that is turning, so
                                       the fit was being asked an impossible question and the
                                       residual showed up as a smear: lattice-rms stuck at
                                       0.31 with the constellation collapsing onto 16 of 56
                                       points. The rate is already estimated here; apply it to
                                       the stored samples so the fit sees a still picture.
                                       SIPFAX_ACQ_DEDRIFT=0 restores the old behaviour. */
                                    static int dd = -1;
                                    if (dd < 0) { char *e = getenv("SIPFAX_ACQ_DEDRIFT");
                                                  /* measured: no change to lattice-rms and rho
                                                     got worse, so OFF until it earns its place */
                                                  dd = e ? atoi(e) : 0; }
                                    if (dd) {
                                        int jd; double mid = 0.5*(s->data_acq_n - 1);
                                        for (jd = 0; jd < s->data_acq_n; jd++) {
                                            double ph = -dps * (jd - mid);   /* centre the
                                                           correction so the mean phase, which
                                                           the sweep still solves for, is not
                                                           moved by this */
                                            double c = cos(ph), sn = sin(ph);
                                            double vi = s->data_acq_i[jd], vq = s->data_acq_q[jd];
                                            s->data_acq_i[jd] = vi*c - vq*sn;
                                            s->data_acq_q[jd] = vi*sn + vq*c;
                                        }
                                        fprintf(stderr, "[data] de-drifted acquisition window "
                                                "by %+.2f deg end-to-end\n",
                                                -dps*(s->data_acq_n-1)*180.0/M_PI);
                                    }
                                }
                                fprintf(stderr, "[data] carrier offset %+.4f Hz (%+.1f ppm)"
                                        " -> seeding data_frq %+.3e rad/sym\n",
                                        dps*3428.571/(2*M_PI), dps*3428.571/(2*M_PI)/1959.184*1e6,
                                        dps);
                            }
                        }
                        {   /* SIPFAX: does the cloud have AMPLITUDE structure at all?
                               The inner/outer decision is purely radial - the union of the
                               four inner Voronoi cells is exactly the square [-2,2]^2, so
                               r=1.41 is inner at EVERY phase and r=3.16 outer at every
                               phase, and no rotation can move a symbol across. A resolved
                               shaped L=12 set must therefore put 54.75% of symbols on the
                               inner ring; we see 9.75%. Either the cloud is constant-
                               envelope (kurtosis ~1.0, one radius bin) or it is a diffuse
                               blob (~2.0); a resolved set gives ~1.50. That distinguishes
                               a phase problem from an amplitude one, which no score does. */
                            double s2 = 0, s4 = 0, r4i = 0, r4q = 0, m4 = 0;
                            int rb[5]; int hj, rk;
                            double cthk = cos(-bp*M_PI/180.0), sthk = sin(-bp*M_PI/180.0);
                            for (hj = 0; hj < 5; hj++) rb[hj] = 0;
                            for (hj = 0; hj < s->data_acq_n; hj++) {
                                double bi4 = s->data_acq_i[hj], bq4 = s->data_acq_q[hj];
                                double xr = (bi4*cthk - bq4*sthk) * bg;
                                double xq4 = (bi4*sthk + bq4*cthk) * bg;
                                double r2 = xr*xr + xq4*xq4, r = sqrt(r2);
                                double a2 = xr*xr - xq4*xq4, b2 = 2*xr*xq4;
                                s2 += r2; s4 += r2*r2; m4 += r2*r2;
                                r4i += a2*a2 - b2*b2; r4q += 2*a2*b2;
                                rk = (r < 1.8) ? 0 : (r < 2.4) ? 1 : (r < 2.9) ? 2
                                                 : (r < 3.6) ? 3 : 4;
                                rb[rk]++;
                            }
                            s2 /= s->data_acq_n; s4 /= s->data_acq_n; m4 /= s->data_acq_n;
                        {   /* SIPFAX: the acquisition fits ONE static phase across 2000
                               symbols. If the constellation is rotating, no single angle
                               can fit and the score sits at the no-lock floor even though
                               the amplitude structure is perfect - which is exactly what
                               we see (kurtosis 1.59, inner ring 53.3% vs 54.75% predicted,
                               mean power on target, yet lattice-rms 0.573).
                               Fit the best phase INDEPENDENTLY per sub-window: if short
                               windows score well and their best phase walks linearly, the
                               residual carrier is measured - and unlike the 4th-power
                               estimator this works on a shaped set, where rho is only
                               0.026 and that estimator has almost no signal. */
                            int wl[4], wi3;
                            wl[0]=100; wl[1]=200; wl[2]=500; wl[3]=2000;
                            for (wi3 = 0; wi3 < 4; wi3++) {
                                int W = wl[wi3], nw = s->data_acq_n / W, bk;
                                double tot = 0; int cnt = 0;
                                fprintf(stderr, "[data] window %4d:", W);
                                for (bk = 0; bk < nw && bk < 10; bk++) {
                                    double be3 = 1e30, bp3 = 0, t3;
                                    for (t3 = 0; t3 < 90.0; t3 += 0.25) {
                                        double e3 = data_lattice_rms(
                                            s->data_acq_i + bk*W, s->data_acq_q + bk*W, W,
                                            bg, cos(-t3*M_PI/180.0), sin(-t3*M_PI/180.0));
                                        if (e3 < be3) { be3 = e3; bp3 = t3; }
                                    }
                                    if (nw <= 10) fprintf(stderr, " %.3f@%.1f", be3, bp3);
                                    tot += be3; cnt++;
                                }
                                fprintf(stderr, "%s  mean %.3f\n",
                                        nw > 10 ? " (first 10)" : "", cnt ? tot/cnt : 0.0);
                            }
                        }
                        {   /* SIPFAX: test Tomlinson-Harashima inversion AT THE SYMBOL
                               LEVEL. The inverse in decode_mapping_frame is downstream of
                               this buffer, so it can never move lattice-rms - a test with
                               it enabled returned bit-identical numbers, which proved
                               nothing. THP transmits x(n) = d(n) - SUM h_k x(n-k) (mod),
                               so the receiver recovers d(n) = y(n) + SUM h_k y(n-k) (mod).
                               That is a plain FIR over received symbols and can be applied
                               right here. Both signs, since the convention is what is in
                               doubt. peer_h is only trustworthy from a CRC-clean MP. */
                            double hr[3], hi4[3]; int hk, sgn;
                            for (hk = 0; hk < 3; hk++) {
                                hr[hk]  = s->peer_h[hk*2]   / 16384.0;
                                hi4[hk] = s->peer_h[hk*2+1] / 16384.0;
                            }
                            if (hr[0] || hi4[0] || hr[1] || hi4[1]) {
                                for (sgn = -1; sgn <= 1; sgn += 2) {
                                    static double ti[2048], tq[2048];
                                    double be4 = 1e30, bp4 = 0, t4;
                                    int j4, n4 = s->data_acq_n > 2048 ? 2048 : s->data_acq_n;
                                    for (j4 = 0; j4 < n4; j4++) {
                                        double ai = s->data_acq_i[j4], aq = s->data_acq_q[j4];
                                        for (hk = 0; hk < 3; hk++) {
                                            int idx = j4 - 1 - hk;
                                            if (idx < 0) break;
                                            ai += sgn*(hr[hk]*s->data_acq_i[idx]
                                                       - hi4[hk]*s->data_acq_q[idx]);
                                            aq += sgn*(hr[hk]*s->data_acq_q[idx]
                                                       + hi4[hk]*s->data_acq_i[idx]);
                                        }
                                        ti[j4] = ai; tq[j4] = aq;
                                    }
                                    for (t4 = 0; t4 < 90.0; t4 += 0.25) {
                                        double e4 = data_lattice_rms(ti, tq, n4, bg,
                                                        cos(-t4*M_PI/180.0), sin(-t4*M_PI/180.0));
                                        if (e4 < be4) { be4 = e4; bp4 = t4; }
                                    }
                                    fprintf(stderr, "[data] THP inverse sign %+d: "
                                            "lattice-rms %.3f @ %.2f deg\n", sgn, be4, bp4);
                                }
                            } else {
                                fprintf(stderr, "[data] THP inverse: peer_h is zero"
                                        " (no CRC-clean MP) - not tested\n");
                            }
                        }
                        {   /* SIPFAX: the peer's CRC-clean MP sets nonlin=1 (9.7). The
                               non-linear encoder warps each point RADIALLY by
                               theta = 1 + zeta/6 + zeta^2/120 with zeta from |x|^2, which
                               moves symbols off the odd-integer lattice while leaving the
                               amplitude distribution smoothly intact - the exact signature
                               measured here (kurtosis 1.594, no angular structure at any
                               rotation, rate, shaping or THP inversion). linmodem's own
                               encoder stubs the warp out (dzeta hardcoded 0.3125), so the
                               normalisation is not trustworthy from this source. Sweep the
                               warp parameter instead of guessing it: if some value
                               collapses the score, warping is confirmed AND measured; if
                               nothing does, it is refuted. a=0 is the unwarped baseline. */
                            double aa, bestA = 0, bestE = 1e30, bestP = 0, mr2 = 0;
                            int j6, n6 = s->data_acq_n > 2048 ? 2048 : s->data_acq_n;
                            static double wi6[2048], wq6[2048];
                            for (j6 = 0; j6 < n6; j6++)
                                mr2 += (s->data_acq_i[j6]*s->data_acq_i[j6]
                                      + s->data_acq_q[j6]*s->data_acq_q[j6]);
                            mr2 = mr2 / (n6 > 0 ? n6 : 1);
                            fprintf(stderr, "[data] non-linear warp sweep (peer nonlin=%d):\n",
                                    s->peer_nonlin);
                            for (aa = -0.60; aa <= 0.601; aa += 0.05) {
                                double e6 = 1e30, t6;
                                for (j6 = 0; j6 < n6; j6++) {
                                    double xi6 = s->data_acq_i[j6], xq6 = s->data_acq_q[j6];
                                    double z = (mr2 > 0) ? (xi6*xi6 + xq6*xq6)/mr2 : 0.0;
                                    double th = 1.0 + aa*z/6.0 + (aa*z)*(aa*z)/120.0;
                                    if (th < 0.05) th = 0.05;
                                    wi6[j6] = xi6/th; wq6[j6] = xq6/th;
                                }
                                for (t6 = 0; t6 < 90.0; t6 += 0.5) {
                                    double v = data_lattice_rms(wi6, wq6, n6, bg,
                                                   cos(-t6*M_PI/180.0), sin(-t6*M_PI/180.0));
                                    if (v < e6) { e6 = v; if (v < bestE) { bestE = v; bestA = aa; bestP = t6; } }
                                }
                                if (fabs(aa) < 1e-9 || fabs(aa+0.30) < 1e-9 || fabs(aa-0.30) < 1e-9
                                    || fabs(aa+0.60) < 1e-9 || fabs(aa-0.60) < 1e-9)
                                    fprintf(stderr, "[data]   a=%+.2f -> %.3f\n", aa, e6);
                            }
                            fprintf(stderr, "[data]   BEST a=%+.2f -> lattice-rms %.3f @ %.1f deg\n",
                                    bestA, bestE, bestP);
                        }
                            fprintf(stderr, "[data] amplitude kurtosis %.3f "
                                    "(1.00=constant envelope, 1.50=resolved L=12, 2.00=blob)\n",
                                    s2 > 0 ? s4/(s2*s2) : 0.0);
                            fprintf(stderr, "[data] radius bins <1.8:%d 1.8-2.4:%d 2.4-2.9:%d"
                                    " 2.9-3.6:%d >3.6:%d   (rings at 1.41 / 3.16)\n",
                                    rb[0], rb[1], rb[2], rb[3], rb[4]);
                            fprintf(stderr, "[data] rho = |E[x^4]|/E|x|^4 = %.4f "
                                    "(0=no 4-fold phase structure)\n",
                                    m4 > 0 ? sqrt(r4i*r4i + r4q*r4q)
                                             /(double)s->data_acq_n/m4 : 0.0);
                        }
                            fprintf(stderr, "[data] point usage over %d syms (L=%d):",
                                    s->data_acq_n, s->L);
                            for (hi2 = 0; hi2 < nq2*4 && hi2 < 64; hi2++) {
                                fprintf(stderr, " %d", hc[hi2]);
                                if (hc[hi2] > 0) used++;
                            }
                            fprintf(stderr, "\n[data] %d of %d points used, mean|c|^2 %.2f"
                                    " (shaped target %.2f)%s\n", used, s->L,
                                    pw0*bg*bg, s->data_meanc2,
                                    used*4 < s->L*3 ? "  <-- COLLAPSED, score is an artifact" : "");
                        }
                        gc = s->data_agc;
                        {   /* SIPFAX: is the acquisition FINDING the optimum, or is there
                               no optimum to find? The only transforms between the equaliser
                               output and the lattice score are a rotation and a scale, and
                               the acquisition fixes the gain by measurement and searches the
                               phase over 0-90 deg. Sweep BOTH exhaustively and report the
                               global minimum: if it is far below what the acquisition picks,
                               the search is at fault; if it equals it, then no rotation or
                               scale can make these symbols fit the lattice and the fault is
                               upstream. Self-contained - needs no alignment to ground truth. */
                            double gg, tt, gbest = 0, tbest = 0, ebest = 1e30;
                            for (gg = 0.30; gg <= 3.001; gg *= 1.02) {
                                for (tt = 0; tt < 90.0; tt += 0.5) {
                                    double e5 = data_lattice_rms(s->data_acq_i, s->data_acq_q,
                                                    s->data_acq_n, bg*gg,
                                                    cos(-tt*M_PI/180.0), sin(-tt*M_PI/180.0));
                                    if (e5 < ebest) { ebest = e5; gbest = gg; tbest = tt; }
                                }
                            }
                            { extern int v34_dbg; if (v34_dbg)
                                fprintf(stderr, "[data] 2-D (gain,phase) sweep: best %.3f at "
                                        "gain x%.3f phase %+.2f deg   [acquisition chose %.3f]\n",
                                        ebest, bg*gbest, tbest, be); }
                        }
                        { extern int v34_dbg; if (v34_dbg)
                            fprintf(stderr, "[data] acquired: gain x%.3f, phase %+.2f deg,"
                                    " lattice-rms %.3f (0.577 = no lock, <0.2 = good)\n",
                                    bg, bp, be); }
                    }
                }
                if (s->data_acq_done) {
                { double dth2 = data_carrier(s); ct2 = cos(-dth2); st2 = sin(-dth2); }
                xi = (oi*ct2 - oq*st2) * gc;
                xq = (oi*st2 + oq*ct2) * gc;
                data_slice(s, xi, xq, &di, &dq);
                {   /* SIPFAX: carry a decision-directed tap error back to the equaliser.
                       Rotate the decision out of the derotated/scaled frame into the raw
                       equaliser output frame, so the LMS gradient is in the same units as
                       cma_bufi/cma_bufq. Only used when SIPFAX_DATA_DDMU > 0. */
                    double dth3 = data_carrier(s);
                    double c3 = cos(dth3), s3 = sin(dth3);
                    double dri = (di*c3 - dq*s3) / (gc != 0 ? gc : 1.0);
                    double drq = (di*s3 + dq*c3) / (gc != 0 ? gc : 1.0);
                    s->data_ei = dri - oi; s->data_eq = drq - oq; s->data_ev = 1;
                }
                nrm2 = di*di + dq*dq;
                if (nrm2 > 0) {
                    pe = atan2(xq*di - xi*dq, xi*di + xq*dq);
                    if (pe >  0.8) pe =  0.8;                   /* clamp: a wrong decision
                                                                   must not kick the loop */
                    if (pe < -0.8) pe = -0.8;
                    s->data_frq += ki*pe;
                    s->data_th  += s->data_frq + kp*pe;
                }
                {   static double mu_agc = -1;
                    if (mu_agc < 0) { char *e7 = getenv("SIPFAX_DATA_AGC");
                                      mu_agc = e7 ? atof(e7) : 0.0;   /* OFF: needs correct phase first */ }
                    if (mu_agc > 0 && nrm2 > 0) {
                        double ay = sqrt(xi*xi + xq*xq), ad = sqrt(nrm2);
                        if (ay > 1e-6) s->data_agc *= (1.0 + mu_agc*(ad/ay - 1.0));
                    }
                }
                s->data_n++;
                {   /* SIPFAX: the decoder pairs consecutive symbols into 4D symbols and
                       groups 8 into a mapping frame, but nothing aligns that grouping at
                       data-mode entry - we start feeding at whatever symbol we happen to
                       be on. SIPFAX_DATA_SKIP drops N symbols first so the alignment can
                       be swept. */
                    static int skip = -1;
                    if (skip < 0) { char *e4 = getenv("SIPFAX_DATA_SKIP"); skip = e4 ? atoi(e4) : 0; }
                    {   /* SIPFAX: the 4D PAIRING shift must happen HERE, at the 2D symbol
                           feed - the mapping-frame site downstream moves whole 4D symbols
                           (4 s16 = two 2D symbols) and so can only change phi, never the
                           pairing. v0 locks for every odd DATA_SKIP and no even one, so a
                           failure to lock means we are on the wrong 2D parity: drop one. */
                        extern int g_v0_pairdrop;
                        if (g_v0_pairdrop) { g_v0_pairdrop = 0; skip++; }
                    }
                    if (skip > 0) { skip--; }
                    else {
                        int si2 = (int)lrint(xi*128.0), sq2 = (int)lrint(xq*128.0);
                        {   /* SIPFAX: the decided symbols are EXACTLY the transmitted ones
                               (29456/29456 against ground truth) but rotated a constant
                               180 degrees. The acquisition searches phase over 0-90 only,
                               relying on the differential Z to absorb whole-quadrant
                               offsets - which it should, since a constant offset cancels in
                               (Z[0]-Z_1). SIPFAX_ROT180 applies the correction at the feed
                               so the two can be separated: if the bits come good, the
                               differential path is not absorbing the offset as intended. */
                            static int r180 = -1;
                            if (r180 < 0) { char *e = getenv("SIPFAX_ROT180");
                                            r180 = e ? atoi(e) : 0; }
                            if (r180) { si2 = -si2; sq2 = -sq2; }
                        }
                        {   /* SIPFAX: dump what the receiver actually hands the decoder,
                               so it can be diffed against the encoder ground truth. */
                            static FILE *df = 0; static int op = 0;
                            if (!op) { char *e5 = getenv("SIPFAX_DATASYM"); op = 1;
                                       if (e5) df = fopen(e5, "w"); }
                            if (df) fprintf(df, "%d %d\n", si2, sq2);
                        }
                        baseband_decode_impl(s, si2, sq2);
                    }
                }
                }
                { extern int v34_dbg; if (v34_dbg && (s->data_n % 20000) == 0)
                    fprintf(stderr, "[data] clock %+.1f ppm\n", s->cma_tinc/(7.0/6.0)*1e6),
                    fprintf(stderr, "[data] %ld syms, metric %.1f, freq %.2e rad/sym\n",
                            s->data_n, s->data_mse_n ? s->data_mse_acc/s->data_mse_n : 0.0,
                            s->data_frq); }
                {   /* SIPFAX: IS THE TRELLIS ACTUALLY TRACKING? These counters were already
                       accumulated but only ever printed by the offline test path, so the live
                       path - the one that fails - never showed them. If nearly all 64 states
                       tie at the minimum, the branch metrics carry no information and the
                       fault is upstream of the trellis, in the constellation labelling / 4D
                       subset mapping. A healthy decoder has a small number of tied states and
                       a wide spread. */
                    extern int v34_dbg; extern long g_ss_n, g_ss_tied; extern double g_ss_spread;
                    if (v34_dbg && g_ss_n && (s->data_n % 20000) == 0)
                        fprintf(stderr, "[acs] state metrics: mean spread %.1f over 64 states; "
                                "mean %.1f states share the minimum  (n=%ld)\n",
                                g_ss_spread/g_ss_n, (double)g_ss_tied/g_ss_n, g_ss_n); }
            }
        }
        for (r = 0; r < 4; r++) {
            int z = (r - qd) & 3, nb = 2; b2s[r][0] = z & 1; b2s[r][1] = (z >> 1) & 1;   /* spec CW */
            if (srx_rx16() && s->p4_mode != 0) {
                /* 16-point TRN (10.1.3.6): In = z is ABSOLUTE, and Q1,Q2 carry the
                   base-point index. Four bits per symbol instead of two. */
                int q16, z16;
                srx_slice16(pi_, pq_, s->rx16_rms, &q16, &z16, NULL, NULL);
                z = (r - z16) & 3;
                b2s[r][0] = z & 1; b2s[r][1] = (z >> 1) & 1;
                b2s[r][2] = q16 & 1; b2s[r][3] = (q16 >> 1) & 1;
                nb = 4;
            }
            regsnap[r] = s->srx_reg4[r];
            for (k = 0; k < nb; k++) {
                int pred = ((s->srx_reg4[r] >> 22) & 1) ^ 1;
                s->srx_hist4[r] = (s->srx_hist4[r] << 1) | (unsigned long long)(pred == b2s[r][k]);
                s->srx_reg4[r] = (s->srx_reg4[r] << 1) & 0x7fffff;
                if (b2s[r][k]) s->srx_reg4[r] ^= poly;
            }
        }
        w = 0; wm = -1;
        for (r = 0; r < 4; r++) { int m2 = __builtin_popcountll(s->srx_hist4[r]); if (m2 > wm) { wm = m2; w = r; } }
        if (s->srx_locked && wm >= 55 && (s->p4_mode != 2)) {
            /* reference-directed LMS: predicted TRN symbol (winner reg, input=1) is
               ground truth; adapt taps toward it to hold the eye against channel
               drift / clock offset that frozen taps cannot track. */
            static const double TI[4] = {  1,  1, -1, -1 };
            static const double TQ[4] = { -1,  1,  1, -1 };
            unsigned int rr = regsnap[w]; int pp0, pp1, zx, tq2;
            double ti, tq_, er, eqr, c2, s2;
            pp0 = ((rr >> 22) & 1) ^ 1;
            rr = (rr << 1) & 0x7fffff; if (b2s[w][0]) rr ^= poly;
            pp1 = ((rr >> 22) & 1) ^ 1;
            zx = (pp1 << 1) | pp0;
            tq2 = (w - zx) & 3;   /* spec CW inverse */
            ti = TI[tq2]*R; tq_ = TQ[tq2]*R;
            er = ti - pi_; eqr = tq_ - pq_;
            c2 = cos(s->srx_th); s2 = sin(s->srx_th);
            ei = er*c2 - eqr*s2; eq = er*s2 + eqr*c2;
            mu = 1e-3;
        }
        { extern int v34_dbg; if (v34_dbg && (s->cma_qn % 500) == 0) fprintf(stderr, "[srx] sym %ld w=%d wm=%d th=%.2f\n", s->cma_qn, w, wm, s->srx_th); }
        if (!s->srx_locked && s->cma_qn >= 40 && wm >= 58) {
            s->srx_locked = 1; s->srx_rot = w;
            { int j2; for (j2 = 0; j2 < 8; j2++) { s->jh_bitpos[j2] = (unsigned)(j2*2); s->jh_hist[j2] = 0; } }
            s->srx_regD = 0; s->srx_pqd = qd;
            { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[srx] TRN detected rot=%d at sym %ld (%d/64) p4=%d\n", w, s->cma_qn, wm, s->p4_mode); }
        }
        if (s->srx_locked && s->p4_mode == 1) {
            if (wm >= 55) s->p4_trn_syms++;
            if (s->p4_trn_syms >= 512) {
                s->p4_mode = 2; s->p4_ybits = 0; s->p4_ones_run = 0; s->p4_collect = 0; s->p4_last_valid = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] caller TRN 512T done at sym %ld -> MP hunt\n", s->cma_qn); }
            }
        }
        if (s->srx_locked && s->p4_mode == 2) {
            /* MP receiver: differential-CW dibits (In = pqd-qd) -> GPC self-sync FIR
               descramble (x[n]=y[n]^y[n-18]^y[n-23]) -> ring. A folding majority-vote
               decoder reads MP frames (Tables 20/21) with a real CRC through the
               hybrid-echo BER; E = >=19 descrambled ones after an MP. */
            int dqp = (s->srx_pqd - qd) & 3, kb, nbits = 2, q16 = 0;
            if (srx_rx16()) {
                /* 16-point MP: In is still DIFFERENTIAL (10.1.3.3) but comes from the
                   16-point slicer's z, and Q1,Q2 follow from the base-point index. */
                int z16;
                srx_slice16(pi_, pq_, s->rx16_rms, &q16, &z16, NULL, NULL);
                dqp = (z16 - s->rx16_z) & 3;
                s->rx16_z = z16;
                nbits = 4;
            }
            for (kb = 0; kb < nbits; kb++) {
                int yb = (kb == 0) ? (dqp & 1) : (kb == 1) ? ((dqp >> 1) & 1)
                                              : (kb == 2) ? (q16 & 1) : ((q16 >> 1) & 1);
                int xb = yb ^ ((s->p4_ybits >> 17) & 1) ^ ((s->p4_ybits >> 22) & 1);
                s->p4_ybits = ((s->p4_ybits << 1) | (unsigned int)yb) & 0x7fffff;
                s->p4_ring[s->p4_rn & P4_RING_MASK] = (u8)xb; s->p4_rn++;
                if (p4bitf) fputc('0'+xb, p4bitf);
                if (xb) { if (++s->p4_ones_run >= 19 && s->p4_mp_rx && !s->p4_e_rx) {
                            s->p4_e_rx = 1; { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] E received at sym %ld\n", s->cma_qn); } } }
                else s->p4_ones_run = 0;
                if (!s->p4_mpp_rx && s->p4_rn > 700 && (++s->p4_try >= 128)) {
                    int Ls[2] = { 88, 188 }, li;
                    s->p4_try = 0;
                    for (li = 0; li < 2; li++) {
                        /* SIPFAX: fold as deeply as the ring allows (was a fixed 8).
                            Each doubling of the repetition count roughly halves the
                            majority-vote error rate, and a CRC-clean frame is what
                            carries the peer's precoder coefficients. */
                        int L = Ls[li], per, avail = s->p4_rn > P4_RING_SZ ? P4_RING_SZ : s->p4_rn;
                        { static int pcap = -1;
                          if (pcap < 0) { char *e = getenv("SIPFAX_MP_FOLD"); pcap = e ? atoi(e) : 64; }
                          per = avail / L; if (per > pcap) per = pcap; if (per < 3) per = 3; }
                        int start2, i2, run, st;
                        static u8 maj[188]; static int votes[188];
                        if (avail < per*L) { if (avail/L >= 3) per = avail/L; else continue; }
                        if (per*L > P4_RING_SZ) per = P4_RING_SZ / L;
                        start2 = s->p4_rn - per*L;
                        for (i2 = 0; i2 < L; i2++) votes[i2] = 0;
                        for (i2 = 0; i2 < per*L; i2++) votes[i2 % L] += s->p4_ring[(start2 + i2) & P4_RING_MASK];
                        for (i2 = 0; i2 < L; i2++) maj[i2] = (votes[i2]*2 >= per);
                        run = 0; st = -1;
                        for (i2 = 0; i2 < 2*L; i2++) {
                            if (maj[i2 % L]) run++;
                            else { if (run >= 17 && i2 >= L) { st = (i2 - 17) % L; break; } run = 0; }
                        }
                        if (st < 0) continue;
                        {
                            u8 f[188]; int type, rate_ca, rate_ac, ackb, crc_off, rx_crc = 0, ok, i3, mi;
                            extern int calc_crc(u8 *buf, int size);
                            static u8 cb[188]; int cn = 0; unsigned int msk = 0;
                            for (i2 = 0; i2 < L; i2++) f[i2] = maj[(st + i2) % L];
                            type = f[18];
                            if ((type ? 188 : 88) != L) continue;
                            crc_off = type ? 171 : 69;
                            rate_ca = (f[20]<<3)|(f[21]<<2)|(f[22]<<1)|f[23];
                            rate_ac = (f[24]<<3)|(f[25]<<2)|(f[26]<<1)|f[27];
                            ackb = f[33];
                            for (mi = 0; mi < 15; mi++) msk |= ((unsigned int)f[35+mi]) << mi;
                            for (i3 = 17; i3 < crc_off; i3++) {   /* spec CRC: exclude start bits */
                                int st = (i3==17 || i3==34) || (type ? (i3>=51 && ((i3-51)%17)==0) : (i3==51 || i3==68));
                                if (!st) cb[cn++] = f[i3];
                            }
                            for (i3 = 0; i3 < 16; i3++) rx_crc |= ((int)f[crc_off+i3]) << (15-i3);
                            ok = (calc_crc(cb, cn) == rx_crc);
                            {   /* accept on CRC, or on consensus of the reliable head fields */
                                int key = (type<<28) ^ (rate_ca<<12) ^ (rate_ac<<4) ^ (f[29]<<2) ^ (f[30]<<1) ^ ackb;
                                int trel = (f[29]<<1) | f[30];
                                int consensus = (key == s->p4_key);
                                s->p4_keyn = consensus ? s->p4_keyn+1 : 1; s->p4_key = key;
                                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] FOLD L=%d type=%d ca=%d ac=%d trel=%d ack=%d nonlin=%d shape=%d crc=%s cons=%d\n", L, type, rate_ca*2400, rate_ac*2400, trel, ackb, f[31], f[32], ok?"OK":"fail", s->p4_keyn); }
                                /* SIPFAX: bit 31 is the NON-LINEAR ENCODER request (9.7)
                                   and nothing has ever read it. If the peer sets it, its
                                   transmitter warps each point radially by
                                   theta = 1 + zeta/6 + zeta^2/120 with zeta from |x|^2 -
                                   which moves symbols OFF the odd-integer lattice while
                                   leaving the amplitude distribution smoothly intact.
                                   That is exactly the signature measured here: kurtosis
                                   1.594 (real amplitude structure) with no angular
                                   structure at any rotation, rate, shaping or THP
                                   inversion. Note linmodem's own encoder stubs this out -
                                   dzeta is hardcoded to 0.3125 with the real formula
                                   commented out - so we neither apply it nor invert it. */
                                if (ok) s->peer_nonlin = f[31];
                                if (ok && type == 1) {
                                    /* SIPFAX: the fold decoder never read the peer's
                                       precoder coefficients, so peer_h stayed zero even
                                       on calls where MP decoded fine - which read as "the
                                       caller advertises h=0" when it actually meant "we
                                       never looked". Only trust them from a CRC-clean
                                       frame; a consensus frame has known bit errors and
                                       these 96 bits are not covered by the head-field
                                       agreement. Offsets 52/69/86/103/120/137, LSB-first. */
                                    static const int hof[6] = { 52, 69, 86, 103, 120, 137 };
                                    int hx, hb;
                                    for (hx = 0; hx < 6; hx++) {
                                        int v = 0;
                                        for (hb = 0; hb < 16; hb++) v |= ((int)f[hof[hx]+hb]) << hb;
                                        s->peer_h[hx] = (short)v;
                                    }
                                    { extern int v34_dbg; if (v34_dbg)
                                        fprintf(stderr, "[p4] peer precoder h = %.4f%+.4fj "
                                                "%.4f%+.4fj %.4f%+.4fj\n",
                                                s->peer_h[0]/16384.0, s->peer_h[1]/16384.0,
                                                s->peer_h[2]/16384.0, s->peer_h[3]/16384.0,
                                                s->peer_h[4]/16384.0, s->peer_h[5]/16384.0); }
                                }
                                if (ok || s->p4_keyn >= 2) {
                                    s->p4_mp_rate_ca = rate_ca; s->p4_mp_rate_ac = rate_ac; s->p4_mp_mask = msk;
                                    s->p4_trellis = trel; s->p4_mp_crcok = ok;
                                    if (!s->p4_mp_rx) { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] MP READ (%s): ca=%d ac=%d trellis=%dstate ack=%d\n", ok?"CRC":"consensus", rate_ca*2400, rate_ac*2400, (1<<(4+trel)), ackb); }
                                    s->p4_mp_rx = 1;
                                    if (ackb && !s->p4_mpp_rx) { s->p4_mpp_rx = 1; { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] MP-prime READ (%s)\n", ok?"CRC":"consensus"); } }
                                    if (ok) break;
                                }
                            }
                        }
                    }
                }
            }
        }
        if (s->jp_hunt && !s->JP_received) {
            /* SIPFAX: J' scorer. J' terminates J and is sent once (10.1.3.4). regD is
               already tracking the differential dibits, so score the same 8 word phases
               against the J' pattern. J and J' share their low 12 bits, so a genuine J
               scores only 12/16 here -- 14/16 discriminates safely. */
            int dq2 = (s->srx_pqd - qd) & 3, jb0 = dq2 & 1, jb1 = (dq2 >> 1) & 1, jj;
            unsigned int rA = s->srx_regD, rB;
            rB = (rA << 1) & 0x7fffff; if (jb0) rB ^= poly;
            for (jj = 0; jj < 8; jj++) {
                int p0 = (int)((rA >> 22) & 1) ^ (int)jppat[s->jh_bitpos[jj] & 15];
                int p1 = (int)((rB >> 22) & 1) ^ (int)jppat[(s->jh_bitpos[jj]+1) & 15];
                s->jph_hist[jj] = (s->jph_hist[jj] << 2) | ((unsigned long long)(p0 == jb0) << 1) | (unsigned long long)(p1 == jb1);
                if (__builtin_popcountll(s->jph_hist[jj] & 0xffffULL) >= 14) {
                    int rr3;
                    s->JP_received = 1; s->jp_hunt = 0;
                    if (srx_rx16()) {
                        /* SIPFAX: the caller's Phase 4 is 16-point from here. The taps are
                           frozen from a 4-point Phase 3, so restart adaptation with the
                           16-point Godard radius rather than carrying them over. */
                        {   /* SIPFAX: the channel is unchanged across the phase
                               boundary, so keep the taps trained on the 4-point Phase-3
                               TRN; only the amplitude reference needs re-estimating,
                               because 16-point slicing depends on scale. */
                            char *kt = getenv("SIPFAX_KEEPTAPS");
                            if (kt && !atoi(kt)) {
                                s->cma_phase = 0; s->cma_phn = 0;
                                s->cma_c4i = 0; s->cma_c4q = 0;
                            }
                        }
                        s->rx16_rms = 0;
                        { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] 16-point Phase 4 (keeptaps=%d)\n", (getenv("SIPFAX_KEEPTAPS") && !atoi(getenv("SIPFAX_KEEPTAPS"))) ? 0 : 1); }
                    }
                    { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[srx] caller J' detected at sym %ld (phase %d) -> Phase 4 anchored\n", s->cma_qn, jj); }
                    s->p4_mode = 1; s->srx_locked = 0;
                    for (rr3 = 0; rr3 < 4; rr3++) { s->srx_reg4[rr3] = 0; s->srx_hist4[rr3] = 0; }
                    break;
                }
            }
            if (s->jp_hunt && (s->cma_qn - s->jp_since) > 600) {
                int rr3;   /* J' missed: fall back to the old behaviour rather than hang */
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[srx] J' not seen within 600 syms -> falling back to TRN re-hunt\n"); }
                s->jp_hunt = 0; s->p4_mode = 1; s->srx_locked = 0;
                for (rr3 = 0; rr3 < 4; rr3++) { s->srx_reg4[rr3] = 0; s->srx_hist4[rr3] = 0; }
            }
        }
        if (s->srx_locked && (!s->J_received || s->jvar_wait > 0) && s->p4_mode == 0) {
            /* (spawn-based J bank replaced by always-on regD phase scorers below) */
            {   /* Always-on J detector: regD is a second scrambler register updated with
                   the DIFFERENTIAL dibit bits (the true scrambler outputs during J; it
                   self-syncs within 23 bits of J starting). 8 pattern-phase scorers run
                   against it; a phase >=28/32 while TRN agreement is broken -> J. */
                int dq_ = (s->srx_pqd - qd) & 3; int jb0 = dq_ & 1, jb1 = (dq_ >> 1) & 1;   /* spec CW */
                unsigned int rD0 = s->srx_regD, rD1;
                rD1 = (rD0 << 1) & 0x7fffff; if (jb0) rD1 ^= poly;
                s->srx_regD = (rD1 << 1) & 0x7fffff; if (jb1) s->srx_regD ^= poly;
                if (s->J_received && s->jvar_wait > 0) {
                    /* SIPFAX: J-variant vote (see above). Same regD predictions, both
                       pattern hypotheses, winning phase only. */
                    int jw = s->jvar_phase;
                    int p0 = (int)((rD0 >> 22) & 1), p1 = (int)((rD1 >> 22) & 1);
                    int k0 = s->jh_bitpos[jw] & 15, k1 = (s->jh_bitpos[jw]+1) & 15;
                    s->jvar_c4  += ((p0 ^ (int)jpat[k0])   == jb0) + ((p1 ^ (int)jpat[k1])   == jb1);
                    s->jvar_c16 += ((p0 ^ (int)jpat16[k0]) == jb0) + ((p1 ^ (int)jpat16[k1]) == jb1);
                    s->jh_bitpos[jw] += 2;
                    if (--s->jvar_wait == 0) {
                        s->rx_j16 = (s->jvar_c16 > s->jvar_c4);
                        if (s->jvar_defer) {        /* SIPFAX: now do the Phase-4 switch */
                            int rr4;
                            s->jvar_defer = 0; s->p4_mode = 1; s->srx_locked = 0;
                            for (rr4 = 0; rr4 < 4; rr4++) { s->srx_reg4[rr4] = 0; s->srx_hist4[rr4] = 0; }
                        }
                        { extern int v34_dbg; if (v34_dbg)
                            fprintf(stderr, "[srx] J variant vote: J4=%d J16=%d /192 -> %s\n",
                                    s->jvar_c4, s->jvar_c16,
                                    s->rx_j16 ? "J16POINTS (16-pt Phase 4 commanded)" : "J4POINTS"); }
                    }
                }
                for (j = 0; !s->J_received && j < 8; j++) {
                    int q0 = (int)((rD0 >> 22) & 1) ^ (int)jpat[s->jh_bitpos[j] & 15];
                    int q1 = (int)((rD1 >> 22) & 1) ^ (int)jpat[(s->jh_bitpos[j]+1) & 15];
                    s->jh_hist[j] = (s->jh_hist[j] << 2) | ((unsigned long long)(q0 == jb0) << 1) | (unsigned long long)(q1 == jb1);
                    s->jh_bitpos[j] += 2;
                    if (wm < 52 && s->cma_qn > 80) {
                        int mj = __builtin_popcountll(s->jh_hist[j] & 0xffffffffULL);
                        if (mj >= 28 && !s->J_received) {
                            int rr2;
                            s->J_received = 1;
                            {   /* SIPFAX: which J variant? J4POINTS=0x0991 and J16POINTS=0x0D91
                                   differ at exactly one bit per 16 (pattern index 5), and the
                                   caller's J commands OUR Phase-4 constellation (10.1.3.3).
                                   This caller sends 0x0D91, which fired the old J4-only
                                   correlator at 29/32 and the variant bit was silently
                                   discarded - the root cause of 13 failed handshakes (the
                                   caller trained for a 16-point TRN we never sent). One
                                   32-bit snapshot is too noisy to discriminate (measured
                                   tie 29/29 on real audio), so VOTE: keep predicting the
                                   next 96 symbols against BOTH patterns and decide by the
                                   totals - 12 variant positions instead of 2. */
                                s->jvar_wait = 96; s->jvar_phase = j;
                                s->jvar_c4 = 0; s->jvar_c16 = 0;
                                { extern int v34_dbg; if (v34_dbg)
                                    fprintf(stderr, "[srx] caller J detected at sym %ld (phase %d, %d/32) - voting variant over 96 syms\n",
                                            s->cma_qn, j, mj); }
                            }
                            {   /* SIPFAX: 11.4.1.2.1 wants J' (Table 19) detected before TRN.
                                   SIPFAX_JP=1 waits for it; otherwise keep the old behaviour. */
                                char *jpe = getenv("SIPFAX_JP");
                                if (jpe && atoi(jpe)) {
                                    s->jp_hunt = 1; s->jp_since = s->cma_qn;
                                    for (rr2 = 0; rr2 < 8; rr2++) s->jph_hist[rr2] = 0;
                                    break;   /* stay locked; the J' scorer runs below */
                                }
                            }
                            /* SIPFAX: DO NOT switch to Phase 4 while the J-variant vote is
                               still running. This block arms the vote (jvar_wait = 96) and then
                               immediately set p4_mode = 1 and srx_locked = 0 - and the gate that
                               runs the vote requires srx_locked && p4_mode == 0. So the vote was
                               dead on the very next symbol: jvar_wait stayed at 96, the
                               "J variant vote:" line never printed on a single live call, and
                               rx_j16 kept its initialised 0. We therefore decided 4-POINT on
                               100% of calls no matter what the caller commanded - while the
                               comment on the vote itself records that THIS caller sends 0x0D91,
                               J16POINTS. That is the documented root cause of the original 13
                               failed handshakes, recurring because the fix for it could never
                               complete. Defer the switch until the vote lands; 96 symbols is
                               about 28 ms at 3429 baud. SIPFAX_JVOTE=0 restores the old
                               behaviour. */
                            {   static int jv = -1;
                                if (jv < 0) { char *e = getenv("SIPFAX_JVOTE"); jv = e ? atoi(e) : 1; }
                                if (jv && s->jvar_wait > 0) {
                                    s->jvar_defer = 1;      /* switch when the vote completes */
                                } else {
                                    s->p4_mode = 1; s->srx_locked = 0;
                                    for (rr2 = 0; rr2 < 4; rr2++) { s->srx_reg4[rr2] = 0; s->srx_hist4[rr2] = 0; }
                                }
                            }
                        }
                    }
                }
            }
        }
        s->srx_pqd = qd;
    }
    {   /* MP: decision-FREE CMA tap tracking. DD decisions are wrong once timing drifts,
           so DD-LMS can't recover; CMA holds constant modulus and tracks slow drift.

           SIPFAX: this used to run for ALL of p4_mode == 2, which includes DATA MODE, and
           it silently defeated the "taps frozen" line in the Phase-C branch above (that
           sets mu = 0; this then set it straight back to 2e-3). Correct for MP, where the
           signal really is 4- or 16-point constant-modulus - catastrophic for data mode,
           where the shaped constellation has THREE distinct rings and CMA's whole purpose
           is to force one. Measured on the caller's capture: the received points collapsed
           onto a single radius, 98% of symbols landing on the four rotations of one
           |c|^2 = 10 point with nothing on the equal-energy neighbour and nothing on the
           other rings. That is CMA doing exactly what it is designed to do, to a signal
           that must not have it done. Stop at E. */
        int cma_in_data = 0;
        { static int e8 = -2; if (e8 == -2) { char *v = getenv("SIPFAX_DATA_CMA");
                                              e8 = v ? atoi(v) : 0; } cma_in_data = e8; }
        if (s->p4_mode == 2 && (!s->p4_e_rx || cma_in_data)) {
            double r2t2 = srx_rx16() ? 1.32 : 1.0;                    /* 16-pt Godard radius */
            double m2 = oi*oi + oq*oq, g = r2t2 - m2; ei = g*oi; eq = g*oq; mu = 2e-3;
        }
    }
    {   /* SIPFAX: DECISION-DIRECTED tap adaptation in data mode. CMA is wrong here - its
           cost is constant-modulus and a shaped multi-ring set has its minimum elsewhere -
           but DD-LMS is right once the decisions are mostly correct, which they now are
           (our own signal resolves to lattice-rms 0.166). Normalised by the tap-line energy
           so the step is scale-free. Off by default; SIPFAX_DATA_DDMU sets the step. */
        static double ddmu = -1;
        if (ddmu < 0) { char *e = getenv("SIPFAX_DATA_DDMU"); ddmu = e ? atof(e) : 0.0; }
        if (s->p4_e_rx && ddmu > 0 && s->data_ev) {
            double nrm = 1e-9; int k2;
            for (k2 = 0; k2 < CMANT; k2++)
                nrm += s->cma_bufi[k2]*s->cma_bufi[k2] + s->cma_bufq[k2]*s->cma_bufq[k2];
            ei = s->data_ei; eq = s->data_eq; mu = ddmu / nrm;
            s->data_ev = 0;
        }
    }
    for (i = 0; i < CMANT; i++) {
        double gi = ei*s->cma_bufi[i] + eq*s->cma_bufq[i];
        double gq = eq*s->cma_bufi[i] - ei*s->cma_bufq[i];
        s->cma_wi[i] += mu*gi; s->cma_wq[i] += mu*gq;
    }
    if (mu != 0.0) {
        /* SIPFAX: TRAILING average of the taps. A cumulative mean over the whole adaptation
           history is biased - it starts at the initial delta and includes the entire
           convergence transient, so it leans toward pre-convergence taps instead of
           smoothing the post-convergence noise (measured: it recovered only 0.452 -> 0.442
           where the tap error implies more headroom). An exponential moving average with
           time constant N tracks the converged solution and averages out the misadjustment
           that CMA leaves at mu = 2e-3. SIPFAX_TAPAVG_N sets N. */
        static double a = -1.0;
        if (a < 0) { char *e = getenv("SIPFAX_TAPAVG_N");
                     double nn = e ? atof(e) : 1000.0; if (nn < 1) nn = 1; a = 1.0/nn; }
        if (s->cma_an == 0) {
            for (i = 0; i < CMANT; i++) { s->cma_ai[i] = s->cma_wi[i]; s->cma_aq[i] = s->cma_wq[i]; }
        } else {
            for (i = 0; i < CMANT; i++) {
                s->cma_ai[i] += a*(s->cma_wi[i] - s->cma_ai[i]);
                s->cma_aq[i] += a*(s->cma_wq[i] - s->cma_aq[i]);
            }
        }
        s->cma_an++;
    }
    s->cma_cnt++;
}

/* ===================================================================
   SIPFAX: Phase-4 block receiver (16-point capable).

   A direct port of the chain validated in research/v34-rx, which decodes the captured
   caller Phase 4 at TRN score 0.99 where the streaming receiver reaches only chance. The
   streaming chain is built around a 4-point constant-modulus assumption and its output
   measures kurtosis 1.001 - it destroys the amplitude structure a 16-point constellation
   carries its data in.

   Phase 4 lasts a few seconds, so this buffers it and processes the block. That also
   restores the multi-pass equalizer convergence the Python version relies on, which a
   single-pass streaming receiver cannot do.

       3x interpolate -> exact-rational downconvert -> RRC (7 samples/symbol)
       -> Oerder-Meyr timing (closed form) -> T/2 FSE, CMA then decision-directed,
          multiple passes, with a 16-point target
   =================================================================== */

#define P4_US    3                  /* 8 kHz -> 24 kHz */
#define P4_SPS   7                  /* exactly 7 samples/symbol at 24 kHz */
#define P4_MAXIN 48000              /* 6 s of 8 kHz input */
#define P4_MAXZ  (P4_MAXIN*P4_US)
#define P4_MAXSY (P4_MAXZ/P4_SPS + 8)
#define P4_NT    31                 /* FSE taps */

/* the 16 Phase-4 points: base[q] rotated clockwise by z*90 (10.1.3.6) */
static const double p4_bi[4] = { 1, -3, 1, -3 };
static const double p4_bq[4] = { 1, 1, -3, -3 };

static void p4_point(int q, int z, double *x, double *y)
{
    double a = p4_bi[q], b = p4_bq[q], t;
    int k;
    for (k = 0; k < z; k++) { t = b; b = -a; a = t; }
    *x = a; *y = b;
}

/* root-raised-cosine, beta 0.15, at P4_SPS samples/symbol */
static double p4_rrc_tap(int i, int n, double beta, double sps)
{
    double t = (i - (n-1)/2.0)/sps, v, d;
    if (fabs(t) < 1e-8) return 1.0 - beta + 4.0*beta/M_PI;
    if (beta > 0 && fabs(fabs(t) - 1.0/(4.0*beta)) < 1e-6)
        return (beta/sqrt(2.0))*((1+2/M_PI)*sin(M_PI/(4*beta)) + (1-2/M_PI)*cos(M_PI/(4*beta)));
    d = M_PI*t*(1.0 - (4.0*beta*t)*(4.0*beta*t));
    v = sin(M_PI*t*(1-beta)) + 4.0*beta*t*cos(M_PI*t*(1+beta));
    return v/d;
}

/* Oerder-Meyr: closed-form symbol timing, in fractions of a symbol */
static double p4_timing(const double *zi, const double *zq, int n)
{
    double sr = 0, si = 0, ph;
    int m, M = (n/P4_SPS)*P4_SPS;
    for (m = 0; m < M; m++) {
        double e = zi[m]*zi[m] + zq[m]*zq[m];
        double a = -2.0*M_PI*m/P4_SPS;
        sr += e*cos(a); si += e*sin(a);
    }
    ph = atan2(si, sr);
    ph = -ph/(2.0*M_PI);
    while (ph < 0) ph += 1.0;
    while (ph >= 1.0) ph -= 1.0;
    return ph;
}

/* front end: 8 kHz real passband -> 24 kHz complex baseband, matched filtered */
static int p4_front(const short *x, int n, double *zi, double *zq)
{
    static double up[P4_MAXZ];
    static double h[P4_SPS*24+1];
    int nz = n*P4_US, i, k, nh = P4_SPS*24+1;
    double s = 0;
    if (nz > P4_MAXZ) { nz = P4_MAXZ; n = nz/P4_US; }
    /* 3x interpolation: zero-stuff then lowpass with the RRC itself (it is the
       matched filter and band-limits to well under 4 kHz) */
    for (i = 0; i < nz; i++) up[i] = 0;
    for (i = 0; i < n; i++) up[i*P4_US] = x[i];
    for (i = 0; i < nh; i++) { h[i] = p4_rrc_tap(i, nh, 0.15, P4_SPS); s += h[i]*h[i]; }
    s = sqrt(s);
    for (i = 0; i < nh; i++) h[i] /= s;
    /* downconvert (exact: 4/49 cycles per 24 kHz sample) then matched filter */
    for (i = 0; i < nz; i++) {
        double a = -2.0*M_PI*(4.0/49.0)*i, c = cos(a), sn = sin(a);
        double v = up[i]*P4_US;
        zi[i] = v*c; zq[i] = v*sn;
    }
    {   /* in-place FIR over the complex baseband */
        static double ti[P4_MAXZ], tq[P4_MAXZ];
        for (i = 0; i < nz; i++) {
            double ai = 0, aq = 0;
            int lo = i - nh/2;
            for (k = 0; k < nh; k++) {
                int j = lo + k;
                if (j >= 0 && j < nz) { ai += h[k]*zi[j]; aq += h[k]*zq[j]; }
            }
            ti[i] = ai; tq[i] = aq;
        }
        for (i = 0; i < nz; i++) { zi[i] = ti[i]; zq[i] = tq[i]; }
    }
    {   /* normalise to unit mean power */
        double p = 0;
        for (i = 0; i < nz; i++) p += zi[i]*zi[i] + zq[i]*zq[i];
        p = (nz > 0) ? sqrt(p/nz) : 1.0;
        if (p > 1e-12) for (i = 0; i < nz; i++) { zi[i] /= p; zq[i] /= p; }
    }
    return nz;
}

/* nearest point of the signal set; returns (q,z) and optionally the point itself */
static void p4_slice(double x, double y, int sixteen, int *qo, int *zo, double *dx, double *dy)
{
    int q, z, bq2 = 0, bz = 0, nq = sixteen ? 4 : 1;
    double bd = 1e30, px, py;
    for (q = 0; q < nq; q++)
        for (z = 0; z < 4; z++) {
            double ex, ey, d;
            p4_point(q, z, &px, &py);
            ex = x - px; ey = y - py; d = ex*ex + ey*ey;
            if (d < bd) { bd = d; bq2 = q; bz = z; }
        }
    *qo = bq2; *zo = bz;
    if (dx) { p4_point(bq2, bz, &px, &py); *dx = px; *dy = py; }
}

/* T/2 fractionally-spaced equalizer: CMA warm-up then decision-directed, multi-pass. */
/* SIPFAX: `reset` re-initialises the taps. It used to be unconditional, which meant every
   pass of a decode had to run inside one call. The taps are static and persist between
   calls, so with reset under the caller's control the same multi-pass scheme can be driven
   ONE pass per call and amortised across audio frames - see p4_block_step. */
/* SIPFAX: Gardner loop gains; swept offline against a live capture (see FINDINGS 39). */
/* SIPFAX: swept defaults. The block-path Gardner loop shipped disabled (kp=0) with sign
   +1, and the sign matters enormously - +1 drove the residual to 41-56% against 13-14% at
   -1. With the loop enabled at these values the per-block EVM V across the caller's TRN
   flattens (37.6/3.6/15.6 -> 3.9/3.4/3.8). Leaving it off cost lattice-rms 0.498 against
   0.166 on a signal that resolves. */
static double p4_ted_kp = 0.10, p4_ted_ki = 0.002, p4_ted_sign = -1.0, p4_ted_last = 0.0;
static void p4_ted_init(void)
{
    static int done; char *e;
    if (done) return; done = 1;
    e = getenv("SIPFAX_TED_KP");   if (e) p4_ted_kp   = atof(e);
    e = getenv("SIPFAX_TED_KI");   if (e) p4_ted_ki   = atof(e);
    e = getenv("SIPFAX_TED_SIGN"); if (e) p4_ted_sign = atof(e);
}

static int p4_equalize(const double *zi, const double *zq, int nz, double off,
                       int sixteen, int ncma, int ndd, int reset, double *si, double *sq)
{
    static double wi[P4_NT], wq[P4_NT], bufi[P4_NT], bufq[P4_NT];
    double R2 = sixteen ? 1.32 : 1.0;          /* Godard radius, unit mean power */
    double scale = sixteen ? sqrt(10.0) : sqrt(2.0);   /* base units -> unit power */
    int p, i, ns = 0;
    p4_ted_init();
    if (reset) {
        for (i = 0; i < P4_NT; i++) { wi[i] = 0; wq[i] = 0; }
        wi[P4_NT/2] = 1.0;
    }
    for (p = 0; p < ncma + ndd; p++) {
        int dd = (p >= ncma), cnt = 0;
        double pos = off*P4_SPS + P4_SPS*8, th = 0, fr = 0, g = 1.0;
        /* SIPFAX: symbol-clock recovery. This loop used to take ONE timing estimate at the
           top of the burst and then advance by a fixed P4_SPS/2 forever, with no tracking
           anywhere in the Phase-4 or data-mode receiver. Measured per-block EVM across the
           caller's TRN came out as a clean V - 37.6% / 3.6% at the middle / 15.6% - with
           the residual phase flat at +-0.4 deg, i.e. not carrier but sampling instant: a
           ~20 ppm clock offset between the two ends of the RTP path, accumulating
           unopposed while the fractionally-spaced taps absorb only the burst average. At
           the V's minimum the EVM is 3.6% (28.9 dB), so the line is fine and the 16 dB we
           were measuring was entirely our own drift.

           Gardner is the right detector here: it needs the T/2 samples we already compute
           and no knowledge of the constellation, so the same loop serves TRN and data mode.
                e = ( y(n) - y(n-1) ) . y(n-1/2)
           tfr accumulates the rate error (samples per half-symbol); the proportional term
           nudges the phase directly. */
        double tfr = 0, gmi = 0, gmq = 0, gpi = 0, gpq = 0;
        for (i = 0; i < P4_NT; i++) { bufi[i] = 0; bufq[i] = 0; }
        ns = 0;
        while (pos < nz - 2) {
            int i0 = (int)pos, k;
            double f = pos - i0, yi, yq, oi, oq, nrm = 1e-2;
            yi = zi[i0]*(1-f) + zi[i0+1]*f;
            yq = zq[i0]*(1-f) + zq[i0+1]*f;
            for (k = P4_NT-1; k > 0; k--) { bufi[k] = bufi[k-1]; bufq[k] = bufq[k-1]; }
            bufi[0] = yi; bufq[0] = yq;
            oi = 0; oq = 0;
            for (k = 0; k < P4_NT; k++) {
                oi += wi[k]*bufi[k] - wq[k]*bufq[k];
                oq += wi[k]*bufq[k] + wq[k]*bufi[k];
                nrm += bufi[k]*bufi[k] + bufq[k]*bufq[k];
            }
            cnt++;
            if (cnt & 1) {                       /* mid-symbol: Gardner's y(n-1/2) */
                gmi = oi; gmq = oq;
                pos += P4_SPS/2.0 + tfr; continue;
            }
            {
                double ei, eq, mu, ypi, ypq, c = cos(-th), s2 = sin(-th);
                ypi = (oi*c - oq*s2)*g; ypq = (oi*s2 + oq*c)*g;
                if (!dd) {
                    double m2 = oi*oi + oq*oq, gg = R2 - m2;
                    ei = gg*oi; eq = gg*oq; mu = 2e-3;
                } else {
                    int q2, z2; double dxx, dyy, pe, dri, drq, cc, ss;
                    p4_slice(ypi*scale, ypq*scale, sixteen, &q2, &z2, &dxx, &dyy);
                    dxx /= scale; dyy /= scale;
                    pe = atan2(ypq*dxx - ypi*dyy, ypi*dxx + ypq*dyy);
                    fr += 1e-4*pe; th += fr + 5e-3*pe;
                    g *= (1.0 + 2e-4*(sqrt(dxx*dxx+dyy*dyy) - sqrt(ypi*ypi+ypq*ypq)));
                    cc = cos(th); ss = sin(th);
                    dri = (dxx*cc - dyy*ss)/g; drq = (dxx*ss + dyy*cc)/g;
                    ei = dri - oi; eq = drq - oq; mu = 2e-3;
                }
                for (k = 0; k < P4_NT; k++) {
                    wi[k] += mu*(ei*bufi[k] + eq*bufq[k])/nrm;
                    wq[k] += mu*(eq*bufi[k] - ei*bufq[k])/nrm;
                }
                if (ns < P4_MAXSY) { si[ns] = ypi; sq[ns] = ypq; ns++; }
            }
            {   /* Gardner TED, normalised so the gains are independent of level */
                double ted = (oi - gpi)*gmi + (oq - gpq)*gmq;
                double pwn = oi*oi + oq*oq + gpi*gpi + gpq*gpq + 1e-9;
                ted = p4_ted_sign * ted / pwn;
                pos += p4_ted_kp * ted;          /* phase */
                tfr += p4_ted_ki * ted;          /* rate  */
                if (tfr >  0.02) tfr =  0.02;    /* +-4000 ppm: far past any real clock */
                if (tfr < -0.02) tfr = -0.02;
                p4_ted_last = tfr;
                gpi = oi; gpq = oq;
            }
            pos += P4_SPS/2.0 + tfr;
        }
    }
    return ns;
}

/* self-synchronising descrambler: out = MSB(reg) ^ in, reg driven by the RECEIVED bit */
static int p4_descr_ones(const int *bits, int n, int poly)
{
    unsigned int reg = 0;
    int i, ones = 0;
    for (i = 0; i < n; i++) {
        int o = (int)((reg >> 22) & 1) ^ bits[i];
        reg = (reg << 1) & 0x7fffff;
        if (bits[i]) reg ^= (unsigned int)poly;
        ones += o;
    }
    return ones;
}

/* TRN score: ABSOLUTE rotation (10.1.3.6). ~1.0 => TRN decoded. */
static double p4_trn_score(const double *si, const double *sq, int ns, int sixteen, int poly)
{
    static int bits[P4_MAXSY*4];
    double best = 0, pw = 0, scale;
    int rot, i;
    for (i = 0; i < ns; i++) pw += si[i]*si[i] + sq[i]*sq[i];
    if (ns < 64 || pw <= 0) return 0;
    scale = sqrt((sixteen ? 10.0 : 2.0)*ns/pw);
    for (rot = 0; rot < 4; rot++) {
        int nb = 0, on;
        double cr = cos(rot*M_PI/2), sr = sin(rot*M_PI/2), f;
        for (i = 0; i < ns; i++) {
            double x = (si[i]*cr - sq[i]*sr)*scale, y = (si[i]*sr + sq[i]*cr)*scale;
            int q2, z2;
            p4_slice(x, y, sixteen, &q2, &z2, NULL, NULL);
            bits[nb++] = z2 & 1; bits[nb++] = (z2 >> 1) & 1;
            if (sixteen) { bits[nb++] = q2 & 1; bits[nb++] = (q2 >> 1) & 1; }
        }
        on = p4_descr_ones(bits, nb, poly);
        f = (double)on/nb;
        if (f < 0.5) f = 1.0 - f;
        if (f > best) best = f;
    }
    return best;
}


/* MP frames from equalised Phase-4 symbols.
   MP uses DIFFERENTIAL rotation (10.1.3.3: Zn = In + Zn-1), unlike TRN which is absolute,
   so the receiver phase ambiguity cancels and no rotation search is needed. Bits per symbol
   are I1,I2 from the differential rotation and, for 16-point, Q1,Q2 from the base index. */
static int p4_mp_decode(const double *si, const double *sq, int ns, int sixteen, int poly,
                        int *out_ca, int *out_ac, int *out_trel, int *out_ack,
                        int *out_shape, unsigned int *out_mask, short *out_h,
                        int *out_nonlin)
{
    static int raw[P4_MAXSY*4], db[P4_MAXSY*4];
    double pw = 0, scale;
    unsigned int reg = 0;
    int i, nb = 0, pz = -1, found = 0;
    if (ns < 200) return 0;
    for (i = 0; i < ns; i++) pw += si[i]*si[i] + sq[i]*sq[i];
    if (pw <= 0) return 0;
    scale = sqrt((sixteen ? 10.0 : 2.0)*ns/pw);
    for (i = 0; i < ns; i++) {
        int q2, z2, dz;
        p4_slice(si[i]*scale, sq[i]*scale, sixteen, &q2, &z2, NULL, NULL);
        if (pz >= 0) {
            dz = (z2 - pz) & 3;
            raw[nb++] = dz & 1; raw[nb++] = (dz >> 1) & 1;
            if (sixteen) { raw[nb++] = q2 & 1; raw[nb++] = (q2 >> 1) & 1; }
        }
        pz = z2;
    }
    for (i = 0; i < nb; i++) {          /* self-synchronising descramble */
        db[i] = (int)((reg >> 22) & 1) ^ raw[i];
        reg = (reg << 1) & 0x7fffff;
        if (raw[i]) reg ^= (unsigned int)poly;
    }
    for (i = 0; i + 190 < nb; i++) {    /* hunt the 17-bit all-ones sync, then verify CRC */
        int k, ok = 1, type, L, co, cn = 0, rxc = 0;
        static u8 cov[200];
        for (k = 0; k < 17; k++) if (!db[i+k]) { ok = 0; break; }
        if (!ok) continue;
        type = db[i+18]; L = type ? 188 : 88; co = type ? 171 : 69;
        if (i + L > nb) break;
        for (k = 17; k < co; k++) {
            int st = (k == 17 || k == 34) ||
                     (type ? (k >= 51 && ((k-51) % 17) == 0) : (k == 51 || k == 68));
            if (!st) cov[cn++] = (u8)db[i+k];
        }
        for (k = 0; k < 16; k++) rxc |= db[i+co+k] << (15-k);
        if (calc_crc(cov, cn) != rxc) continue;
        found++;
        {   /* SIPFAX: report the LAST frame's fields, and OR the ack across the whole
               window. Reporting the FIRST frame - the OLDEST audio in a 2.5 s sliding
               window - hid the caller's ack: the working caller flips ack 0->1 mid-run
               (slmodem's own MP does the same, 24 ack=0 frames then 4 ack=1), so the
               window's head stays ack=0 for up to 2.5 s after the caller has actually
               acknowledged - longer than it waits for our E before retraining. */
            if (out_ca)  *out_ca  = (db[i+20]<<3)|(db[i+21]<<2)|(db[i+22]<<1)|db[i+23];
            if (out_ac)  *out_ac  = (db[i+24]<<3)|(db[i+25]<<2)|(db[i+26]<<1)|db[i+27];
            if (out_trel) *out_trel = (db[i+29]<<1)|db[i+30];
            if (out_shape) *out_shape = db[i+32];
            /* SIPFAX: bit 31 is the peer's non-linear-encoder request. This decoder pulled
               out shape (bit 32) and the precoder coefficients but never nonlin, and it is
               the decoder the LIVE path uses - the fold path that does read f[31] only runs
               offline. So peer_nonlin stayed 0 on every live call no matter what the caller
               asked for, which is why the precoder fired live and the non-linear encoder
               never did: same CRC gate, same rx->tx copy, but one field was simply never
               extracted. Measured: captures decode nonlin=1 offline while the live journal
               recorded zero "non-linear encoder ON" events on the same calls. */
            if (out_nonlin) *out_nonlin = db[i+31];
            if (out_ack) { if (found == 1) *out_ack = db[i+33]; else *out_ack |= db[i+33]; }
            if (out_mask) { unsigned int m = 0; for (k = 0; k < 15; k++) m |= (unsigned int)db[i+35+k] << k; *out_mask = m; }
            if (out_h && type == 1) {
                /* SIPFAX: the peer's precoder coefficients, at frame bit offsets
                   52/69/86/103/120/137 (each preceded by a start bit), LSB-first. These
                   are computed by the PEER'S RECEIVER for OUR TRANSMITTER (9.6), so they
                   belong to the transmit path, not to ours. */
                static const int hoff[6] = { 52, 69, 86, 103, 120, 137 };
                int hi2, hb2;
                for (hi2 = 0; hi2 < 6; hi2++) {
                    int v = 0;
                    for (hb2 = 0; hb2 < 16; hb2++) v |= db[i + hoff[hi2] + hb2] << hb2;
                    out_h[hi2] = (short)v;
                }
            }
        }
        i += L - 1;
    }
    return found;
}


/* Block receiver over buffered Phase-4 audio, AMORTISED: each call performs at most one
   bounded unit of work - the front end, or a single equaliser pass, or the MP decode.

   It used to do the whole decode in one call: front end plus 2 constellations x 16 passes
   over 20000 samples. That is ~100 ms of arithmetic, and it ran inside the audio callback,
   so the transmit path was starved for five packet-times every second. Measured against a
   working slmodem call: slmodem never exceeds 27 ms between RTP packets, while this stalled
   17 times over 40 ms and 5 times over 100 ms, at exactly 1.00 s intervals matching this
   decoder's cadence, beginning the instant our MP started. Our own MP carrier therefore had
   a ~100 ms hole punched in it every second, which is reason enough for a peer to ignore it.

   One pass is ~3 ms against a 20 ms frame, so a full decode now spans ~18 frames (~360 ms)
   and nothing is ever starved. 4-point is tried first because that is what the caller uses;
   16-point is retried on the same buffered audio before giving up and taking a fresh window. */
#define P4_NCMA 6
#define P4_NDD  10

static double p4_zi[P4_MAXZ], p4_zq[P4_MAXZ], p4_si[P4_MAXSY], p4_sq[P4_MAXSY];
static int    p4_nz, p4_ns, p4_stage, p4_pass, p4_six;
static double p4_off;

static void p4_block_reset(void)
{
    p4_stage = 0; p4_pass = 0; p4_six = 0; p4_nz = 0; p4_ns = 0;
}

/* SIPFAX: (9.6) the precoder coefficients are computed by the RECEIVER, from the residual
   ISI it still sees after its own equaliser, and handed to the far TRANSMITTER in MP so
   that IT pre-cancels them. We have advertised h = 0,0,0 in every call to date - telling
   the caller "my channel is flat, do not precode" - while the caller sends us real
   coefficients (0.285, 0.167, 0.102) because it measured real ISI on its side.

   Our equaliser is trained on Phase-4 TRN and then frozen: CMA has no usable error signal
   on a multi-ring shaped constellation and decision-directed adaptation has no gradient
   until the decisions are already mostly right. So whatever ISI is left at the end of TRN
   stays there for the whole of data mode. Precoding is the mechanism the spec provides for
   exactly this situation, and we have been declining it.

   Estimate the post-cursor ISI from TRN, whose 4-point symbols the slicer recovers
   reliably (measured 8.7% EVM), by correlating the slicer error with delayed decisions:

       y(n) = d(n) + SUM_k h_k d(n-k) + noise
       e(n) = y(n) - d(n)   =>   h_k = E{ e(n) d*(n-k) } / E{|d|^2}

   If the h_k come out at the noise floor the residual is additive noise and precoding
   cannot help; if they are well above it the residual is ISI and it can. */
static s16 p4_hest[3][2];
static int p4_have_h;

static void p4_est_precoder(const double *si, const double *sq, int ns, int sixteen)
{
    double scale = sixteen ? sqrt(10.0) : sqrt(2.0);
    double ar[3] = {0,0,0}, ai[3] = {0,0,0}, pw = 0, ep = 0;
    double dhi[4] = {0,0,0,0}, dhq[4] = {0,0,0,0};
    int i, k, nn = 0;
    if (ns < 600) return;
    for (i = 200; i < ns; i++) {
        double yi = si[i]*scale, yq = sq[i]*scale, dx, dy, ei, eq;
        int q2, z2;
        p4_slice(yi, yq, sixteen, &q2, &z2, &dx, &dy);
        ei = yi - dx; eq = yq - dy;
        for (k = 3; k >= 1; k--) { dhi[k] = dhi[k-1]; dhq[k] = dhq[k-1]; }
        dhi[0] = dx; dhq[0] = dy;
        if (nn >= 3) {
            for (k = 1; k <= 3; k++) {            /* e(n) * conj(d(n-k)) */
                ar[k-1] += ei*dhi[k] + eq*dhq[k];
                ai[k-1] += eq*dhi[k] - ei*dhq[k];
            }
            pw += dx*dx + dy*dy;
            ep += ei*ei + eq*eq;
        }
        nn++;
    }
    if (pw <= 0 || nn < 400) return;
    fprintf(stderr, "[p4] precoder estimate from TRN (%d syms, residual %.1f%% rms):\n",
            nn, 100.0*sqrt(ep/pw));
    fprintf(stderr, "[p4] block-path clock: %+.1f ppm\n", p4_ted_last/(P4_SPS/2.0)*1e6);
    {   /* SIPFAX: is that residual stationary noise, or drift? p4_equalize advances its
           read pointer by a FIXED P4_SPS/2 with no timing-recovery loop, so any sample
           clock offset between the caller and us accumulates; a fractionally-spaced
           equaliser absorbs some of that by sliding its taps, but only slowly and only
           within its span. Stationary EVM across the burst means additive noise and a
           genuinely poor line; EVM that ramps, or a phase that walks, means we are
           losing the margin ourselves. */
        int b, nb = nn/500;
        for (b = 0; b < nb && b < 16; b++) {
            double be = 0, bp = 0, sr = 0, si2 = 0;
            int j0 = 200 + b*500, j;
            for (j = j0; j < j0+500 && j < ns; j++) {
                double yi = si[j]*scale, yq = sq[j]*scale, dx, dy, e1, e2;
                int q2, z2;
                p4_slice(yi, yq, sixteen, &q2, &z2, &dx, &dy);
                e1 = yi - dx; e2 = yq - dy;
                be += e1*e1 + e2*e2; bp += dx*dx + dy*dy;
                sr += yi*dx + yq*dy; si2 += yq*dx - yi*dy;   /* mean residual rotation */
            }
            if (bp <= 0) continue;
            fprintf(stderr, "[p4]   block %2d: EVM %5.1f%%  resid-phase %+6.2f deg\n",
                    b, 100.0*sqrt(be/bp), atan2(si2, sr)*180.0/M_PI);
        }
    }
    for (k = 0; k < 3; k++) {
        double hr = ar[k]/pw, hi = ai[k]/pw;
        /* a tap driven only by white noise lands near sqrt(ep/pw/nn) - the floor */
        fprintf(stderr, "[p4]   h%d = %+.4f %+.4fj  |h|=%.4f  (noise floor %.4f)\n",
                k+1, hr, hi, sqrt(hr*hr+hi*hi), sqrt(ep/pw/(double)nn));
        p4_hest[k][0] = (s16)lrint(hr * 16384.0);
        p4_hest[k][1] = (s16)lrint(hi * 16384.0);
    }
    p4_have_h = 1;
}

static int p4_block_step(const short *x, int n, int *ca, int *ac, int *trel, int *ack,
                         int *shape, unsigned int *mask, int *sixteen_out, short *hout,
                         int *nlout)
{
    if (p4_stage == 0) {                       /* snapshot: front end + symbol timing */
        if (n < 8000) return 0;
        p4_nz  = p4_front(x, n, p4_zi, p4_zq);
        p4_off = p4_timing(p4_zi, p4_zq, p4_nz);
        p4_pass = 0; p4_six = 0; p4_stage = 1;
        return 0;
    }
    if (p4_stage == 1) {                       /* exactly one equaliser pass */
        int dd = (p4_pass >= P4_NCMA);
        p4_ns = p4_equalize(p4_zi, p4_zq, p4_nz, p4_off, p4_six,
                            dd ? 0 : 1, dd ? 1 : 0, p4_pass == 0, p4_si, p4_sq);
        if (++p4_pass >= P4_NCMA + P4_NDD) p4_stage = 2;
        return 0;
    }
    {                                          /* decode what the passes produced */
        int nmp;
        if (!p4_have_h) p4_est_precoder(p4_si, p4_sq, p4_ns, p4_six);
        nmp = p4_mp_decode(p4_si+200, p4_sq+200, p4_ns-200 > 0 ? p4_ns-200 : 0,
                               p4_six, V34_GPC, ca, ac, trel, ack, shape, mask, hout,
                               nlout);
        if (nmp) {
            if (sixteen_out) *sixteen_out = p4_six;
            p4_stage = 0;                      /* next window */
            return nmp;
        }
        if (!p4_six) { p4_six = 1; p4_pass = 0; p4_stage = 1; }  /* other constellation */
        else         { p4_stage = 0; }                           /* both failed: fresh audio */
        return 0;
    }
}


/* SIPFAX: re-derive the data-frame and constellation parameters after Phase 4 has
   negotiated them, WITHOUT calling V34_init_low - that would wipe the equaliser taps,
   symbol timing and carrier phase we spent all of Phase 3/4 acquiring, which is exactly
   the state a V.34 receiver must carry into data mode (there is no training signal there
   to re-acquire from). Mirrors the parameter block in V34_init_low. */
/* SIPFAX: nearest point of the negotiated data constellation, in lattice-coordinate
   units. L is 48-56 here, so the linear search costs ~50 distance evaluations per symbol
   at 3429 baud - negligible, and it avoids duplicating the shell-mapping geometry. */
/* SIPFAX: rms distance of a block to the nearest ODD-INTEGER lattice point, in lattice
   units. The data constellation is a subset of that lattice, so this scores a candidate
   (gain, phase) without needing to know WHICH subset - which matters, because the shell
   mapper's shaping makes the occupied subset data-dependent. Uniform/garbage scores
   ~0.577; a correctly scaled and derotated block scores ~0.13. */
static double data_lattice_rms(const double *bi, const double *bq, int n,
                               double g, double ct, double st)
{
    double acc = 0; int i;
    for (i = 0; i < n; i++) {
        double xr = (bi[i]*ct - bq[i]*st) * g;
        double xi2 = (bi[i]*st + bq[i]*ct) * g;
        double dr = xr - (2.0*floor((xr-1.0)/2.0 + 0.5) + 1.0);
        double di2 = xi2 - (2.0*floor((xi2-1.0)/2.0 + 0.5) + 1.0);
        acc += dr*dr + di2*di2;
    }
    return sqrt(acc / (2.0*n));
}

/* SIPFAX: slice against the FULL constellation. This used to scan s->constellation[0..L),
   which is wrong twice over: the array holds only the QUARTER constellation (L/4 entries
   are in the negotiated set, the rest are higher-energy coset points that the peer never
   transmits), and every entry lies on the (4x+1, 4y+1) coset, so a decision could never
   land on any of the three rotations that make up the rest of the odd-integer lattice.
   Walk the L/4 real points through all four rotations instead - that is exactly the set
   the 9.6.1 mapper can emit. */
static int data_slice(V34DSPState *s, double xi, double xq, double *di, double *dq)
{
    int i, j, nq = s->L / 4, best = 0; double bd = 1e30;
    *di = 1.0; *dq = 1.0;
    for (i = 0; i < nq; i++) {
        int x1 = s->constellation[i][0], y1 = s->constellation[i][1];
        for (j = 0; j < 4; j++) {
            int x, y; double dx, dy, d;
            rotate_clockwise(x, y, x1, y1, j);
            dx = xi - (double)x; dy = xq - (double)y; d = dx*dx + dy*dy;
            if (d < bd) { bd = d; best = i; *di = (double)x; *dq = (double)y; }
        }
    }
    return best;
}

/* SIPFAX: configure the TRANSMIT data-mode parameters from the negotiated rate.
   V34_init hardcodes s->R = 19200 with a "TODO: derive S/R from the negotiation", and nothing
   ever did - so at the Phase-4 -> data handoff the transmitter built a 19200 constellation
   (logged live as L=88) while our own MP had told the caller ac=9600 (L=12). The caller
   therefore tried to demodulate our data against a 9600 set, failed, and retrained; measured
   on two live calls, it looped Phase 4 on a 1.51 s cycle for the whole window and then sent
   the 1200 Hz abandon tone. This is the transmit-side mirror of the receive cap fixed earlier.
   Deliberately NOT a call to v34_rx_data_params: that resets conv_reg, the scrambler register
   and Z_1, which on a TX instance are the live encoder state and must run continuously. */
static void v34_tx_data_params(V34DSPState *s, int R)
{
    int S = s->S, d, e;
    s->R = R;
    if (!s->use_high_carrier) { d = S_tab[S][2]; e = S_tab[S][3]; }
    else                      { d = S_tab[S][4]; e = S_tab[S][5]; }
    s->symbol_rate = 2400.0 * (float)S_tab[S][0] / (float)S_tab[S][1];
    s->carrier_freq = s->symbol_rate * (float)d / (float)e;
    s->J = S_tab[S][6];
    s->P = S_tab[S][7];
    s->N = (s->R * 28) / (s->J * 100);
    s->b = s->N / s->P;
    if ((s->b * s->P) < s->N) s->b++;
    s->r = s->N - (s->b - 1) * s->P;
    s->W = 0;
    s->q = 0;
    if (s->b <= 12) s->K = 0;
    else { s->K = s->b - 12; while (s->K >= 32) { s->K -= 8; s->q++; } }
    if (!s->expanded_shape) s->M = (int) ceil(pow(2.0, s->K / 8.0));
    else                    s->M = (int) rint(1.25 * pow(2.0, s->K / 8.0));
    s->L = 4 * s->M * (1 << s->q);
    build_constellation(s);
    build_rings(s);
}

static void v34_rx_data_params(V34DSPState *s, int R)
{
    int S = s->S, d, e;
    s->R = R;
    if (!s->use_high_carrier) { d = S_tab[S][2]; e = S_tab[S][3]; }
    else                      { d = S_tab[S][4]; e = S_tab[S][5]; }
    s->symbol_rate = 2400.0 * (float)S_tab[S][0] / (float)S_tab[S][1];
    s->carrier_freq = s->symbol_rate * (float)d / (float)e;
    s->J = S_tab[S][6];
    s->P = S_tab[S][7];
    s->N = (s->R * 28) / (s->J * 100);
    s->b = s->N / s->P;
    if ((s->b * s->P) < s->N) s->b++;      /* the round-up matters: b=40 not 39 */
    s->r = s->N - (s->b - 1) * s->P;
    s->W = 0;                               /* no aux channel */
    s->q = 0;
    if (s->b <= 12) s->K = 0;
    else { s->K = s->b - 12; while (s->K >= 32) { s->K -= 8; s->q++; } }
    { char *e2 = getenv("SIPFAX_SHAPE"); if (e2) s->expanded_shape = atoi(e2); }
    if (!s->expanded_shape) s->M = (int) ceil(pow(2.0, s->K / 8.0));
    else                    s->M = (int) rint(1.25 * pow(2.0, s->K / 8.0));
    s->L = 4 * s->M * (1 << s->q);
    build_constellation(s);
    build_rings(s);
    {   /* SIPFAX: RESET THE DECODER STATE. This function exists to avoid V34_init_low so
           that the equaliser taps, symbol timing and carrier phase acquired over Phase 3/4
           survive into data mode - which is right. But V34_init_low is also the only place
           that zeroes the decoder's stateful counters, and they were silently left behind.

           rcnt is the damaging one: it drives the alternation between b-1 and b bits per
           mapping frame (rcnt += r, mod P). If the receiver's phase differs from the
           transmitter's, EVERY frame is mis-sized and the bit extraction is wrong forever,
           however clean the symbols are. Its cycle is P/gcd(r,P) = 5 frames at r=6 P=15,
           i.e. 40 symbols, so no sweep shorter than that can even see it - which is why a
           16-symbol alignment sweep came back flat and uninformative.

           Z_1 is the differential quadrant reference (10.1.3.3); stale, it applies a
           persistent 90-degree offset to every decoded quadrant. conv_reg is the trellis
           memory, s->x the precoder history, and phase_4d the 4D pairing parity.

           Exactly the fields V34_init_low clears, and nothing else - no taps, no timing,
           no carrier. SIPFAX_DEC_RESET=0 restores the old behaviour. */
        static int dr = -1;
        if (dr < 0) { char *e3 = getenv("SIPFAX_DEC_RESET"); dr = e3 ? atoi(e3) : 1; }
        if (dr) {
            s->phase_4d = 0;
            s->Z_1 = 0;
            s->U0 = 0;
            memset(s->x, 0, sizeof(s->x));
            s->half_data_frame_count = 0;
            s->sync_count = 0;
            s->conv_reg = 0;
            {   /* SIPFAX: Viterbi bootstrap. state_error was never initialised anywhere -
                   not here, not in V34_init_low - so every state started equal, and equal
                   path metrics are an ABSORBING state for this ACS: each next-state's 16
                   incoming transitions span the full set of branch metrics, so every state
                   takes the global minimum and they stay identical forever. Measured: all
                   64 state metrics exactly equal on every symbol (spread 0.0) while the
                   branch metrics spread ~700. Start from one known state so the paths can
                   differentiate. */
                int q7;
                for (q7 = 0; q7 < TRELLIS_MAX_STATES; q7++) s->state_error[q7] = 1 << 20;
                {   /* SIPFAX: the receiver runs a constant 180 deg rotation, and sigma_180 maps the
            64 state labels by exactly XOR 32 (verified over all 256 coset 4-tuples through
            the encoder's own algebra). The code is RECURSIVE, so a decoder seeded at 0 while
            the encoder is effectively at 32 never merges - the trajectories stay disjoint
            forever even when every branch decision is correct. SIPFAX_DEC_SEED picks the
            seed state; 32 should put the decoder on the encoder's own trajectory. */
        static int seed = -1;
        if (seed < 0) { char *e = getenv("SIPFAX_DEC_SEED"); seed = e ? atoi(e) : 0; }
        s->state_error[seed & (TRELLIS_MAX_STATES-1)] = 0; }
            }
            s->scrambler_reg = 0;
            s->mapping_frame = 0;
            s->acnt = 0;
            s->rcnt = 0;
            { extern int v34_dbg; if (v34_dbg)
                fprintf(stderr, "[data] decoder state reset (rcnt/acnt/phase_4d/Z_1/U0/"
                        "conv_reg/scrambler/mapping_frame); equaliser preserved\n"); }
        }
    }
    { extern int v34_dbg; if (v34_dbg) {
        int ci; double acc=0; int mx=0;
        for (ci=0; ci<s->L; ci++){ acc += (double)s->constellation[ci][0]*s->constellation[ci][0]
                                        + (double)s->constellation[ci][1]*s->constellation[ci][1];
                                   if (abs(s->constellation[ci][0])>mx) mx=abs(s->constellation[ci][0]); }
        fprintf(stderr,"[data] constellation: L=%d first=(%d,%d) max|x|=%d mean|c|^2=%.1f\n",
                s->L, s->constellation[0][0], s->constellation[0][1], mx, acc/s->L); } }
    { extern int v34_dbg; if (v34_dbg)
        fprintf(stderr, "[data] RX params: R=%d S=%.0f J=%d P=%d N=%d b=%d r=%d K=%d q=%d M=%d L=%d shape=%d trellis=%d\n",
                s->R, s->symbol_rate, s->J, s->P, s->N, s->b, s->r,
                s->K, s->q, s->M, s->L, s->expanded_shape, s->conv_nb_states); }
}

void V34_demod_cma(V34DSPState *s, const s16 *samples, unsigned int nb)
{
    {   /* SIPFAX: Phase-4 block receiver. The streaming path is 4-point only, so when the
           caller switches to 16-point it sees nothing. Buffer a few seconds of Phase 4 and
           run the block chain once; its parameters feed the same p4_* fields the transmit
           state machine already keys on, so MP -> MP' -> E proceeds normally. */
        static short p4b[P4_MAXIN];
        static int p4bn = 0;
        if (s->p4_mode == 0) { p4bn = 0; p4_block_reset(); }
        else if (!s->p4_e_rx && !s->p4_mpp_rx) {
            /* SIPFAX: this gate used to be srx_rx16() && ..., which conflated two
               meanings - "the 16-pt block path is enabled" and "the caller transmits
               16-pt". Re-keying srx_rx16() to what our J commanded (correct for the
               Godard radius) silently disabled the block receiver entirely: a full live
               call went J16-vote -> 16-pt TRN -> MP with zero LIVE MP READs, we never
               acked, and the caller restarted. The block receiver must run in Phase 4
               unconditionally - it tries both constellations itself. */
            /* SIPFAX: keep re-reading. The caller sets its acknowledge bit only after it
               has received OUR MP, so its MP' arrives strictly later than the first MP we
               decode. Reading once would mean never seeing the acknowledgement that gates
               our E. Re-run on a sliding window until MP' or E is seen. */
            unsigned int i;
            unsigned int j;
            if (p4bn >= P4_MAXIN) {           /* slide: keep the most recent ~3 s */
                int keep = 24000, off2 = p4bn - keep;
                for (j = 0; j < (unsigned int)keep; j++) p4b[j] = p4b[off2 + j];
                p4bn = keep;
            }
            for (i = 0; i < nb && p4bn < P4_MAXIN; i++) p4b[p4bn++] = samples[i];
            if (p4bn >= 12000) {
                /* One bounded step per audio frame. Running continuously is now free -
                   the work per frame is a few ms - and it finds the acknowledge sooner
                   than the old once-a-second full decode did. */
                int ca = 0, ac = 0, tr = 0, ak = 0, sh = 0, six = 0, nmp, nlreq = 0;
                unsigned int mk = 0;
                {   /* SIPFAX: this decoder starved the transmit path once (see
                       p4_block_step) - keep it measured so it cannot happen silently. */
                    static double worst = 0.0;
                    struct timespec ta, tb; double ms;
                    clock_gettime(CLOCK_MONOTONIC, &ta);
                    nmp = p4_block_step(p4b + (p4bn > 20000 ? p4bn-20000 : 0),
                                        p4bn > 20000 ? 20000 : p4bn,
                                        &ca, &ac, &tr, &ak, &sh, &mk, &six, s->peer_h,
                                        &nlreq);
                    clock_gettime(CLOCK_MONOTONIC, &tb);
                    ms = (tb.tv_sec-ta.tv_sec)*1e3 + (tb.tv_nsec-ta.tv_nsec)/1e6;
                    if (ms > worst) {
                        worst = ms;
                        { extern int v34_dbg; if (v34_dbg && ms > 8.0)
                            fprintf(stderr, "[p4blk] step %.1f ms - AUDIO FRAME IS 20 ms\n", ms); }
                    }
                }
                if (nmp) {
                    s->p4_mp_rate_ca = ca; s->p4_mp_rate_ac = ac;
                    s->p4_trellis = tr; s->p4_mp_mask = mk;
                    /* SIPFAX: the live path never captured the peer's non-linear-encoder
                       request. p4_mp_decode pulled out shape (bit 32) and the precoder
                       coefficients but not bit 31, so peer_nonlin stayed 0 on every live
                       call however the caller set it - which is why the precoder fired live
                       and the 9.7 encoder never did, off the same CRC gate and the same
                       rx->tx copy. Captures decode nonlin=1 offline while the live journal
                       shows zero "non-linear encoder ON" events for the same calls. */
                    if (nlreq) s->peer_nonlin = 1;
                    s->p4_mp_crcok = 1; s->p4_mp_rx = 1;
                    if (ak && !s->p4_mpp_rx) {
                        s->p4_mpp_rx = 1;
                        { extern int v34_dbg; if (v34_dbg)
                            fprintf(stderr, "[p4blk] CALLER MP-PRIME (ack=1) -> E\n"); }
                    }
                    { extern int v34_dbg; if (v34_dbg)
                        fprintf(stderr, "[p4blk] LIVE MP READ: %d frames %s ca=%d ac=%d trel=%d ack=%d\n",
                                nmp, six ? "16pt" : "4pt", ca*2400, ac*2400, tr, ak); }
                }
            }
        }
    }
    double FCarr = 24000.0/7.0*4.0/7.0, FS = 8000.0, BAUD = 24000.0/7.0;
    double SPS = FS/BAUD, dcinc = 2.0*M_PI*FCarr/FS, step = SPS/2.0;
    unsigned int k;
    if (!s->cma_init) {
        int i; for (i=0;i<CMANT;i++){s->cma_wi[i]=s->cma_wq[i]=s->cma_bufi[i]=s->cma_bufq[i]=0;}
        s->cma_wi[CMANT/2] = 1.0; s->cma_pow = 0.0; s->cma_warm = 0;
        for (i=0;i<8;i++){s->cma_hi[i]=s->cma_hq[i]=0;}
        s->cma_pos = 0.0; s->cma_tinc = 0.0;
        s->cma_gmi = s->cma_gmq = s->cma_gpi = s->cma_gpq = 0.0;
        s->cma_ncma = 1000; { char *e=getenv("SIPFAX_CMA_N"); if (e) s->cma_ncma = atoi(e); }
        { char *e = getenv("SIPFAX_RX_DBG"); if (e && atoi(e)) { extern int v34_dbg; v34_dbg = 1; } }
        { char *e=getenv("SIPFAX_CMA_TPHASE"); if (e) s->cma_residx = atof(e); }
        { char *e=getenv("SIPFAX_CMA_POLY"); if (e && atoi(e)==1) { cma_t1=5; cma_t2=23; } }
        s->cma_init = 1;
    }
    /* RRC matched filter (beta=0.15, span 12 sym) at 8 kHz: rejects the -2fc
       image after downconversion (the un-filtered image was in-band at the T/2
       rate and corrupted symbol decisions) and provides proper matched filtering. */
    static double mft[57]; static int mfinit = 0;
    if (!mfinit) {
        /* SIPFAX: the TRANSMIT pulse is square-root Nyquist with beta = 0.1 - v34gen.c
           builds it as build_sqr_nyquist_filter(..., beta=0.1) and the resulting table
           v34_rc_7_filter matches theoretical sqrt-RC(0.1) at 7 samples/symbol with
           correlation 1.0000, tap for tap. This matched filter was hand-rolled at 0.15,
           so it is not the conjugate of what the peer (or our own modulator) transmits.
           Note linmodem ALSO generates a properly designed receive matched filter for
           every rate/carrier pair - v34_rx_filters[] - which this live path ignores
           entirely; the block receiver's 3x-oversampled design is built around them.
           SIPFAX_RX_BETA overrides. */
        int t2; double beta = 0.10, sum2 = 0.0;
        { char *e = getenv("SIPFAX_RX_BETA"); if (e) beta = atof(e); }
        for (t2 = 0; t2 < 57; t2++) {
            double ti = (t2 - 28) / SPS, v;
            if (fabs(ti) < 1e-9) v = 1.0 - beta + 4.0*beta/M_PI;
            else {
                double dnm = M_PI*ti*(1.0 - (4.0*beta*ti)*(4.0*beta*ti));
                if (fabs(dnm) < 1e-9) dnm = 1e-9;
                v = (sin(M_PI*ti*(1.0-beta)) + 4.0*beta*ti*cos(M_PI*ti*(1.0+beta))) / dnm;
            }
            mft[t2] = v; sum2 += v*v;
        }
        sum2 = sqrt(sum2);
        for (t2 = 0; t2 < 57; t2++) mft[t2] /= sum2;
        mfinit = 1;
    }
    /* EXACT front-end: FC/FS = 12/49 and T/2 step = 7/6 are rational, so both the
       carrier phase and the resampler grid are computed with INTEGER bookkeeping.
       The old float accumulators drifted; each one-sample slip rotated symbols by
       2*pi*12/49 = 88.15 deg, which corrupted the symbol sequence downstream. */
    static double c49[49], s49[49]; static int lut49 = 0;
    if (!lut49) { int li; for (li = 0; li < 49; li++) { c49[li] = cos(2.0*M_PI*li/49.0); s49[li] = sin(2.0*M_PI*li/49.0); } lut49 = 1; }
    {   /* SIPFAX: 3x-OVERSAMPLED FRONT END (SIPFAX_RX3X, default on).
           The 8 kHz path below works at 2.3333 samples/symbol, so every T/2 output is
           interpolated on a grid only 2.33x the symbol rate, and its matched filter was
           hand-rolled for that grid. Upsampling 3x to 24 kHz makes the symbol rate divide
           exactly (7.0 samples/symbol), so T/2 is 3.5 samples and interpolation happens on
           a 7x grid instead - which is what p4_front() does, and that receiver reaches
           3.4-4.7% EVM on the caller's TRN. Downconversion is exact there too: 4/49 cycles
           per 24 kHz sample is precisely 1959.18 Hz. */
        static int rx3 = -1;
        if (rx3 < 0) { char *e = getenv("SIPFAX_RX3X"); rx3 = e ? atoi(e) : 1; }
        if (rx3) {
            static double h3[169]; static int h3init = 0; static double c49b[49], s49b[49];
            const int NH = 169;                    /* 24 symbols at 7 samples/symbol + 1 */
            unsigned int kk;
            if (!h3init) {
                int t2; double beta = 0.10, sum2 = 0.0, li;
                { char *e = getenv("SIPFAX_RX_BETA"); if (e) beta = atof(e); }
                for (t2 = 0; t2 < NH; t2++) {
                    double ti = (t2 - (NH-1)/2.0) / 7.0, v;
                    if (fabs(ti) < 1e-9) v = 1.0 - beta + 4.0*beta/M_PI;
                    else {
                        double dnm = M_PI*ti*(1.0 - (4.0*beta*ti)*(4.0*beta*ti));
                        if (fabs(dnm) < 1e-9) dnm = 1e-9;
                        v = (sin(M_PI*ti*(1.0-beta)) + 4.0*beta*ti*cos(M_PI*ti*(1.0+beta)))/dnm;
                    }
                    h3[t2] = v; sum2 += v*v;
                }
                sum2 = sqrt(sum2);
                for (t2 = 0; t2 < NH; t2++) h3[t2] /= sum2;
                /* SIPFAX: the 4/49 factor lives in the INDEX STEP (rx3_cphi += 4), so the
                   table itself must be plain cos/sin of 2*pi*t/49. Having it in both places
                   downconverted at 16/49 * 24000 = 7836 Hz instead of 1959.18 Hz, and the
                   receiver never locked (TRN EVM 64.8% against 4.5%). */
                for (t2 = 0; t2 < 49; t2++) { li = 2.0*M_PI*t2/49.0;
                                              c49b[t2] = cos(li); s49b[t2] = sin(li); }
                h3init = 1;
                { extern int v34_dbg; if (v34_dbg)
                    fprintf(stderr, "[rx3x] 3x front end: 24 kHz, 7.0 samples/symbol, "
                            "matched filter %d taps beta=%.2f\n", NH, beta); }
            }
            for (kk = 0; kk < nb; kk++) {
                int u;
                for (u = 0; u < 3; u++) {          /* zero-stuff 3x */
                    double v = (u == 0) ? (double)samples[kk]*3.0 : 0.0;
                    int ph = s->rx3_cphi;
                    s->rx3_bi[s->rx3_n & 511] =  v*c49b[ph];
                    s->rx3_bq[s->rx3_n & 511] = -v*s49b[ph];
                    s->rx3_cphi = (ph + 4) % 49;
                    {   /* matched filter over the complex baseband */
                        double ai = 0, aq = 0; int j;
                        for (j = 0; j < NH; j++) {
                            long idx = s->rx3_n - j;
                            if (idx < 0) break;
                            ai += h3[j]*s->rx3_bi[idx & 511];
                            aq += h3[j]*s->rx3_bq[idx & 511];
                        }
                        s->rx3_i[s->rx3_n & 511] = ai; s->rx3_q[s->rx3_n & 511] = aq;
                    }
                    s->rx3_n++;
                    /* emit T/2 outputs: 3.5 samples apart on the 7-per-symbol grid */
                    if (s->rx3_pos < (double)(s->rx3_n - 400)) s->rx3_pos = (double)(s->rx3_n - 400);
                    while (s->rx3_pos + 1 < (double)s->rx3_n) {
                        long i0 = (long)floor(s->rx3_pos);
                        double fr = s->rx3_pos - i0;
                        double vi = s->rx3_i[i0 & 511]*(1.0-fr) + s->rx3_i[(i0+1) & 511]*fr;
                        double vq = s->rx3_q[i0 & 511]*(1.0-fr) + s->rx3_q[(i0+1) & 511]*fr;
                        { static double pa=0; static long pn=0; extern int v34_dbg;
                          pa += vi*vi+vq*vq; if (++pn % 40000 == 0 && v34_dbg)
                            fprintf(stderr,"[rx3x] mean|v|^2 into t2sample = %.3e (n=%ld)\n",
                                    pa/40000, pn), pa=0; }
                        V34_cma_t2sample(s, vi, vq);
                        /* SIPFAX: the Gardner loop steers cma_pos (8 kHz units); this path
                           advances rx3_pos in 24 kHz units, so carry the correction over. */
                        s->rx3_pos += 3.5 + s->cma_tinc*3.0 + (s->cma_pos - s->rx3_last)*3.0;
                        s->rx3_last = s->cma_pos;
                    }
                }
            }
            return;
        }
    }
    for (k = 0; k < nb; k++) {
        double smp = (double)samples[k];
        double rbi = smp*c49[s->cma_cphi], rbq = -smp*s49[s->cma_cphi];
        double cbi, cbq; int mk;
        s->cma_cphi += 12; if (s->cma_cphi >= 49) s->cma_cphi -= 49;
        s->cma_mfi[s->cma_mfp & 63] = rbi; s->cma_mfq[s->cma_mfp & 63] = rbq; s->cma_mfp++;
        cbi = cbq = 0.0;
        for (mk = 0; mk < 57; mk++) {
            int ix = (s->cma_mfp - 1 - mk) & 63;
            cbi += mft[mk]*s->cma_mfi[ix]; cbq += mft[mk]*s->cma_mfq[ix];
        }
        /* SIPFAX: the 7:6 grid is no longer integer - the Gardner loop in t2sample steers
           cma_pos/cma_tinc, so the output instants have to be interpolated from a short
           history rather than from the previous sample alone. */
        s->cma_hi[s->cma_n & 7] = cbi; s->cma_hq[s->cma_n & 7] = cbq;
        for (;;) {
            long ip; double f, vi, vq;
            if (s->cma_pos < (double)(s->cma_n - 6))    /* fell behind the history: resync */
                s->cma_pos = (double)(s->cma_n - 6);
            ip = (long)floor(s->cma_pos);
            if (ip + 1 > s->cma_n) break;
            f = s->cma_pos - ip;
            vi = s->cma_hi[ip & 7]*(1.0-f) + s->cma_hi[(ip+1) & 7]*f;
            vq = s->cma_hq[ip & 7]*(1.0-f) + s->cma_hq[(ip+1) & 7]*f;
            { static double pa=0; static long pn=0; extern int v34_dbg;
              pa += vi*vi+vq*vq; if (++pn % 40000 == 0 && v34_dbg)
                fprintf(stderr,"[rx1x] mean|v|^2 into t2sample = %.3e (n=%ld)\n",
                        pa/40000, pn), pa=0; }
            V34_cma_t2sample(s, vi, vq);
            s->cma_m++;
            s->cma_pos += step + s->cma_tinc;
        }
        s->cma_pbi = cbi; s->cma_pbq = cbq; s->cma_n++;
    }
}
/* offline stream harness: SIPFAX_STREAM_FILE=path */
/* SIPFAX: data-mode bit sink for the offline harness. The live path wires
   serial_put_bit (lm.c), but V34_stream_decode_file had no sink at all, so the first
   decoded bit jumped through a NULL pointer. Counting them here is also how the
   data-mode receive chain gets validated against a captured call. */
static long g_databits = 0, g_dataones = 0, g_force_at = 0;
static FILE *g_databitf = 0;
static void stream_put_bit(void *o, int b)
{
    g_databits++; if (b) g_dataones++;
    if (g_databitf) fputc('0'+(b&1), g_databitf);
}

void V34_stream_decode_file(const char *path)
{
    extern int v34_dbg;
    V34State p; static V34DSPState rx; s16 buf[512]; FILE *f; int n, i;
    memset(&p, 0, sizeof(p)); memset(&rx, 0, sizeof(rx));
    p.S = V34_S3429; p.R = 33600; p.conv_nb_states = 16; p.use_high_carrier = 1; p.calling = 0;
    { extern void dsp_init(void); dsp_init(); } V34_static_init();
    rx.S = p.S; rx.use_high_carrier = 1;
    rx.put_bit = stream_put_bit; rx.opaque = 0;
    {   /* SIPFAX: SIPFAX_FORCE_DATA=<rate> skips the handshake and drops the receiver
           straight into data mode, so a known-good modulated signal can be fed through
           the real receive chain and scored with the same trellis metric. */
        char *fd = getenv("SIPFAX_FORCE_DATA");
        if (fd) {
            rx.p4_mp_rx = 1; rx.p4_mp_rate_ca = atoi(fd) / 2400;
            rx.p4_trellis = 2; rx.p4_e_rx = 1;
            /* let the CMA acquire timing and taps normally - forcing cma_phase=2 would
               skip acquisition and test nothing but the decoder. SIPFAX_FORCE_DATA_AT
               defers the data-mode switch by N seconds so the file can carry 4-point TRN
               first (which CMA CAN acquire) and data after, mirroring the real receiver. */
            { char *hard = getenv("SIPFAX_FORCE_HARD");
              if (hard && atoi(hard)) { rx.srx_locked = 1; rx.p4_mode = 2; rx.cma_phase = 2; } }
            { char *at = getenv("SIPFAX_FORCE_DATA_AT");
              if (at) { rx.p4_e_rx = 0; g_force_at = (long)(atof(at)*8000.0);
                        fprintf(stderr, "[stream] data mode deferred to t=%.2fs\n", atof(at)); } }
            fprintf(stderr, "[stream] FORCE_DATA: entering data mode at R=%d\n", atoi(fd));
        }
    }
    { char *db = getenv("SIPFAX_DATABITS"); if (db) g_databitf = fopen(db, "w"); }
    v34_dbg = 1;
    f = fopen(path, "rb"); if (!f) { perror(path); return; }
    /* SIPFAX: this dump used to be unconditional - one symbol per line for the whole
       decode. A single long sweep run wrote 16 GB and filled the VM's disk, which then
       failed every subsequent build and test in a way that looked like a code fault.
       Opt in with SIPFAX_SOFTDUMP=<path>. */
    { char*e=getenv("SIPFAX_SOFTDUMP"); if(e) cma_dumpf = fopen(e,"w"); }
    { char*e=getenv("SIPFAX_P4BITS"); if(e) p4bitf=fopen(e,"w"); }
    { char *t2 = getenv("SIPFAX_T2DUMP"); if (t2) cma_t2df = fopen(t2, "w"); } fprintf(stderr, "[stream] decoding %s via V34_demod_cma\n", path);
    { long fed = 0;
      while ((n = fread(buf, 2, 512, f)) > 0) {
          if (g_force_at > 0 && fed >= g_force_at && !rx.p4_e_rx) {
              rx.p4_e_rx = 1;
              fprintf(stderr, "[stream] switching to data mode at t=%.2fs (cma_phase=%d)\n",
                      fed/8000.0, rx.cma_phase);
          }
          V34_demod_cma(&rx, buf, n); fed += n;
      } }
    fclose(f);
    if(cma_dumpf){fclose(cma_dumpf);cma_dumpf=0;} if(cma_t2df){fclose(cma_t2df);cma_t2df=0;} if(p4bitf){fclose(p4bitf);p4bitf=0;} fprintf(stderr, "[stream] END: J_received=%d locked=%d rot=%d cma_cnt=%d\n", rx.J_received, rx.srx_locked, rx.srx_rot, rx.cma_cnt);
    if (g_databitf) { fclose(g_databitf); g_databitf = 0; }
    fprintf(stderr, "[data] decoded %ld bits (%ld ones, %.1f%%) from %ld symbols\n",
            g_databits, g_dataones, g_databits ? 100.0*g_dataones/g_databits : 0.0, rx.data_n);
    { extern long g_ss_n, g_ss_tied; extern double g_ss_spread;
      if (g_ss_n) fprintf(stderr,
        "[acs] accumulated state metrics: mean spread %.1f over 64 states; "
        "mean %.1f states share the minimum\n",
        g_ss_spread/g_ss_n, (double)g_ss_tied/g_ss_n); }
    { extern long g_et_n, g_et_zero; extern double g_et_spread;
      if (g_et_n) fprintf(stderr,
        "[acs] branch-metric spread (max-min over the 32 branches): mean %.1f; "
        "ALL BRANCHES TIED in %.1f%% of %ld symbols\n",
        g_et_spread/g_et_n, 100.0*g_et_zero/g_et_n, g_et_n); }
    { extern long g_tr_argmin, g_tr_tot, g_tr_rank, g_tr_gap, g_tr_inf;
      if (g_tr_tot) fprintf(stderr,
        "[acs] true next state was the argmin in %.1f%% of %ld; mean rank %.1f of 64; "
        "mean metric gap %.0f; unreachable %.1f%%\n",
        100.0*g_tr_argmin/g_tr_tot, g_tr_tot, (double)g_tr_rank/g_tr_tot,
        (double)g_tr_gap/(g_tr_tot - g_tr_inf + 1), 100.0*g_tr_inf/g_tr_tot); }
    { extern long g_cs_tot, g_cs_ok, g_cs_all4;
      if (g_cs_tot) fprintf(stderr, "[coset] trellis picked the nearest-point coset on "
              "%.2f%% of coordinates (%ld), all 4 correct on %.2f%% of 4D symbols\n",
              100.0*g_cs_ok/g_cs_tot, g_cs_tot, 400.0*g_cs_all4/g_cs_tot); }
    { extern long g_y0same, g_y0tot;
      if (g_y0tot) fprintf(stderr, "[data] computed Y0 == survivor LSB in %.1f%% of %ld\n",
                           100.0*g_y0same/g_y0tot, g_y0tot); }
    { extern long g_dh[8], g_dmiss, g_dn; int q9;
      fprintf(stderr, "[data] decided |coord| max-of-pair histogram (1,3,5,7,9,11,13,>13):");
      for (q9 = 0; q9 < 8; q9++) fprintf(stderr, " %ld", g_dh[q9]);
      fprintf(stderr, "\n[data] table misses: %ld / %ld = %.2f%%\n",
              g_dmiss, g_dn, g_dn ? 100.0*g_dmiss/g_dn : 0.0); }
    { extern long g_moob, g_mtot, g_mmax;
      fprintf(stderr, "[data] ring index out of constellation: %ld / %ld = %.2f%%  (max m=%ld, M=%d)\n",
              g_moob, g_mtot, g_mtot ? 100.0*g_moob/g_mtot : 0.0, g_mmax, rx.M); }
    fprintf(stderr, "[data] mean trellis metric = %.1f over %ld 4D symbols\n",
            rx.data_mse_n ? rx.data_mse_acc/rx.data_mse_n : 0.0, rx.data_mse_n);
}


void V34_datacfg_dump(void)
{
    extern int v34_dbg;
    V34State p; static V34DSPState s;
    char *e; int R = 16800, i;
    memset(&p, 0, sizeof(p)); memset(&s, 0, sizeof(s));
    e = getenv("SIPFAX_DATA_R"); if (e) R = atoi(e);
    p.S = V34_S3429; p.R = R; p.use_high_carrier = 1; p.calling = 0;
    p.conv_nb_states = 64; { char *se=getenv("SIPFAX_DL_SHAPE"); p.expanded_shape = se?atoi(se):0; } { char *nl=getenv("SIPFAX_DL_NONLIN"); p.use_non_linear = nl?atoi(nl):0; } p.use_aux_channel = 0;
    { extern void dsp_init(void); dsp_init(); }
    V34_static_init();
    V34_init_low(&s, &p, 0);
    fprintf(stderr, "[datacfg] R=%d S=%d symrate=%.1f carrier=%.1f\n", s.R, s.S, s.symbol_rate, s.carrier_freq);
    fprintf(stderr, "[datacfg] J=%d P=%d N=%d b=%d r=%d W=%d K=%d q=%d M=%d L=%d conv_states=%d\n",
            s.J, s.P, s.N, s.b, s.r, s.W, s.K, s.q, s.M, s.L, s.conv_nb_states);
    { FILE *f = fopen("/tmp/constel.txt", "w");
      for (i = 0; i < s.L; i++) fprintf(f, "%d %d\n", s.constellation[i][0], s.constellation[i][1]);
      fclose(f);
      fprintf(stderr, "[datacfg] dumped %d constellation points -> /tmp/constel.txt\n", s.L); }
}


static V34DSPState *g_rx_state;
/* SIPFAX: the decoder's input contract is sample = lattice_coordinate * 128 -
   tcm_decision() reads a coordinate back as (sample>>8)*2+1. The loopback used to hand
   over the raw +-1..+-7 coordinates, 128x too small; that still scored 100% only because
   the channel was noiseless. SIPFAX_DL_SCALE sets the scale (default 128, the real one)
   and SIPFAX_DL_SNR adds white noise in dB relative to the mean symbol power, so the
   trellis decoder can be characterised the way a real receiver will drive it. */
static double dl_scale = -1, dl_sigma = 0;
static unsigned int dl_rng = 12345;
static double dl_gauss(void)
{   /* Box-Muller from a cheap LCG - deterministic across runs */
    double u1, u2;
    dl_rng = dl_rng*1103515245u + 12345u; u1 = ((dl_rng>>8)&0xffffff)/16777216.0;
    dl_rng = dl_rng*1103515245u + 12345u; u2 = ((dl_rng>>8)&0xffffff)/16777216.0;
    if (u1 < 1e-12) u1 = 1e-12;
    return sqrt(-2.0*log(u1))*cos(2*M_PI*u2);
}
static FILE *g_symdumpf = 0;
static void dataloop_symsink(int si, int sq)
{
    extern void baseband_decode_pub(V34DSPState*,int,int);
    {   /* SIPFAX: dump the true data-mode symbols so the FULL chain can be closed
           offline: encoder -> these symbols -> modulate to 8 kHz audio -> our own
           receiver -> trellis metric. That isolates the receive chain from the caller's
           signal, which no measurement so far has done. */
        static int opened = 0;
        if (!opened) { char *e = getenv("SIPFAX_SYMDUMP"); opened = 1;
                       if (e) g_symdumpf = fopen(e, "w"); }
        if (g_symdumpf) fprintf(g_symdumpf, "%d %d\n", si, sq);
    }
    if (dl_scale < 0) {
        char *e = getenv("SIPFAX_DL_SCALE"); dl_scale = e ? atof(e) : 1.0;   /* put_sym already carries *128 */
        e = getenv("SIPFAX_DL_SNR");
        if (e) {
            /* put_sym already carries coordinate*128 (verified on the wire:
               coordinates (1,5) arrive as (128,640)), so the per-component rms is
               ~3.5*128 at R=16800. */
            double ps = 2.0*(3.5*128.0)*(3.5*128.0)*dl_scale*dl_scale;
            dl_sigma = sqrt(ps/2.0/pow(10.0, atof(e)/10.0));
        }
    }
    {
        {   /* SIPFAX: distribution of OUR OWN transmitted data-mode symbols, so the
               caller's can be compared like for like (kurtosis of |x|^2 and the
               4th-moment line, the two statistics that distinguish shaping). */
            static double s2 = 0, s4 = 0, m4r = 0, m4i = 0; static long nn = 0;
            double pw = (double)si*si + (double)sq*sq;
            double r2 = (double)si*si - (double)sq*sq, i2 = 2.0*si*sq;
            double r4 = r2*r2 - i2*i2, i4 = 2.0*r2*i2;
            s2 += pw; s4 += pw*pw; m4r += r4; m4i += i4; nn++;
            if (nn == 20000) {
                double mp = s2/nn;
                fprintf(stderr, "[txdist] kurtosis %.3f  4th-moment %.4f\n",
                        (s4/nn)/(mp*mp), sqrt(m4r*m4r + m4i*m4i)/nn/(s4/nn));
                fflush(stderr);
            }
        }
        double a = si*dl_scale, b = sq*dl_scale;
        { static int z=-1; if(z<0){char*e=getenv("SIPFAX_DL_ZERO");z=e?atoi(e):0;}
          if(z){ a=0; b=0; } }
        if (dl_sigma > 0) { a += dl_sigma*dl_gauss(); b += dl_sigma*dl_gauss(); }
        baseband_decode_pub(g_rx_state, (int)lrint(a), (int)lrint(b));
    }
}
static unsigned int g_prbs;
static u8 g_txb[200000], g_rxb[200000]; static int g_txn, g_rxn;
static int dataloop_src(void *o) { int b = ((g_prbs>>21)^(g_prbs>>20))&1; g_prbs=((g_prbs<<1)|b)&0x7fffff; if(!g_prbs)g_prbs=1; if(g_txn<200000)g_txb[g_txn++]=(u8)b; return b; }
static void dataloop_sink(void *o, int b) { if(g_rxn<200000)g_rxb[g_rxn++]=(u8)b; }
void V34_dataloop_test(void)
{
    static V34DSPState tx, rx; V34State pt, pr; int i, R=16800; char *e;
    e=getenv("SIPFAX_DATA_R"); if(e) R=atoi(e);
    /* SIPFAX: this harness took the rate ONLY from SIPFAX_DATA_R and silently ignored
       SIPFAX_FORCE_DATA, which every other path uses. Passing SIPFAX_FORCE_DATA here ran the
       default 16800 while appearing to sweep rates - it produced byte-identical bit counts at
       9600/16800/21600/28800 and a whole session of "both rates" results that were one rate
       measured twice. Accept both names. */
    else { e=getenv("SIPFAX_FORCE_DATA"); if(e) R=atoi(e); }
    fprintf(stderr,"[dataloop] rate R=%d\n", R);
    memset(&tx,0,sizeof(tx)); memset(&rx,0,sizeof(rx)); memset(&pt,0,sizeof(pt)); memset(&pr,0,sizeof(pr));
    { extern void dsp_init(void); dsp_init(); } V34_static_init();
    pt.S=V34_S3429; pt.R=R; pt.use_high_carrier=1; pt.calling=1; pt.conv_nb_states=64;
    { char *se=getenv("SIPFAX_DL_SHAPE"); pt.expanded_shape = se?atoi(se):0; }
    { char *nl=getenv("SIPFAX_DL_NONLIN"); pt.use_non_linear = nl?atoi(nl):0; }
    {   /* SIPFAX: enable the TRANSMIT precoder in the loopback so the receive side can be
           developed offline. linmodem already implements 9.6.2 on the transmit side, so
           feeding it non-zero coefficients and watching the bit match collapse proves the
           receiver has no inverse - and is the harness that verifies one once written.
           SIPFAX_DL_H="h1r,h1i,h2r,h2i,h3r,h3i" in 14-bit fixed point (16384 = 1.0); the
           caller's own MP values are 13888,26551,-3985,7104,-12417,18847. */
        char *he = getenv("SIPFAX_DL_H");
        memset(pt.h, 0, sizeof(pt.h));
        if (he) {
            int vv[6]; int k2 = 0;
            char hbuf[128], *tok;
            for (k2 = 0; k2 < 6; k2++) vv[k2] = 0;
            strncpy(hbuf, he, sizeof(hbuf)-1); hbuf[sizeof(hbuf)-1] = 0;
            k2 = 0; tok = strtok(hbuf, ",");
            while (tok && k2 < 6) { vv[k2++] = atoi(tok); tok = strtok(NULL, ","); }
            for (k2 = 0; k2 < 3; k2++) { pt.h[k2][0] = (s16)vv[2*k2]; pt.h[k2][1] = (s16)vv[2*k2+1]; }
            fprintf(stderr, "[dataloop] TX precoder ON: h1=(%d,%d) h2=(%d,%d) h3=(%d,%d)\n",
                    pt.h[0][0],pt.h[0][1],pt.h[1][0],pt.h[1][1],pt.h[2][0],pt.h[2][1]);
        }
    }
    memcpy(&pr,&pt,sizeof(pr)); pr.calling=0;
    V34_init_low(&tx,&pt,1); V34_init_low(&rx,&pr,0);
    {   /* SIPFAX: the receiver inverts the precoder exactly when the transmitter uses it */
        /* SIPFAX: OFF by default - the inverse below is NOT yet correct. With it off the
           loopback still reaches 97.9% through a precoding transmitter; with it on,
           either sign, it drops to 50.3%. See the commit message. SIPFAX_RX_PRECODE=1
           enables it for experiments. */
        char *e3 = getenv("SIPFAX_RX_PRECODE");
        rx.rx_precode = (e3 && atoi(e3)) ? 1 : 0;
        if (rx.rx_precode) fprintf(stderr, "[dataloop] RX precoder inverse ON (experimental)\n");
    }
    tx.get_bit=dataloop_src; tx.opaque=0; rx.put_bit=dataloop_sink; rx.opaque=0;
    g_prbs=1; g_txn=0; g_rxn=0; g_rx_state=&rx;
    { extern FILE *gt_f; char *ge=getenv("SIPFAX_GTDUMP"); if(ge) gt_f=fopen(ge,"w"); }
    { extern void (*g_symtap)(int,int); g_symtap=dataloop_symsink;
      for(i=0;i<4000;i++) encode_mapping_frame(&tx);
      g_symtap=0; { extern FILE *gt_f; if(gt_f){fclose(gt_f);gt_f=0;} } }
    fprintf(stderr,"[dataloop] R=%d tx_bits=%d rx_bits=%d  mean-metric=%.1f\n", R, g_txn, g_rxn,
            rx.data_mse_n ? rx.data_mse_acc/rx.data_mse_n : 0.0);
    { FILE*ft=fopen("/tmp/dl_tx.txt","w"); for(i=0;i<g_txn;i++)fputc('0'+g_txb[i],ft); fclose(ft);
      FILE*fr=fopen("/tmp/dl_rx.txt","w"); for(i=0;i<g_rxn;i++)fputc('0'+g_rxb[i],fr); fclose(fr); }
    { int best=-1,bestlag=0,lag;
      for(lag=0;lag<600;lag++){ int mt=0,cn=0;
        for(i=0;i+lag<g_rxn && i<g_txn && i<3000;i++){ if(g_txb[i]==g_rxb[i+lag])mt++; cn++; }
        if(cn>1000 && mt>best){best=mt;bestlag=lag;} }
      { int mt=0,cn=0; for(i=300;i+bestlag<g_rxn && i<g_txn;i++){ if(g_txb[i]==g_rxb[i+bestlag])mt++; cn++; }
        fprintf(stderr,"[dataloop] best lag=%d: %d/%d = %.1f%% bit match (100%%=DSP round-trips)\n", bestlag, mt, cn, 100.0*mt/(cn?cn:1));
    {   extern long g_sym, g_v0hn; extern int g_v0h[], g_v0base[], g_v0lock, g_v0ph;
        int ph,k,tot=0; int sc[480]; int b1=-1,b2=-1,p1=0;
        for (k=0;k<480;k++) tot += g_v0h[k];
        for (ph=0; ph<480; ph++) { int q=0;
            for (k=0;k<480;k++) if (g_v0base[(k+ph)%480]) q += g_v0h[k];
            sc[ph]=q; if (q>b1){b2=b1;b1=q;p1=ph;} else if (q>b2) b2=q; }
        fprintf(stderr,"[v0dbg] g_sym=%ld est_n=%ld ones=%d lock=%d ph=%d | best=%d@%d "
                "runnerup=%d\n", g_sym, g_v0hn, tot, g_v0lock, g_v0ph, b1, p1, b2); }
    { extern long g_az_n,g_az_ok,g_az_half;
      if (g_az_n) fprintf(stderr,"[acs] survivor took the ZERO-cost branch on %.2f%% of "
          "symbols; its predecessor was in the zero branch's reachable half on %.2f%%\n",
          100.0*g_az_ok/g_az_n, 100.0*g_az_half/g_az_n); }
    { extern long g_z_n,g_z_min0,g_z_cnt,g_z_hist[8]; extern double g_z_minv; int zq;
      if (g_z_n) { fprintf(stderr,"[zero] min branch metric == 0 on %.2f%% of symbols; "
          "mean min %.1f; mean #zero-branches %.3f; hist(#zeros 0..7+):",
          100.0*g_z_min0/g_z_n, g_z_minv/g_z_n, (double)g_z_cnt/g_z_n);
        for (zq=0;zq<8;zq++) fprintf(stderr," %ld",g_z_hist[zq]); fprintf(stderr,"\n"); } }
    { extern long g_cs_tot, g_cs_ok, g_cs_all4;
      if (g_cs_tot) fprintf(stderr, "[coset] NOISE-FREE: trellis picked the nearest-point "
              "coset on %.2f%% of coordinates (%ld), all 4 correct on %.2f%% of 4D symbols\n",
              100.0*g_cs_ok/g_cs_tot, g_cs_tot, 400.0*g_cs_all4/g_cs_tot); } } }
}


void V34_mptest(void)
{
    static V34DSPState s; V34State p;
    memset(&s, 0, sizeof(s)); memset(&p, 0, sizeof(p));
    { extern void dsp_init(void); dsp_init(); }
    V34_static_init();
    p.S = V34_S3429; p.R = 16800; p.use_high_carrier = 1; p.calling = 0;
    p.conv_nb_states = 64;
    V34_init_low(&s, &p, 0);
    /* simulate the caller having proposed ca=16800 (7), ac=9600 (4), 64-state (2) */
    s.p4_mp_rx = 1; s.p4_mp_rate_ca = 7; s.p4_mp_rate_ac = 4;
    s.p4_trellis = 2; s.p4_mp_mask = 0x0fff;
    V34_send_MP(&s, 1, 0);      /* MP  */
    V34_send_MP(&s, 1, 1);      /* MP' */
    s.p4_mp_rx = 0;             /* pre-negotiation fallback */
    V34_send_MP(&s, 1, 0);
    fprintf(stderr, "[mptest] 3 frames dumped\n");
}

/* SIPFAX_P4BLOCK=<file.s16> : run the block receiver over a raw 8 kHz capture and report,
   for each window, the 4-point and 16-point TRN scores. Acceptance: the 16-point Phase-4
   TRN window should score ~0.99, matching research/v34-rx. */
/* SIPFAX: drive the amortised block receiver exactly as the audio callback does - one
   160-sample frame at a time - and report the WORST per-call latency. That number is the
   whole point of the amortisation: it must stay far below the 20 ms frame period, because
   when it did not, our transmit carrier lost ~100 ms every second and no peer would ack. */
void V34_p4step_test(void)
{
    static short buf[P4_MAXIN];
    static short frame[160];
    char *fn = getenv("SIPFAX_P4STEP");
    double t0 = 0.0, worst = 0.0, total = 0.0;
    int n, i, bn = 0, calls = 0, reads = 0, over = 0;
    long fed = 0;
    FILE *f;
    char *e;
    if (!fn) return;
    if ((e = getenv("SIPFAX_P4S_T0")) != 0) t0 = atof(e);
    f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "[p4step] cannot open %s\n", fn); return; }
    fseek(f, (long)(t0*8000.0)*2, SEEK_SET);
    p4_block_reset();
    fprintf(stderr, "[p4step] %s from t=%.1fs, 160-sample frames\n", fn, t0);
    while ((n = (int)fread(frame, 2, 160, f)) > 0) {
        if (bn >= P4_MAXIN) {
            int keep = 24000, off2 = bn - keep;
            for (i = 0; i < keep; i++) buf[i] = buf[off2 + i];
            bn = keep;
        }
        for (i = 0; i < n && bn < P4_MAXIN; i++) { buf[bn++] = frame[i]; fed++; }
        if (bn >= 12000) {
            int ca = 0, ac = 0, tr = 0, ak = 0, sh = 0, six = 0, nmp;
            unsigned int mk = 0; short hh[6];
            struct timespec ta, tb;
            double ms;
            clock_gettime(CLOCK_MONOTONIC, &ta);
            nmp = p4_block_step(buf + (bn > 20000 ? bn-20000 : 0), bn > 20000 ? 20000 : bn,
                                &ca, &ac, &tr, &ak, &sh, &mk, &six, hh, NULL);
            clock_gettime(CLOCK_MONOTONIC, &tb);
            ms = (tb.tv_sec-ta.tv_sec)*1e3 + (tb.tv_nsec-ta.tv_nsec)/1e6;
            total += ms; calls++;
            if (ms > worst) worst = ms;
            if (ms > 20.0) over++;
            if (nmp) {
                reads++;
                { static int shown=0;
                  if (!shown) { shown=1;
                    fprintf(stderr,"[p4] peer precoder h (LSB-first, /16384): "
                            "h1=(%+.4f,%+.4f) h2=(%+.4f,%+.4f) h3=(%+.4f,%+.4f)\n",
                            hh[0]/16384.0, hh[1]/16384.0, hh[2]/16384.0,
                            hh[3]/16384.0, hh[4]/16384.0, hh[5]/16384.0); } }
                fprintf(stderr, "  t=%6.2fs  MP READ: %d frames %s ca=%d ac=%d trel=%d ack=%d\n",
                        t0 + (double)fed/8000.0, nmp, six ? "16pt" : "4pt",
                        ca*2400, ac*2400, tr, ak);
            }
        }
    }
    fclose(f);
    fprintf(stderr, "[p4step] %d steps, %d MP reads | worst %.2f ms, mean %.2f ms, over-20ms %d\n",
            calls, reads, worst, calls ? total/calls : 0.0, over);
    fprintf(stderr, "[p4step] %s\n", worst < 20.0 ?
            "OK - every step fits inside one audio frame" : "FAIL - a step exceeds the frame period");
}

void V34_p4block_test(void)
{
    static short x[P4_MAXIN];
    static double zi[P4_MAXZ], zq[P4_MAXZ], si[P4_MAXSY], sq[P4_MAXSY];
    char *fn = getenv("SIPFAX_P4BLOCK");
    double t0 = 0, wsec = 1.2, tend = 60.0;
    char *e;
    FILE *f;
    if (!fn) return;
    if ((e = getenv("SIPFAX_P4B_T0")) != 0) t0 = atof(e);
    if ((e = getenv("SIPFAX_P4B_T1")) != 0) tend = atof(e);
    if ((e = getenv("SIPFAX_P4B_W")) != 0) wsec = atof(e);
    f = fopen(fn, "rb");
    if (!f) { fprintf(stderr, "[p4blk] cannot open %s\n", fn); return; }
    fprintf(stderr, "[p4blk] window  4pt-TRN  16pt-TRN   (>=0.90 = decoded)\n");
    for (; t0 < tend; t0 += wsec) {
        int n, nz, ns;
        double off, s4, s16;
        int sixteen_mp = 1;
        if (fseek(f, (long)(t0*8000.0)*2, SEEK_SET) != 0) break;
        n = (int)fread(x, 2, (size_t)(wsec*8000.0), f);
        if (n < 4000) break;
        {   /* skip silence */
            double p = 0; int i;
            for (i = 0; i < n; i++) p += (double)x[i]*x[i];
            if (sqrt(p/n) < 250) continue;
        }
        nz = p4_front(x, n, zi, zq);
        off = p4_timing(zi, zq, nz);
        {   int nc = 6, nd = 10; char *ev;   /* 16-point needs the extra passes:
                                               2/3 scores 0.71, 4/6 gives 0.975, 6/10 gives 0.985 */
            if ((ev = getenv("SIPFAX_P4B_NCMA")) != 0) nc = atoi(ev);
            if ((ev = getenv("SIPFAX_P4B_NDD")) != 0) nd = atoi(ev);
            ns = p4_equalize(zi, zq, nz, off, 0, nc, nd, 1, si, sq);
            s4  = p4_trn_score(si+200, sq+200, ns-200 > 0 ? ns-200 : 0, 0, V34_GPC);
            ns = p4_equalize(zi, zq, nz, off, 1, nc, nd, 1, si, sq);
            s16 = p4_trn_score(si+200, sq+200, ns-200 > 0 ? ns-200 : 0, 1, V34_GPC);
        }
        if (0) ns = p4_equalize(zi, zq, nz, off, 0, 2, 3, 1, si, sq);
        {   int ca = 0, ac = 0, tr = 0, ak = 0, sh = 0, nmp;
            unsigned int mk = 0;
            nmp = p4_mp_decode(si+200, sq+200, ns-200 > 0 ? ns-200 : 0, 1, V34_GPC,
                               &ca, &ac, &tr, &ak, &sh, &mk, NULL, NULL);
            if (!nmp) {
                int ns4 = p4_equalize(zi, zq, nz, off, 0, 6, 10, 1, si, sq);
                nmp = p4_mp_decode(si+200, sq+200, ns4-200 > 0 ? ns4-200 : 0, 0, V34_GPC,
                                   &ca, &ac, &tr, &ak, &sh, &mk, NULL, NULL);
                if (nmp) sixteen_mp = 0;
            } else sixteen_mp = 1;
            fprintf(stderr, "[p4blk] t=%5.1f   %.3f    %.3f%s", t0, s4, s16,
                    (s16 >= 0.90) ? "   <== 16-POINT TRN" : (s4 >= 0.90 ? "   <== 4-point TRN" : ""));
            if (nmp) fprintf(stderr, "   MP: %d frames %s ca=%d ac=%d trel=%d shape=%d ACK=%d mask=0x%04x",
                             nmp, sixteen_mp ? "16pt" : "4pt", ca*2400, ac*2400, tr, sh, ak, mk);
            fprintf(stderr, "\n");
        }
    }
    fclose(f);
}

/* init the V34 constants. Should be launched once */
void V34_static_init(void)
{
    V34eq_init();
}


/* V.34 Phase-2 answer state machine (v34_phase2.c) */
extern void *v34_phase2_new(void);
extern int v34_phase2_run(void *p, s16 *out, s16 *in, int n);
extern int v34_phase2_symrate(void *p);
extern void v34_phase2_free(void *p);

void V34_init(struct V34State *s, int calling)
{
    /* Fixed V.34 params for first bring-up (S=2400 baud, R=19200, 16-state).
       TODO: derive S/R from the V.34 phase-2 INFO/probing negotiation. */
    s->S = V34_S2400;
    s->R = 19200;
    /* SIPFAX: our MP advertises constellation shaping (shape=1, the slmodem-compatible
       value) and the caller obliges - but this was hard-coded 0, so the constellation
       builder produced L=48 while the caller transmits L=56 (expanded shaping scales the
       set by 1.25: M = rint(1.25*2^(K/8)) instead of ceil(2^(K/8))). Demapping a 56-point
       signal against a 48-point set is why the captured data-mode symbols fit no lattice
       at any size (measured lattice error 0.578 = uniform, against 0.076 for a matched
       control). The loopback round-trips at 100% with shaping on, so the codec supports
       it. SIPFAX_SHAPE overrides. */
    { char *e = getenv("SIPFAX_SHAPE"); s->expanded_shape = e ? atoi(e) : 1; }
    s->conv_nb_states = 16;
    s->use_non_linear = 0;
    s->use_high_carrier = 1;
    s->use_aux_channel = 0;
    memset(s->h, 0, sizeof(s->h));

    /* TX uses our role's carrier; RX must demodulate the peer's carrier. */
    s->calling = calling;
    V34_mod_init(&s->v34_tx, s);
    s->calling = !calling;
    V34_demod_init(&s->v34_rx, s);
    s->calling = calling;

    /* SIPfax answers: run the V.34 Phase-2 negotiation before Phase 3 */
    s->phase2 = 0; s->phase2_active = 0;
    if (!calling) { s->phase2 = v34_phase2_new(); s->phase2_active = 1; }
}

int V34_process(struct V34State *s, s16 *output, s16 *input, int nb_samples)
{
    if (s->phase2_active) {
        int r = v34_phase2_run(s->phase2, output, input, nb_samples);
        if (r == 1) {
            int sr = v34_phase2_symrate(s->phase2);
            if (sr >= 0) s->S = sr;   /* 0..5 == V34_S2400..V34_S3429 */
            /* hand off to Phase 3, preserving the serial data callbacks */
            get_bit_func gb = s->v34_tx.get_bit; void *go = s->v34_tx.opaque;
            put_bit_func pb = s->v34_rx.put_bit; void *po = s->v34_rx.opaque;
            s->calling = 0; V34_mod_init(&s->v34_tx, s);
            s->v34_tx.get_bit = gb; s->v34_tx.opaque = go;
            s->calling = 1; V34_demod_init(&s->v34_rx, s); s->calling = 0;
            s->v34_rx.put_bit = pb; s->v34_rx.opaque = po;
            v34_phase2_free(s->phase2); s->phase2 = 0; s->phase2_active = 0; s->p3n = 0; s->p3x1 = 0; s->p3go = 0;
            { char *rp = getenv("SIPFAX_P3_REPLAY"); s->p3rep = 0; s->p3rep_len = 0; s->p3rep_ptr = 0;
              if (rp) { FILE *rf = fopen(rp, "rb"); if (rf) { fseek(rf,0,SEEK_END); long sz=ftell(rf); fseek(rf,0,SEEK_SET);
                  s->p3rep = malloc(sz); s->p3rep_len = fread(s->p3rep, 2, sz/2, rf); fclose(rf);
                  fprintf(stderr, "[v34p3] REPLAY mode: %ld samples from %s\n", s->p3rep_len, rp); fflush(stderr); } } }
        } else if (r == -1) {
            return 1;   /* negotiation failed -> hang up */
        }
        return 0;
    }
    { static int rxcma = -1; if (rxcma < 0) { char *e = getenv("SIPFAX_RX_CMA"); rxcma = e ? atoi(e) : 0; }
      if (rxcma) { extern void V34_demod_cma(V34DSPState*, const s16*, unsigned int); V34_demod_cma(&s->v34_rx, input, nb_samples); }
      else V34_demod(&s->v34_rx, input, nb_samples); }
    {   /* SIPFAX: flush the stale J HERE, before this block's V34_mod sees J_received
           and queues S into the same tx buffer. While muted (p3go && !J_received) the
           modulator kept cycling J through the queue, and the leftover ~100 symbols
           drained AUDIBLY at unmute - the Phase-4 burst opened with stale J and a timed
           mute either under- or over-shot (its first version swallowed S entirely; its
           second still shaved 12 ms off S's head). The exact fix: tx_buf is both the
           symbol queue and the tx-filter history, so zeroing it makes the leftover J
           exactly silence and S emerges whole, on time, with no arithmetic. S cannot be
           in the buffer yet - it is queued only after J_received bridges, below. */
        extern int sipfax_jmute;
        if (sipfax_jmute < 0 && s->v34_rx.J_received && !s->v34_tx.J_received) {
            memset(s->v34_tx.tx_buf, 0, sizeof(s->v34_tx.tx_buf));
            sipfax_jmute = 0;
            { extern int v34_dbg; if (v34_dbg)
                fprintf(stderr, "[p4] stale J zeroed in tx queue (%d syms) - S goes out whole\n",
                        s->v34_tx.tx_buf_size); }
        }
    }
    s->v34_tx.J_received = s->v34_rx.J_received;   /* bridge caller-J -> TX WAIT_J */
    /* SIPFAX: obey the caller's J constellation command (10.1.3.3): 0x0D91 means OUR
       Phase-4 TRN, MP, MP' and E must all be 16-point. slmodem - which this caller
       acknowledges in 0.5 s - complies; ignoring it left the caller training against a
       4-point TRN it was told would be 16-point, and it never read our MP at all.
       One flag drives all three signals; SIPFAX_J16_OBEY=0 restores the old behaviour. */
    {   /* SIPFAX: this used to fire only for rx_j16==1, leaving mp_16point at whatever
           SIPFAX_MP16/SIPFAX_MP_SLCOMPAT had forced when the caller commanded 4-point -
           so we answered J4POINTS with a 16-point MP. That mattered the moment Phase 2
           began completing properly: with a correct INFO exchange this caller asks for
           J4POINTS (vote J4=192 J16=180), not the J16 it demanded when it had only ever
           seen our broken Phase 2. The caller's J is the authority for BOTH flags. */
        static int applied = 0;
        if (s->v34_rx.J_received && !applied) {
            static int obey = -1;
            if (obey < 0) { char *e = getenv("SIPFAX_J16_OBEY"); obey = e ? atoi(e) : 1; }
            if (obey) {
                applied = 1;
                s->v34_tx.is_16states = s->v34_rx.rx_j16 ? 1 : 0;
                s->v34_tx.mp_16point  = s->v34_rx.rx_j16 ? 1 : 0;
                { extern int v34_dbg; if (v34_dbg)
                    fprintf(stderr, "[p4] caller commanded %s -> TRN/MP/E all %s\n",
                            s->v34_rx.rx_j16 ? "16-point" : "4-point",
                            s->v34_rx.rx_j16 ? "16-point" : "4-point"); }
            }
        }
    }
    s->v34_tx.p4_mp_hunt_rx = (s->v34_rx.p4_mode == 2);
    s->v34_tx.p4_mp_rx = s->v34_rx.p4_mp_rx;
    s->v34_tx.p4_mpp_rx = s->v34_rx.p4_mpp_rx;
    s->v34_tx.p4_e_rx = s->v34_rx.p4_e_rx;
    /* SIPFAX: bridge the negotiated MP PARAMETERS too, not just the flags -- the MP
       decoder stores them on v34_rx while V34_send_MP builds the frame from v34_tx. */
    s->v34_tx.p4_mp_rate_ca = s->v34_rx.p4_mp_rate_ca;
    s->v34_tx.p4_mp_rate_ac = s->v34_rx.p4_mp_rate_ac;
    s->v34_tx.p4_trellis    = s->v34_rx.p4_trellis;
    s->v34_tx.p4_mp_mask    = s->v34_rx.p4_mp_mask;
    {   /* SIPFAX: the precoder coefficients in the peer's MP are what ITS receiver computed
           for OUR transmitter to apply (9.6.2) - they are a request, not a description. Carry
           them, and the non-linear-encoder request, to the tx instance. */
        int hq; for (hq = 0; hq < 6; hq++) s->v34_tx.peer_h[hq] = s->v34_rx.peer_h[hq];
        s->v34_tx.peer_nonlin = s->v34_rx.peer_nonlin;
    }
    V34_mod(&s->v34_tx, output, nb_samples);
    {   /* Phase-3 output stage: /5 level-match then optional pre-emphasis, both TUNABLE
           at runtime for level/pre-emphasis sweeps (SIPFAX_P3_GAIN, SIPFAX_P3_PREEMPH). */
        static int p3_init = 0; static double p3_gain = 1.0; static int p3_preemph = 1;
        int _i;
        if (!p3_init) {
            char *g = getenv("SIPFAX_P3_GAIN"); char *pe = getenv("SIPFAX_P3_PREEMPH");
            if (g) p3_gain = atof(g);
            if (pe) p3_preemph = atoi(pe);
            fprintf(stderr, "[v34p3] output stage: gain=%.2f preemph=%d\n", p3_gain, p3_preemph);
            fflush(stderr); p3_init = 1;
        }
        static const double p3fir[31] = {
            0.000262,0.000420,0.000231,0.001537,-0.000164,0.001513,-0.000989,-0.001946,
            -0.002864,-0.023000,-0.006023,-0.046162,0.001433,-0.026118,-0.114908,0.971567,
            -0.114908,-0.026118,0.001433,-0.046162,-0.006023,-0.023000,-0.002864,-0.001946,
            -0.000989,0.001513,-0.000164,0.001537,0.000231,0.000420,0.000262 };
        for (_i = 0; _i < nb_samples; _i++) {
            double xx = (double)output[_i] / 5.0;
            double yy;
            if (p3_preemph) {
                int _k;
                for (_k = 30; _k > 0; _k--) s->p3h[_k] = s->p3h[_k-1];
                s->p3h[0] = xx;
                yy = 0.0; for (_k = 0; _k < 31; _k++) yy += p3fir[_k] * s->p3h[_k];
            } else yy = xx;
            yy *= p3_gain;
            if (yy > 32767) yy = 32767; if (yy < -32768) yy = -32768;
            output[_i] = (s16)yy;
        }
    }
    if (s->p3rep && s->p3rep_len > 0) {
        int _i;
        for (_i = 0; _i < nb_samples; _i++)
            output[_i] = (s->p3rep_ptr < s->p3rep_len) ? s->p3rep[s->p3rep_ptr++] : 0;
    }
    /* Phase-3 telemetry. Turn-taking is per spec: TRN -> J; the caller answers J
       with its S (11.3.1.1.3); our RX machine handles the rest. */
    {
        if (s->p3n == 0) { fprintf(stderr, "[v34p3] Phase 3: S/Sbar/PP/TRN -> J (awaiting caller S)\n"); fflush(stderr); }
        s->p3n += nb_samples;
        if (!s->p3go) {
            double e = 0; int _i;
            for (_i = 0; _i < nb_samples; _i++) e += (double)input[_i]*input[_i];
            if (e/nb_samples > 500.0*500.0) {
                s->p3go = 1;
                fprintf(stderr, "[v34p3] CALLER TRANSMITTING in Phase 3 (rx rms>500) at p3n=%ldms\n", s->p3n*1000/8000); fflush(stderr);
            }
        }
        /* YIELD THE FLOOR (match slmodem timing): slmodem transmits its Phase-3 block
           ~2.3s then goes SILENT ~2s so the caller can send its S; we used to spam J
           forever and the modem never got a clear window. Mute output in a repeating
           cycle: ~2.3s TX, ~2.0s silent, so the modem sees the same turn-taking rhythm
           it connects to with slmodem. Stop cycling once the caller transmits (p3go). */
        {
            long ms = s->p3n * 1000 / 8000;
            long cyc = ms % 4300;
            if (s->p3rep) cyc = 0;   /* replay: never yield-mute, play recording as-is */            /* 2300 TX + 2000 silent */
            int yielding = (!s->p3go) && (cyc >= 2300);
            /* SIPFAX: flush the stale J. While muted the modulator keeps cycling J
               through the tx symbol queue, and the leftover ~80 queued symbols plus the
               tx-filter pipeline drained AUDIBLY when J_received unmuted us: the Phase-4
               burst opened with 94 symbols (28 ms) of stale J before S. slmodem opens
               with S directly. Keep muting for exactly the queued residue (7/3 samples
               per symbol at 8 kHz) so S is the first thing on the wire. */
            /* (stale-J handling moved to the bridge: the queued symbols are zeroed
               in place, so no output muting is needed here any more) */
            /* SIPFAX: Phase 3 is FULL DUPLEX (11.3): both modems transmit S, S-bar, PP,
               TRN and J simultaneously - that is what the Phase-2 echo cancellers are
               for. This mute zeroed our ENTIRE Phase-3 block whenever the caller began
               transmitting first, which is exactly what happened once reactive ranging
               let the caller reach its Phase 3 promptly: it started at p3n=40ms, our
               S/PP/TRN/J went out silent, it had nothing to train on and never sent J,
               and we sat in WAIT_J - a deadlock, each side waiting for the other.
               Yield only once our own block is fully transmitted (state WAIT_J), which
               preserves the original intent of not spamming J over the caller. */
            /* SIPFAX: the p3go mute is GONE. Restricting it to WAIT_J still killed the
               J itself: V34_send_J queues its symbols and the state machine moves to
               WAIT_J in the same call, so the mute zeroed the very J the caller is
               waiting for - measured live, our J never reached the wire and the caller
               never answered with its own. 11.3 makes Phase 3 duplex: the answer modem
               transmits S, S-bar, PP, TRN and then J continuously until it detects the
               call modem's J. Only the pre-p3go yield cycle remains, which just gives
               the caller a clear window to START. */
            {   /* SIPFAX: YIELD THE FLOOR AFTER OUR J IS ON THE WIRE.
                   Measured on a call that WORKS (sl-up/sl-down, the same caller):

                     t=7.50-9.50   slmodem only   - it sends its phase-3 block
                     t=10.0-11.75  CALLER ONLY    - slmodem is silent, rms 0, for 2.0 s
                     t=12.0+       both           - data mode, full duplex

                   slmodem hands the caller a clean two-second window and the caller
                   completes its phase 4 in it. We transmit continuously through the same
                   window (2240-2350 rms across t=10-16), so we are talking over the caller
                   exactly while it trains - which is why it takes 2.78 s to return J and
                   1.82 s to return MP, and then abandons.

                   The old p3go mute did this and was removed for a real reason: V34_send_J
                   QUEUES its symbols and the state machine enters WAIT_J in the same call,
                   so muting on the state transition zeroed the very J the caller waits for
                   and both sides deadlocked. The idea was right and the trigger was wrong.
                   Gate on TIME SINCE the J went out instead: keep transmitting for
                   SIPFAX_J_HOLD ms after entering WAIT_J so the queued J and the tx filter
                   pipeline fully drain, then yield until the caller answers with its J.
                   SIPFAX_J_YIELD=0 restores the previous always-on behaviour. */
                extern int v34_dbg; extern long g_txsym_out;
                static int yen = -1, yhold = -1; static long wait_j_start = -1;
                static long drain_mark = -1;
                static int announced = 0;
                if (yen < 0)   { char *e = getenv("SIPFAX_J_YIELD"); yen = e ? atoi(e) : 1; }
                if (yhold < 0) { char *e = getenv("SIPFAX_J_HOLD");  yhold = e ? atoi(e) : 0; }
                /* 400 ms stalled 2 of 4 calls in WAIT_J - the queued J and the tx filter did
                   not always reach the wire before the mute engaged, and the caller never
                   answered. 800 ms: 3 of 3 reached MP, ac stayed at 26400 and S4_MP stayed at
                   0.920 s, so the longer hold costs neither gain. */
                if (yen && s->v34_tx.state == V34_STARTUP3_WAIT_J && !s->v34_rx.J_received) {
                    /* SIPFAX: DRAIN CHECK, not a timer. A fixed hold cannot win this race -
                       the mute is chasing the J out of the transmit queue and the filter
                       pipeline, and that drain time varies per call, which is why 400 ms and
                       800 ms both stalled about 40% of calls (5 of 8 reached DATA overall).
                       The queue depth is readable, so read it: on entering WAIT_J note how
                       many symbols are outstanding, add the filter width, and wait until the
                       modulator has actually consumed that many. g_txsym_out counts symbols
                       leaving tx_buf, so this is the real event rather than a guess at it.
                       SIPFAX_J_HOLD adds an extra fixed delay on top if ever needed. */
                    if (wait_j_start < 0) {
                        wait_j_start = s->p3n;
                        drain_mark = g_txsym_out + s->v34_tx.tx_buf_size
                                                 + s->v34_tx.tx_filter_wsize;
                        if (v34_dbg)
                            fprintf(stderr, "[v34p3] WAIT_J: %d symbols queued + %d filter "
                                    "-> yield once %ld symbols have gone out\n",
                                    s->v34_tx.tx_buf_size, s->v34_tx.tx_filter_wsize,
                                    drain_mark - g_txsym_out);
                    }
                    if (g_txsym_out >= drain_mark
                        && (s->p3n - wait_j_start) * 1000 / 8000 >= yhold) {
                        /* SIPFAX: DUTY CYCLE, not a permanent mute. The drain check above
                           measured the thing the fixed holds were guessing at: 97 queued
                           symbols + 40 of filter = 137 symbols, which is about 40 ms at 3429
                           baud. So the J was always on the wire within 40 ms, we were waiting
                           400-800 ms, and calls STILL stalled ~40% of the time - the drain was
                           never the cause.
                           What actually stalls is muting FOREVER once drained. 11.3.1.2.4 says
                           keep sending J until the caller responds; we sent it once and then
                           went silent until J_received, so a caller that missed that single
                           burst never got another and both sides waited. That is exactly the
                           shape of an intermittent ~40% failure.
                           Alternate instead: a short J burst, then a silent window for the
                           caller, repeating. It gets J repeatedly AND gets quiet to answer in.
                           SIPFAX_J_ON / SIPFAX_J_OFF are the two halves in ms. */
                        static int jon = -1, joff = -1;
                        long since, per, ph;
                        since = (s->p3n - wait_j_start) * 1000 / 8000;
                        /* SIPFAX: defaults restored to the best configuration measured.
                           Four variants were tried and compared on live calls:

                             400 ms J then mute      ac=26400   2 of 4 reached DATA
                             800 ms J then mute      ac=26400   5 of 8 reached DATA
                             drain-gated (40 ms)     - masked by a stale J_HOLD override
                             120/380 ms duty cycle   ac=24000 and 9600, 2 of 5

                           The duty cycle is worse on the metric that matters most - the
                           caller's own estimate of our channel fell from a consistent 26400
                           to 24000 and 9600 - because chopping phase 3 into 120 ms bursts
                           gives it less continuous signal to measure. Reading the four
                           together, the '800 ms hold' was never waiting for the 40 ms drain:
                           it was SENDING J for 800 ms, and the amount of J before the silence
                           is what tracks ac. So: send J for J_ON, then stay quiet.
                           J_OFF = 0 means the silence is permanent (the 800 ms configuration);
                           set J_OFF > 0 to duty-cycle instead. */
                        if (jon  < 0) { char *e = getenv("SIPFAX_J_ON");  jon  = e ? atoi(e) : 800; }
                        if (joff < 0) { char *e = getenv("SIPFAX_J_OFF"); joff = e ? atoi(e) : 0; }
                        if (joff <= 0) {
                            if (since >= jon) {
                                if (!announced && v34_dbg) {
                                    fprintf(stderr, "[v34p3] J sent for %ld ms - yielding the "
                                            "floor for the caller's phase 4\n", since);
                                    fflush(stderr); announced = 1;
                                }
                                yielding = 1;
                            }
                            goto duty_done;
                        }
                        since = (s->p3n - wait_j_start) * 1000 / 8000;
                        per = jon + joff; if (per < 1) per = 1;
                        ph = since % per;
                        if (!announced && v34_dbg) {
                            fprintf(stderr, "[v34p3] J drained after %ld ms (%ld symbols) - "
                                    "duty-cycling J %d ms on / %d ms off for the caller\n",
                                    since, drain_mark - (g_txsym_out - (g_txsym_out - drain_mark)),
                                    jon, joff);
                            fflush(stderr); announced = 1;
                        }
                        if (ph >= jon) yielding = 1;    /* silent half of the cycle */
                        duty_done: ;
                    }
                } else if (s->v34_rx.J_received) {
                    wait_j_start = -1; drain_mark = -1; announced = 0;
                }
            }
            if (yielding) {
                int _i; for (_i = 0; _i < nb_samples; _i++) output[_i] = 0;
                if (yielding && cyc < 2300 + (nb_samples*1000/8000) + 1)
                    { fprintf(stderr, "[v34p3] yielding the floor (silent ~2s for caller S) at %ldms\n", ms); fflush(stderr); }
            }
        }
    }
    return 0;
}

/* V34 test: half duplex with fixed parameters */

#define NB_SAMPLES 40 /* 5 ms */

static int test_get_bit(void *opaque)
{
    return 1;
}

static int nb_bits, errors;

static void test_put_bit(void *opaque, int bit)
{
    nb_bits++;
    if (bit != 1) {
        errors++;
    }
}

void V34_test(void)
{
    V34State s;
    V34DSPState v34_rx, v34_tx;
    int err;
    struct LineModelState *line_state;
    s16 buf[NB_SAMPLES];
    s16 buf1[NB_SAMPLES];
    s16 buf2[NB_SAMPLES];
    s16 buf3[NB_SAMPLES];
    FILE *f1;
    
    err = lm_display_init();
    if (err < 0) {
        fprintf(stderr, "Could not init X display\n");
        exit(1);
    }

    line_state = line_model_init();

    /* fill the test V34 parameters */
#if 0
    s.S = V34_S3200;
    s.R = 28800;
#else
    s.S = V34_S2400;
    s.R = 19200;
#endif
    s.expanded_shape = 0;
    s.conv_nb_states = 16;
    s.use_non_linear = 0;
    s.use_high_carrier = 1;
    s.use_aux_channel = 0;
    memset(s.h, 0, sizeof(s.h));

    f1 = fopen("cal.sw", "wb");
    if (f1 == NULL) {
        perror("cal.sw");
        exit(1);
    }

    s.calling = 1;
    V34_mod_init(&v34_tx, &s);
    v34_tx.opaque = NULL;
    v34_tx.get_bit = test_get_bit;

    s.calling = 0;
    V34_demod_init(&v34_rx, &s);
    v34_rx.opaque = NULL;
    v34_rx.put_bit = test_put_bit;

    nb_bits = 0;
    errors = 0;
    for(;;) {
        if (lm_display_poll_event())
            break;
        
        V34_mod(&v34_tx, buf, NB_SAMPLES);
        
        memset(buf3, 0, sizeof(buf3));

        line_model(line_state, buf1, buf, buf2, buf3, NB_SAMPLES);
        
        fwrite(buf, 1, NB_SAMPLES * 2, f1);

        V34_demod(&v34_rx, buf, NB_SAMPLES);
    }

    fclose(f1);

    fprintf(stderr, "errors=%d nb_bits=%d Pe=%f\n", 
           errors, nb_bits, (float) errors / (float)nb_bits);
}
