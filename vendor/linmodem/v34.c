#include <stdlib.h>
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
                r_ca = 7; r_ac = 7; trel = 0; msk = 0x3fff;
                shape = 1;
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
                put_bits(&p, 1, 0); /* start bit */
                put_bits(&p, 16, s->h[i][j]); /* precoding coef */
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

/* J sequence */
#define J4POINTS   0x0991
#define J16POINTS  0x0D91
#define JEND       0xF991

static void V34_send_J(V34DSPState *s, int length)
{
    { extern long v34_nj; v34_nj++; }
    int i,val;
    u8 buf[16],*p;

    if (s->mp_16point)
        val = J16POINTS;
    else
        val = J4POINTS;
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
            if ((s->p4_mp_hunt_rx && s->p4_trn_tx >= 1) || s->p4_trn_tx >= 6) {
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[p4] TX: TRN done (%d chunks) -> MP\n", s->p4_trn_tx); }
                s->state = V34_STARTUP4_MP;
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
    s->is_16states = 0; /* TRN stays 4-point: the caller trains on this */
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
    s->put_bit(s->opaque, b);
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
    f = fopen(path, "wb");
    fprintf(stderr, "[enc] encoding answer Phase-3 TX (S=3429) -> %s\n", path);
    for (b = 0; b < (int)(4.0 * 8000 / 512); b++) {   /* ~4 s */
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
            if (cq > 0.30 && s->cma_phn >= 500) {
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
            if (cq > 0.65 && s->cma_phn >= 300) {
                s->cma_phase = 2; s->cma_phn = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] tight (q=%.2f) -> track at sym %d\n", cq, s->cma_cnt); }
            } else if (s->cma_phn >= 2500) {
                int rj; for (rj = 0; rj < CMANT; rj++) { s->cma_wi[rj] = 0; s->cma_wq[rj] = 0; }
                s->cma_wi[CMANT/2] = 1.0; s->cma_phase = 0; s->cma_phn = 0; s->cma_c4i = s->cma_c4q = 0;
                { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[cma] DD stuck (q=%.2f), back to CMA at sym %d\n", cq, s->cma_cnt); }
            }
        }
    }
    if (s->cma_phase == 0) {                         /* Phase A: CMA blind (open eye) */
        double m2 = oi*oi + oq*oq, gg = 1.0 - m2; ei = gg*oi; eq = gg*oq; mu = mu_cma;
        if (cma_dumpf) fprintf(cma_dumpf, "%.4f %.4f 0\n", oi, oq);
    } else if (s->cma_phase == 1) {                  /* Phase B: DD refine (FSE adapts, tightens) */
        double di = (oi>=0?R:-R), dq = (oq>=0?R:-R); ei = di - oi; eq = dq - oq; mu = mu_dd;
        if (cma_dumpf) fprintf(cma_dumpf, "%.4f %.4f 0\n", oi, oq);
    } else {   /* Phase C v3: frozen taps + feedforward carrier + 4-candidate scrambler tracking */
        { static int tapdumped = 0;
          if (!tapdumped && cma_dumpf) { FILE *tf = fopen("/tmp/taps.txt","w"); int ti2;
            for (ti2 = 0; ti2 < CMANT; ti2++) fprintf(tf, "%.8f %.8f\n", s->cma_wi[ti2], s->cma_wq[ti2]);
            fclose(tf); tapdumped = 1; } }
        static u8 jpat[16]; static u8 jppat[16]; static int jpat_init = 0;
        unsigned int poly = (cma_t1 == 5) ? (1u|(1u<<18)) : (1u|(1u<<5));  /* GPA : GPC */
        double ct, st_, pi_, pq_, o2i, o2q, o4i, o4q; int qd, r, k, j, w, wm, b2s[4][2]; unsigned int regsnap[4];
        if (!jpat_init) { for (k = 0; k < 16; k++) { jpat[k] = (0x0991 >> (15-k)) & 1;
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
        for (r = 0; r < 4; r++) {
            int z = (r - qd) & 3; b2s[r][0] = z & 1; b2s[r][1] = (z >> 1) & 1;   /* spec CW */
            regsnap[r] = s->srx_reg4[r];
            for (k = 0; k < 2; k++) {
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
            int dqp = (s->srx_pqd - qd) & 3, kb;
            for (kb = 0; kb < 2; kb++) {
                int yb = kb ? ((dqp >> 1) & 1) : (dqp & 1);
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
        if (s->srx_locked && !s->J_received && s->p4_mode == 0) {
            /* (spawn-based J bank replaced by always-on regD phase scorers below) */
            {   /* Always-on J detector: regD is a second scrambler register updated with
                   the DIFFERENTIAL dibit bits (the true scrambler outputs during J; it
                   self-syncs within 23 bits of J starting). 8 pattern-phase scorers run
                   against it; a phase >=28/32 while TRN agreement is broken -> J. */
                int dq_ = (s->srx_pqd - qd) & 3; int jb0 = dq_ & 1, jb1 = (dq_ >> 1) & 1;   /* spec CW */
                unsigned int rD0 = s->srx_regD, rD1;
                rD1 = (rD0 << 1) & 0x7fffff; if (jb0) rD1 ^= poly;
                s->srx_regD = (rD1 << 1) & 0x7fffff; if (jb1) s->srx_regD ^= poly;
                for (j = 0; j < 8; j++) {
                    int q0 = (int)((rD0 >> 22) & 1) ^ (int)jpat[s->jh_bitpos[j] & 15];
                    int q1 = (int)((rD1 >> 22) & 1) ^ (int)jpat[(s->jh_bitpos[j]+1) & 15];
                    s->jh_hist[j] = (s->jh_hist[j] << 2) | ((unsigned long long)(q0 == jb0) << 1) | (unsigned long long)(q1 == jb1);
                    s->jh_bitpos[j] += 2;
                    if (wm < 52 && s->cma_qn > 80) {
                        int mj = __builtin_popcountll(s->jh_hist[j] & 0xffffffffULL);
                        if (mj >= 28 && !s->J_received) {
                            int rr2;
                            s->J_received = 1;
                            { extern int v34_dbg; if (v34_dbg) fprintf(stderr, "[srx] caller J detected at sym %ld (phase %d, %d/32) -> J_received\n", s->cma_qn, j, mj); }
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
        double m2 = oi*oi + oq*oq, g = 1.0 - m2; ei = g*oi; eq = g*oq; mu = 2e-3;
    }
    for (i = 0; i < CMANT; i++) {
        double gi = ei*s->cma_bufi[i] + eq*s->cma_bufq[i];
        double gq = eq*s->cma_bufi[i] - ei*s->cma_bufq[i];
        s->cma_wi[i] += mu*gi; s->cma_wq[i] += mu*gq;
    }
    s->cma_cnt++;
}
void V34_demod_cma(V34DSPState *s, const s16 *samples, unsigned int nb)
{
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
void V34_stream_decode_file(const char *path)
{
    extern int v34_dbg;
    V34State p; static V34DSPState rx; s16 buf[512]; FILE *f; int n, i;
    memset(&p, 0, sizeof(p)); memset(&rx, 0, sizeof(rx));
    p.S = V34_S3429; p.R = 33600; p.conv_nb_states = 16; p.use_high_carrier = 1; p.calling = 0;
    { extern void dsp_init(void); dsp_init(); } V34_static_init();
    rx.S = p.S; rx.use_high_carrier = 1;
    v34_dbg = 1;
    f = fopen(path, "rb"); if (!f) { perror(path); return; }
    cma_dumpf = fopen("/tmp/stream-soft.txt","w"); { char*e=getenv("SIPFAX_P4BITS"); if(e) p4bitf=fopen(e,"w"); }
    { char *t2 = getenv("SIPFAX_T2DUMP"); if (t2) cma_t2df = fopen(t2, "w"); } fprintf(stderr, "[stream] decoding %s via V34_demod_cma\n", path);
    while ((n = fread(buf, 2, 512, f)) > 0) V34_demod_cma(&rx, buf, n);
    fclose(f);
    if(cma_dumpf){fclose(cma_dumpf);cma_dumpf=0;} if(cma_t2df){fclose(cma_t2df);cma_t2df=0;} if(p4bitf){fclose(p4bitf);p4bitf=0;} fprintf(stderr, "[stream] END: J_received=%d locked=%d rot=%d cma_cnt=%d\n", rx.J_received, rx.srx_locked, rx.srx_rot, rx.cma_cnt);
}


void V34_datacfg_dump(void)
{
    extern int v34_dbg;
    V34State p; static V34DSPState s;
    char *e; int R = 16800, i;
    memset(&p, 0, sizeof(p)); memset(&s, 0, sizeof(s));
    e = getenv("SIPFAX_DATA_R"); if (e) R = atoi(e);
    p.S = V34_S3429; p.R = R; p.use_high_carrier = 1; p.calling = 0;
    p.conv_nb_states = 64; p.expanded_shape = 0; p.use_non_linear = 0; p.use_aux_channel = 0;
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
static void dataloop_symsink(int si, int sq) { extern void baseband_decode_pub(V34DSPState*,int,int); baseband_decode_pub(g_rx_state, si, sq); }
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
    memcpy(&pr,&pt,sizeof(pr)); pr.calling=0;
    V34_init_low(&tx,&pt,1); V34_init_low(&rx,&pr,0);
    tx.get_bit=dataloop_src; tx.opaque=0; rx.put_bit=dataloop_sink; rx.opaque=0;
    g_prbs=1; g_txn=0; g_rxn=0; g_rx_state=&rx;
    { extern FILE *gt_f; char *ge=getenv("SIPFAX_GTDUMP"); if(ge) gt_f=fopen(ge,"w"); }
    { extern void (*g_symtap)(int,int); g_symtap=dataloop_symsink;
      for(i=0;i<4000;i++) encode_mapping_frame(&tx);
      g_symtap=0; { extern FILE *gt_f; if(gt_f){fclose(gt_f);gt_f=0;} } }
    fprintf(stderr,"[dataloop] R=%d tx_bits=%d rx_bits=%d\n", R, g_txn, g_rxn);
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
    s->expanded_shape = 0;
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
    s->v34_tx.J_received = s->v34_rx.J_received;   /* bridge caller-J -> TX WAIT_J */
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
            if (yielding || (s->p3go && !s->v34_rx.J_received)) {
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
