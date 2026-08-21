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
static int  data_slice(V34DSPState *s, double xi, double xq, double *di, double *dq);
static double data_lattice_rms(const double *bi, const double *bq, int n,
                               double g, double ct, double st);
static int  p4_block_step(const short *x, int n, int *ca, int *ac, int *trel, int *ack,
                          int *shape, unsigned int *mask, int *sixteen_out, short *hout);
void baseband_decode_pub(V34DSPState *s, int si, int sq);
int v34_dbg = 0;  /* offline decode verbosity */
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
        int x2;
        float dzeta,theta;
        /* XXX: average power ? */

        x2 = (x_re * x_re + x_im * x_im) >> 7;
        dzeta = 0.3125 /* x2 / 128.0 */;
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
static void put_sym(V34DSPState *s, int si, int sq)
{
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
                for (hb = 0; hb < 16; hb++)
                    put_bits(&p, 1, (s->h[i][j] >> hb) & 1);
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



static void V34_mod(V34DSPState *s, s16 *samples, unsigned int nb)
{
    int n;

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

        { extern int v34_dbg; if (v34_dbg && s->state != s->dbg_last2) { fprintf(stderr, "[enc] tx protocol state -> %d\n", s->state); fflush(stderr); s->dbg_last2 = s->state; } }
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
    int i, j, k, n, nbbt, nb_trans, state, next_state, error, trellis_ptr;
    int error_table[32],decision_table[32],emin,jmin,u0,x,y;
    u8 *p,*q;

    trellis_ptr = s->trellis_ptr;

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
        p = &trellis_trans_16b[0][0]; /* SIPFAX: corrected 16-cand table */
        break;
    }
    nb_trans = 1 << nbbt;

    /* write a previous decoded symbol : extract a decoded bit from
       the beginning of a path */

    k = trellis_ptr;
    k--;
    if (k < 0) k = TRELLIS_LENGTH-1;
    /* start traceback from the MINIMUM-metric survivor (not arbitrary j=0):
       survivors have not necessarily merged, so the wrong start gives wrong bits */
    { int bs=0, be=s->state_error[0], st; for(st=1;st<s->conv_nb_states;st++) if(s->state_error[st]<be){be=s->state_error[st];bs=st;} j=bs; }
    for(i=0;i<(TRELLIS_LENGTH-1);i++) {
        j = s->state_path[j][k];
        k--;
        if (k < 0) k = TRELLIS_LENGTH-1;
    }
    u0 = s->u0_memory[trellis_ptr];
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
      } }
    /* undo the rotation */    
    if ((s->state_decision[j][k] >> 7)) {
        x = yout[1][1];
        y = - yout[1][0];
        yout[1][0] = x;
        yout[1][1] = y;
    }
    /* rotate only if u0 is set */
    if (u0 ^ (s->state_decision[j][k] >> 7)) {
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
    if (s->conv_nb_states >= 64) n = 16; /* SIPFAX: 16 coset-tuples/branch */
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

    /* init the error table to +infinity */
    for(state=0;state<s->conv_nb_states;state++) {
        s->state_error1[state] = 0x7fffffff;
    }

    for(state=0;state<s->conv_nb_states;state++) {
        /* select the value of y0 depending on the current state */
        /* for each state, we update the next state entry by selecting
           the shortest path */
        /* XXX: should handle error overflow */
        if (state & 1)
            n = nb_trans;
        else 
            n = 0;
        for(j=0;j<nb_trans;j++) {
            next_state = trellis_next_state(s->conv_nb_states, state, j);
            error = s->state_error[state] + error_table[j + n];
            if (error < s->state_error1[next_state]) {
                s->state_error1[next_state] = error;
                s->state_decision[next_state][trellis_ptr] = decision_table[j + n];
                s->state_path[next_state][trellis_ptr] = state;
            }
        }
    }

    /* XXX: this copy is not needed. Permute the two tables */
    memcpy(s->state_error, s->state_error1, sizeof(s->state_error));

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

      t = s->constellation_to_code[(x+C_RADIUS) >> 1][(y+C_RADIUS) >> 1];
      /* mapping to the symbol */
      Z[i] = t >> 14;
      t = t & 0xff;

      Q[j][i] = t & ((1 << s->q)-1);
      m[j][i] = t >> s->q;
    }

    t = (Z[0] - s->Z_1) & 3;
    s->Z_1 = Z[0];
    I[1][j] = t & 1;
    I[2][j] = t >> 1;
    
    t = (Z[1] - Z[0]) & 3;
    I[0][j] = t >> 1;
  }

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

        /* synchronization bit */
        if (s->sync_count == 0) {
            v0 = (SYNC_PATTERN >> (15 - s->half_data_frame_count)) & 1;
        } else {
            v0 = 0;
        }

        /* synchronization bit */
        if (++s->sync_count == 2*s->P) {
            s->sync_count = 0;
            if (++s->half_data_frame_count == 2*s->J) {
                s->half_data_frame_count = 0;
            }
        }

        memcpy(&s->rx_mapping_frame[s->rx_mapping_frame_count][0], 
               &y[0][0], 4 * sizeof(s16));
        delay++;
        if (delay > TRELLIS_LENGTH) {

            s->rx_mapping_frame_count += 2;
            if (s->rx_mapping_frame_count == 8) {
                /* a complete mapping frame was read */
                decode_mapping_frame(s, s->rx_mapping_frame); 
                s->rx_mapping_frame_count = 0;
            }
        }
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
    if (s->cma_t2 & 1) return;
    oi = oq = 0;
    for (i = 0; i < CMANT; i++) { oi += s->cma_wi[i]*s->cma_bufi[i] - s->cma_wq[i]*s->cma_bufq[i];
                                  oq += s->cma_wi[i]*s->cma_bufq[i] + s->cma_wq[i]*s->cma_bufi[i]; }
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
                int our_ca = 4, their_ca, R;
                { char *mc = getenv("SIPFAX_MP_CA"); if (mc) our_ca = atoi(mc); }
                their_ca = s->p4_mp_rate_ca > 0 ? s->p4_mp_rate_ca : 7;
                R = (their_ca < our_ca ? their_ca : our_ca) * 2400;
                s->conv_nb_states = (s->p4_trellis == 0) ? 16 : (s->p4_trellis == 1) ? 32 : 64;
                v34_rx_data_params(s, R);
                {   /* mean |c|^2 of the negotiated constellation, in lattice units */
                    int ci; double acc = 0;
                    for (ci = 0; ci < s->L; ci++)
                        acc += (double)s->constellation[ci][0]*s->constellation[ci][0]
                             + (double)s->constellation[ci][1]*s->constellation[ci][1];
                    s->data_pw_target = (s->L > 0 ? acc / s->L : 30.0) * (128.0*128.0);
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
                s->data_th = s->srx_th; s->data_frq = 0.0;   /* inherit Phase-4 phase */
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
                if (kp < 0) { char *e1 = getenv("SIPFAX_DD_KP"); kp = e1 ? atof(e1) : 0.0;   /* acquisition handles the static offset; DD drags a dense constellation */
                              char *e2 = getenv("SIPFAX_DD_KI"); ki = e2 ? atof(e2) : 0.0; }
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
                    if (s->data_acq_n < DATA_ACQ_N) {
                        double ct0 = cos(-s->data_th), st0 = sin(-s->data_th);
                        s->data_acq_i[s->data_acq_n] = (oi*ct0 - oq*st0) * gc;
                        s->data_acq_q[s->data_acq_n] = (oi*st0 + oq*ct0) * gc;
                        s->data_acq_n++;
                    } else
                    {
                        double bg = 1.0, be = 1e30, bp = 0.0, gdb, th2;
                        double pw0 = 0, pwmin; int qi;
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
                        for (gdb = -14.0; gdb <= 6.0; gdb += 0.1) {
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
                        s->data_agc *= bg;
                        s->data_th  += bp*M_PI/180.0;
                        s->data_acq_done = 1;
                        gc = s->data_agc;
                        { extern int v34_dbg; if (v34_dbg)
                            fprintf(stderr, "[data] acquired: gain x%.3f, phase %+.2f deg,"
                                    " lattice-rms %.3f (0.577 = no lock, <0.2 = good)\n",
                                    bg, bp, be); }
                    }
                }
                if (s->data_acq_done) {
                ct2 = cos(-s->data_th); st2 = sin(-s->data_th);
                xi = (oi*ct2 - oq*st2) * gc;
                xq = (oi*st2 + oq*ct2) * gc;
                data_slice(s, xi, xq, &di, &dq);
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
                    if (skip > 0) { skip--; }
                    else {
                        int si2 = (int)lrint(xi*128.0), sq2 = (int)lrint(xq*128.0);
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
                    fprintf(stderr, "[data] %ld syms, metric %.1f, freq %.2e rad/sym\n",
                            s->data_n, s->data_mse_n ? s->data_mse_acc/s->data_mse_n : 0.0,
                            s->data_frq); }
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
                s->p4_ring[s->p4_rn & 4095] = (u8)xb; s->p4_rn++;
                if (p4bitf) fputc('0'+xb, p4bitf);
                if (xb) { if (++s->p4_ones_run >= 19 && s->p4_mp_rx && !s->p4_e_rx) {
                            s->p4_e_rx = 1; { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] E received at sym %ld\n", s->cma_qn); } } }
                else s->p4_ones_run = 0;
                if (!s->p4_mpp_rx && s->p4_rn > 700 && (++s->p4_try >= 128)) {
                    int Ls[2] = { 88, 188 }, li;
                    s->p4_try = 0;
                    for (li = 0; li < 2; li++) {
                        int L = Ls[li], per = 8, avail = s->p4_rn > 4096 ? 4096 : s->p4_rn;
                        int start2, i2, run, st;
                        static u8 maj[188]; static int votes[188];
                        if (avail < per*L) { if (avail/L >= 3) per = avail/L; else continue; }
                        start2 = s->p4_rn - per*L;
                        for (i2 = 0; i2 < L; i2++) votes[i2] = 0;
                        for (i2 = 0; i2 < per*L; i2++) votes[i2 % L] += s->p4_ring[(start2 + i2) & 4095];
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
                                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] FOLD L=%d type=%d ca=%d ac=%d trel=%d ack=%d crc=%s cons=%d\n", L, type, rate_ca*2400, rate_ac*2400, trel, ackb, ok?"OK":"fail", s->p4_keyn); }
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
                            /* Phase 4: re-hunt the caller's post-J' TRN */
                            s->p4_mode = 1; s->srx_locked = 0;
                            for (rr2 = 0; rr2 < 4; rr2++) { s->srx_reg4[rr2] = 0; s->srx_hist4[rr2] = 0; }
                        }
                    }
                }
            }
        }
        s->srx_pqd = qd;
    }
    if (s->p4_mode == 2) {   /* MP/data: decision-FREE CMA tap tracking. DD decisions are
                                wrong once timing drifts, so DD-LMS can't recover; CMA holds
                                constant modulus (4-point) and tracks slow channel/clock drift. */
        double r2t2 = srx_rx16() ? 1.32 : 1.0;                        /* SIPFAX: 16-pt Godard radius */
        double m2 = oi*oi + oq*oq, g = r2t2 - m2; ei = g*oi; eq = g*oq; mu = 2e-3;
    }
    for (i = 0; i < CMANT; i++) {
        double gi = ei*s->cma_bufi[i] + eq*s->cma_bufq[i];
        double gq = eq*s->cma_bufi[i] - ei*s->cma_bufq[i];
        s->cma_wi[i] += mu*gi; s->cma_wq[i] += mu*gq;
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
static int p4_equalize(const double *zi, const double *zq, int nz, double off,
                       int sixteen, int ncma, int ndd, int reset, double *si, double *sq)
{
    static double wi[P4_NT], wq[P4_NT], bufi[P4_NT], bufq[P4_NT];
    double R2 = sixteen ? 1.32 : 1.0;          /* Godard radius, unit mean power */
    double scale = sixteen ? sqrt(10.0) : sqrt(2.0);   /* base units -> unit power */
    int p, i, ns = 0;
    if (reset) {
        for (i = 0; i < P4_NT; i++) { wi[i] = 0; wq[i] = 0; }
        wi[P4_NT/2] = 1.0;
    }
    for (p = 0; p < ncma + ndd; p++) {
        int dd = (p >= ncma), cnt = 0;
        double pos = off*P4_SPS + P4_SPS*8, th = 0, fr = 0, g = 1.0;
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
            if (cnt & 1) { pos += P4_SPS/2.0; continue; }   /* T/2: adapt on symbol instants */
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
            pos += P4_SPS/2.0;
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
                        int *out_shape, unsigned int *out_mask, short *out_h)
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
                         int *shape, unsigned int *mask, int *sixteen_out, short *hout)
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
                               p4_six, V34_GPC, ca, ac, trel, ack, shape, mask, hout);
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

static int data_slice(V34DSPState *s, double xi, double xq, double *di, double *dq)
{
    int i, best = 0; double bd = 1e30;
    for (i = 0; i < s->L; i++) {
        double dx = xi - (double)s->constellation[i][0];
        double dy = xq - (double)s->constellation[i][1];
        double d = dx*dx + dy*dy;
        if (d < bd) { bd = d; best = i; }
    }
    *di = (double)s->constellation[best][0];
    *dq = (double)s->constellation[best][1];
    return best;
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
                int ca = 0, ac = 0, tr = 0, ak = 0, sh = 0, six = 0, nmp;
                unsigned int mk = 0;
                {   /* SIPFAX: this decoder starved the transmit path once (see
                       p4_block_step) - keep it measured so it cannot happen silently. */
                    static double worst = 0.0;
                    struct timespec ta, tb; double ms;
                    clock_gettime(CLOCK_MONOTONIC, &ta);
                    nmp = p4_block_step(p4b + (p4bn > 20000 ? p4bn-20000 : 0),
                                        p4bn > 20000 ? 20000 : p4bn,
                                        &ca, &ac, &tr, &ak, &sh, &mk, &six, s->peer_h);
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
        int t2; double beta = 0.15, sum2 = 0.0;
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
        for (;;) {              /* exact 7:6 grid: output m lives at input position 7m/6 */
            long q = 7*s->cma_m; long ip = q/6; int fr = (int)(q%6);
            long need = ip + (fr ? 1 : 0);
            double f, vi, vq;
            if (need > s->cma_n) break;
            if (fr == 0) { vi = cbi; vq = cbq; }
            else { f = fr/6.0; vi = s->cma_pbi*(1.0-f) + cbi*f; vq = s->cma_pbq*(1.0-f) + cbq*f; }
            V34_cma_t2sample(s, vi, vq);
            s->cma_m++;
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
    cma_dumpf = fopen("/tmp/stream-soft.txt","w"); { char*e=getenv("SIPFAX_P4BITS"); if(e) p4bitf=fopen(e,"w"); }
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
    p.conv_nb_states = 64; { char *se=getenv("SIPFAX_DL_SHAPE"); p.expanded_shape = se?atoi(se):0; } p.use_non_linear = 0; p.use_aux_channel = 0;
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
    memset(&tx,0,sizeof(tx)); memset(&rx,0,sizeof(rx)); memset(&pt,0,sizeof(pt)); memset(&pr,0,sizeof(pr));
    { extern void dsp_init(void); dsp_init(); } V34_static_init();
    pt.S=V34_S3429; pt.R=R; pt.use_high_carrier=1; pt.calling=1; pt.conv_nb_states=64;
    { char *se=getenv("SIPFAX_DL_SHAPE"); pt.expanded_shape = se?atoi(se):0; }
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
        fprintf(stderr,"[dataloop] best lag=%d: %d/%d = %.1f%% bit match (100%%=DSP round-trips)\n", bestlag, mt, cn, 100.0*mt/(cn?cn:1)); } }
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
                                &ca, &ac, &tr, &ak, &sh, &mk, &six, hh);
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
                               &ca, &ac, &tr, &ak, &sh, &mk, NULL);
            if (!nmp) {
                int ns4 = p4_equalize(zi, zq, nz, off, 0, 6, 10, 1, si, sq);
                nmp = p4_mp_decode(si+200, sq+200, ns4-200 > 0 ? ns4-200 : 0, 0, V34_GPC,
                                   &ca, &ac, &tr, &ak, &sh, &mk, NULL);
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
