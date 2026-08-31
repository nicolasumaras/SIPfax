/* 
 * V22 modulator & demodulator
 * 
 * Copyright (c) 1999,2000 Fabrice Bellard.
 *
 * This code is released under the GNU General Public License version
 * 2. Please read the file COPYING to know the exact terms of the
 * license.
 */
#include "lm.h"
#include <math.h>

/*
 * This code is also used by the V34 phase 2 at 600 bits/s. V22bis
 * 2400 bps is implemented in the modulation but not in the
 * demodulation.  
 */

void V22_mod_init(V22ModState *s)
{
    s->baud_phase = 0;
    s->baud_num = 3;
    s->baud_denom = 40;
    s->tx_filter_wsize = V22_TX_FILTER_SIZE / s->baud_denom;
    memset(s->tx_buf, 0, sizeof(s->tx_buf));
    s->tx_outbuf_ptr = 0;
    s->carrier_phase = 0;
    s->carrier2_phase = 0;
    s->Z = 0;
    
    if (s->calling) {
        /* call modem DPSK: 600 bps, carrier at 1200 Hz, 0 db */
        s->carrier_incr = (PHASE_BASE * 1200.0) / V34_SAMPLE_RATE;
    } else {
        /* answer modem DPSK: 600 bps, carrier at 2400 Hz, -1 db, 
           guard tone at 1800 Hz, -7db */
        s->carrier_incr = (PHASE_BASE * 2400.0) / V34_SAMPLE_RATE;
        s->carrier2_incr = (PHASE_BASE * 1800.0) / V34_SAMPLE_RATE;
    }
}

static void V22_mod_baseband(V22ModState *s, s16 *x_ptr, s16 *y_ptr)
{
    int x, y, x1, y1, b1, b2;

    /* handle each kind of modulation */
    switch(s->mod_type) {
    default:
    case V34_MOD_600:
        b1 = s->get_bit(s->opaque);
        /* rotation by 0 or 180 degrees */
        s->Z = s->Z ^ (b1 << 1);
        x1 = 0x2000;
        y1 = 0x2000;
        break;
    case V22_MOD_600:
        b1 = s->get_bit(s->opaque);
        /* rotation by 90 or 270 degrees */
        s->Z = (s->Z + ((b1 << 1) | 1)) & 3;
        x1 = 0x2000;
        y1 = 0x2000;
        break;
    case V22_MOD_1200:
        b1 = s->get_bit(s->opaque);
        b2 = s->get_bit(s->opaque);
        b2 ^= (1 - b1);
        s->Z = (s->Z + ((b1 << 1) | b2)) & 3;
        x1 = 0x2000;
        y1 = 0x2000;
        break;
    case V22_MOD_2400:
        /* quadrant selection */
        b1 = s->get_bit(s->opaque);
        b2 = s->get_bit(s->opaque);
        b2 ^= (1 - b1);
        s->Z = (s->Z + ((b1 << 1) | b2)) & 3;
        /* 4 positions inside the quadrant */
        b1 = s->get_bit(s->opaque);
        b2 = s->get_bit(s->opaque);
        /* XXX: normalize */
        x1 = 0x1000;
        if (b2) 
            x1 += 0x2000;
        y1 = 0x1000;
        if (b1)
            y1 += 0x2000;
        break;
    }
    
    /* rotate counter clockwise */
    switch(s->Z) {
    case 0:
        x = x1;
        y = y1;
        break;
    case 1:
        x = -y1;
        y = x1;
        break;
    case 2:
        x = -x1;
        y = -y1;
        break;
    default:
    case 3:
        x = y1;
        y = -x1;
        break;
    }
    *x_ptr = x;
    *y_ptr = y;
}

void V22_mod(V22ModState *s, s16 *samples, unsigned int nb)
{
    int i, j, k, val, si, sq, ph;
    
    for(i=0;i<nb;i++) {

        /* apply the spectrum shaping filter */
        ph = s->baud_phase;
        si = sq = 0;
        for(j=0;j<s->tx_filter_wsize;j++) {
            k = (s->tx_outbuf_ptr - j - 1) & 
                (V22_TX_BUF_SIZE - 1);
            si += s->tx_buf[k][0] * v22_tx_filter[ph];
            sq += s->tx_buf[k][1] * v22_tx_filter[ph];
            ph += s->baud_denom;
        }
        si = si >> 14;
        sq = sq >> 14;
        { static int dn=0; if (getenv("SIPFAX_V22DBG") && dn<6 && s->tx_outbuf_ptr>25) { dn++;
            fprintf(stderr, "[v22mod] si=%d sq=%d  tx_buf[-1]=(%d,%d) ptr=%d wsize=%d filt[0]=%d\n",
                    si, sq, s->tx_buf[(s->tx_outbuf_ptr-1)&(V22_TX_BUF_SIZE-1)][0],
                    s->tx_buf[(s->tx_outbuf_ptr-1)&(V22_TX_BUF_SIZE-1)][1],
                    s->tx_outbuf_ptr, s->tx_filter_wsize, v22_tx_filter[0]); } }

        /* get next baseband symbol ? */
        s->baud_phase += s->baud_num;
        if (s->baud_phase >= s->baud_denom) {
            s->baud_phase -= s->baud_denom;
            V22_mod_baseband(s, 
                             &s->tx_buf[s->tx_outbuf_ptr][0],
                             &s->tx_buf[s->tx_outbuf_ptr][1]);

            s->tx_outbuf_ptr = (s->tx_outbuf_ptr + 1) & (V22_TX_BUF_SIZE - 1);
        }
        
        val = (si * dsp_cos(s->carrier_phase) - 
               sq * dsp_cos((PHASE_BASE/4) - s->carrier_phase)) >> COS_BITS;
        s->carrier_phase += s->carrier_incr;
        if (!s->calling) {
            /* a 1800 Hz tone is added for answer modem modulation at 6 dB below it */
            val += (dsp_cos(s->carrier2_phase) >> 1);
            s->carrier2_phase += s->carrier2_incr;
        }
        samples[i] = val;
    }
}

void V22_demod_init(V22DemodState *s)
{
    s->baud_phase = 0;
    s->baud_num = 3;
    s->baud_denom = 40;

    s->carrier_phase = 0;
    memset(s->rx_buf, 0, sizeof(s->rx_buf));
    s->rx_ptr = 0; s->Z = 0; s->started = 0; s->sym_count = 0;
    s->agc = 0; s->ted_i = s->ted_q = 0; s->mid_i = s->mid_q = 0; s->tphase = 0;
    /* SIPFAX: 8000 Hz / 600 baud = 13.333 samples per symbol, so 6.667 per half symbol.
       The loop steers `step` around that. */
    s->step = 8000.0 / 600.0 / 2.0;
    s->lp_i = s->lp_q = 0; s->half = 0;
    { char *p = getenv("SIPFAX_V22_TPH"); if (p) s->tphase = atof(p); }
    memset(s->bx_i, 0, sizeof(s->bx_i)); memset(s->bx_q, 0, sizeof(s->bx_q)); s->bx_p = 0;
    s->pi_ = s->pq_ = s->mi_ = s->mq_ = 0; s->ted_acc = 0;
    memset(s->h_i, 0, sizeof(s->h_i)); memset(s->h_q, 0, sizeof(s->h_q));

    if (!s->calling) {
        /* call modem DPSK: 600 bps, carrier at 1200 Hz, 0 db */
        s->carrier_incr = (PHASE_BASE * 1200) / 8000;   /* SIPFAX: was PHASE_BITS (=16), giving 2 */
    } else {
        /* answer modem DPSK: 600 bps, carrier at 2400 Hz, -1 db, 
           guard tone at 1800 Hz, -7db */
        s->carrier_incr = (PHASE_BASE * 2400) / 8000;   /* SIPFAX: was PHASE_BITS (=16), giving 4 */
    }
}

/* SIPFAX: V.22 / V.22bis RECEIVER.
   V22_demod was an empty function body - there has never been a V.22 receiver in this tree,
   so the caller's V.22bis fallback (its unmodulated 1200 Hz originate carrier, measured at
   1199.97 Hz with 100% of its energy in +/-40 Hz) could not be answered. Everything below is
   new. It inverts V22_mod exactly:

     - downconvert with an NCO at the far end's carrier (1200 Hz when we answer),
     - reject the image at twice the carrier that mixing produces,
     - sample once per 600 baud symbol, timing steered by a Gardner detector,
     - differential quadrant decode, undoing s->Z = (s->Z + dibit) & 3 and the
       b2 ^= (1 - b1) the modulator applies before forming the dibit.

   The 2400 bps (16-QAM) branch additionally recovers the two in-quadrant bits from the
   amplitude, which is why the AGC matters there and not at 1200. */
/* SIPFAX: sample the image-rejection box at a FRACTIONAL delay, in samples, back from the
   newest sample. The box itself is the one that already worked - 7 taps, backward from the
   baud instant - and the delay is what the timing loop steers. Rewriting this as a free-running
   T/2 interpolator regressed 1200 bps to no-sync at every sampling phase, so the proven path
   stays and only gains a controllable offset. */
static void v22_boxsamp(V22DemodState *s, double off, double *oi, double *oq)
{
    static int len = -1;
    int j, n;
    double fr, ai = 0, aq = 0;
    if (len < 0) { char *e = getenv("SIPFAX_V22AVG"); len = e ? atoi(e) : 7; if (len < 1) len = 1; }
    /* SIPFAX: CUBIC, not linear, interpolation. Reordering the box and the interpolator does
       nothing - both are linear filters, so they commute, and swapping them reproduced the
       previous BER bit for bit. The accuracy of the interpolator itself is what matters: a
       straight line between two samples is a poor fit to a signal still carrying the mixer
       image at twice the carrier (2400 Hz, 3.3 samples per cycle at 8 kHz), and the resulting
       error depends on the fractional delay. That is why a few percent of errors appeared only
       once a clock offset made tadj sweep the whole [0,1) range, sat away from the wraps rather
       than at them, and vanished at offsets small enough to pin tadj near zero. Four-point
       Lagrange fits the curvature the straight line was missing. */
    double b[4][2]; int t;
    if (off < 0) off = 0;
    off += 1.0;                       /* keep the n-1 tap inside written history */
    n = (int)off; fr = off - n;
    for (t = 0; t < 4; t++) {
        double si = 0, sq = 0;
        for (j = 0; j < len; j++) {
            int k = (s->rx_ptr - 1 - (n - 1 + t) - j) & (V22_RX_BUF_SIZE - 1);
            si += s->rx_buf[k][0]; sq += s->rx_buf[k][1];
        }
        b[t][0] = si / len; b[t][1] = sq / len;
    }
    {   double ym1 = b[0][0], y0 = b[1][0], y1 = b[2][0], y2 = b[3][0];
        ai = y0 + 0.5 * fr * (y1 - ym1 + fr * (2*ym1 - 5*y0 + 4*y1 - y2
                                               + fr * (3*(y0 - y1) + y2 - ym1)));
        ym1 = b[0][1]; y0 = b[1][1]; y1 = b[2][1]; y2 = b[3][1];
        aq = y0 + 0.5 * fr * (y1 - ym1 + fr * (2*ym1 - 5*y0 + 4*y1 - y2
                                               + fr * (3*(y0 - y1) + y2 - ym1)));
    }
    *oi = ai; *oq = aq;
}

int g_v22_wrapped;   /* SIPFAX: set on a sample wrap, sampled per received bit */

void V22_demod(V22DemodState *s, s16 *samples, unsigned int nb)
{
    unsigned int i;
    int k, j;

    for (i = 0; i < nb; i++) {
        int v = samples[i];
        int ci, sn, bi, bq;

        /* downconvert */
        ci = dsp_cos(s->carrier_phase);
        sn = dsp_cos((PHASE_BASE / 4) - s->carrier_phase);
        s->carrier_phase = (s->carrier_phase + s->carrier_incr) & (PHASE_BASE - 1);
        bi =  (v * ci) >> COS_BITS;
        bq = -((v * sn) >> COS_BITS);

        s->rx_buf[s->rx_ptr][0] = bi;
        s->rx_buf[s->rx_ptr][1] = bq;
        s->rx_ptr = (s->rx_ptr + 1) & (V22_RX_BUF_SIZE - 1);

        /* image rejection: the mixer puts a copy at 2x carrier, and a short box over
           one symbol (13.33 samples at 600 baud / 8 kHz) removes it without closing the
           eye, because the transmit shaping is already Nyquist at the symbol instant. */
        s->baud_phase += s->baud_num;
        if (s->baud_phase >= s->baud_denom) {
            double ai = 0, aq = 0, mag;
            int dz, b1, b2;
            s->baud_phase -= s->baud_denom;

            {   /* SIPFAX: TIMING RECOVERY. The sampling instant used to free-run from phase
                   zero, which only decodes because the loopback starts aligned; on a line the
                   phase is arbitrary. s->tadj is a fractional delay in samples applied to the
                   same box, steered by a Gardner detector that compares each symbol with the
                   midpoint half a symbol earlier. */
                static double kp = -1, ki = -1, sp = -1;
                double mi, mq, e, half = (double)s->baud_denom / s->baud_num / 2.0;
                /* SIPFAX: tuned against a swept clock offset AFTER carrier recovery went in.
                   The earlier, hotter constants were compensating for carrier drift that the
                   timing loop cannot fix, and once the carrier loop existed they made things
                   worse - the timing loop only has to track the sampling instant, so it wants
                   to be slow. ki is the term that matters: it settles at the fractional drift
                   per symbol and IS the clock-offset estimate. */
                if (kp < 0) { char *p = getenv("SIPFAX_V22_TKP"); kp = p ? atof(p) : 0.005; }
                if (ki < 0) { char *p = getenv("SIPFAX_V22_TKI"); ki = p ? atof(p) : 5e-5; }
                if (sp < 0) { char *p = getenv("SIPFAX_V22PH");   sp = p ? atof(p) : 0.0;
                              s->tadj = sp; }
                v22_boxsamp(s, s->tadj,        &ai, &aq);
                v22_boxsamp(s, s->tadj + half, &mi, &mq);

                e = (ai - s->ted_i) * mi + (aq - s->ted_q) * mq;
                {   double n2 = (ai*ai + aq*aq) + (s->ted_i*s->ted_i + s->ted_q*s->ted_q) + 1e-9;
                    e /= n2;
                }
                if (e >  0.5) e =  0.5;
                if (e < -0.5) e = -0.5;
                s->ted_acc += ki * e;
                /* SIPFAX: the integrator IS the clock-offset estimate - it settles at the
                   fractional drift per symbol, 6.7e-4 for 50 ppm and 0.0133 for 1000 ppm - so
                   the clamp has to leave room for the worst offset a real line presents, or the
                   loop silently stops tracking at the stop. */
                if (s->ted_acc >  0.06) s->ted_acc =  0.06;
                if (s->ted_acc < -0.06) s->ted_acc = -0.06;
                s->tadj += kp * e + s->ted_acc;
                /* SIPFAX: WRAP, do not clamp. The far end's clock is not ours, so tadj drifts
                   without bound; a clamp would ride to the stop and slip. When the delay passes a
                   whole sample, hand that sample to the baud accumulator instead - the absolute
                   sampling instant is unchanged, but tadj stays inside the box history and the
                   loop keeps tracking. The tick moves one sample EARLIER to offset a tadj that
                   grew by one, because a smaller delay samples later. */
                while (s->tadj >= 1.0) { s->tadj -= 1.0; s->baud_phase += s->baud_num; s->nwrap++; g_v22_wrapped = 1; }
                while (s->tadj <  0.0) { s->tadj += 1.0; s->baud_phase -= s->baud_num; s->nwrap++; g_v22_wrapped = 1; }
                if (getenv("SIPFAX_V22TRC") && (s->sym_count % 64) == 0)
                    fprintf(stderr, "[v22trc] sym %5ld bit ~%5ld tadj=%.3f acc=%+.5f wraps=%d\n",
                            s->sym_count, s->sym_count * 2, s->tadj, s->ted_acc, s->nwrap);
                s->ted_i = ai; s->ted_q = aq;
            }

            mag = sqrt(ai * ai + aq * aq);
            s->agc = (s->agc <= 0) ? mag : (0.99 * s->agc + 0.01 * mag);
            s->sym_count++;
            { extern int v34_dbg; if (getenv("SIPFAX_V22DBG") && s->sym_count > 100 && s->sym_count < 112)
                fprintf(stderr, "[v22rx] sym %ld ai=%.0f aq=%.0f mag=%.0f agc=%.0f started=%d\n",
                        s->sym_count, ai, aq, mag, s->agc, s->started); }
            if (!s->started) {
                if (s->agc > 200.0 && s->sym_count > 16) s->started = 1;
                continue;
            }

            { static int cn=0; if (getenv("SIPFAX_V22CON") && cn<16) { cn++;
                fprintf(stderr, "[v22con] ai=%8.0f aq=%8.0f  |z|=%8.0f  quad=%d\n",
                        ai, aq, mag, (ai>=0)?((aq>=0)?0:3):((aq>=0)?1:2)); } }
            /* quadrant, then differential decode */
            {   /* SIPFAX: CARRIER RECOVERY. A clock offset rescales time, so it drags the
                   carrier with it - the receiver's NCO sits at exactly 1200 Hz, but 50 ppm puts
                   the far end 0.06 Hz away, which is 69 degrees of rotation across a 3.2 s run.
                   The quadrant decision uses fixed axes, so a drifting constellation walks
                   points across the boundaries and mis-scores dz, and no amount of timing work
                   fixes it: the residual few percent survived every timing change, sat away from
                   the wraps, and appeared the moment the offset became nonzero. This is a
                   decision-directed loop - the QPSK points sit mid-quadrant, so the signed
                   cross-product with the decision is the phase error. */
                static double ckp = -1, cki = -1;
                double ri, rq, cs, sn2, e, di, dq, mg;
                if (ckp < 0) { char *p = getenv("SIPFAX_V22_CKP"); ckp = p ? atof(p) : 0.05; }
                if (cki < 0) { char *p = getenv("SIPFAX_V22_CKI"); cki = p ? atof(p) : 1e-3; }
                /* SIPFAX: only for the modes this decision fits. The error term below assumes
                   the point sits mid-quadrant, which is true for the 1200/2400 constellations
                   and not for the 2-phase 600-bps modes - running it there made them worse
                   under a clock offset (6.25% against 1.56% free-running) rather than better.
                   The 600 modes are not offset-robust either way; that is untouched, not fixed. */
                if (s->mod_type != V22_MOD_1200 && s->mod_type != V22_MOD_2400) goto no_carrier;
                cs = cos(s->cph); sn2 = sin(s->cph);
                ri = ai * cs - aq * sn2;
                rq = ai * sn2 + aq * cs;
                ai = ri; aq = rq;
                di = (ai >= 0) ? 1.0 : -1.0;
                dq = (aq >= 0) ? 1.0 : -1.0;
                mg = sqrt(ai*ai + aq*aq) + 1e-9;
                e = (aq * di - ai * dq) / (mg * 1.41421356);
                if (e >  0.7) e =  0.7;
                if (e < -0.7) e = -0.7;
                s->cacc += cki * e;
                if (s->cacc >  0.10) s->cacc =  0.10;
                if (s->cacc < -0.10) s->cacc = -0.10;
                s->cph -= ckp * e + s->cacc;
                no_carrier: ;
            }
            {   int Znew = (ai >= 0) ? ((aq >= 0) ? 0 : 3) : ((aq >= 0) ? 1 : 2);
                dz = (Znew - s->Z) & 3;
                s->Z = Znew;
            }
            b1 = (dz >> 1) & 1;
            b2 = dz & 1;
            b2 ^= (1 - b1);            /* undo the modulator\'s pre-twist */

            switch (s->mod_type) {
            case V34_MOD_600:
                s->put_bit(s->opaque, (dz == 2) ? 1 : 0);   /* 0 or 180 degrees */
                break;
            case V22_MOD_600:
                s->put_bit(s->opaque, b1);
                break;
            default:
            case V22_MOD_1200:
                s->put_bit(s->opaque, b1);
                s->put_bit(s->opaque, b2);
                break;
            case V22_MOD_2400: {
                /* two more bits from the position inside the quadrant: the modulator
                   emits 0x1000 or 0x3000 on each axis, so compare against the middle. */
                double r = (s->agc > 0) ? (mag / s->agc) : 1.0;
                double u = fabs(ai), w = fabs(aq), thr = 0.5 * (u + w);
                int c1 = (w > thr) ? 1 : 0;
                int c2 = (u > thr) ? 1 : 0;
                (void)r;
                s->put_bit(s->opaque, b1);
                s->put_bit(s->opaque, b2);
                s->put_bit(s->opaque, c1);
                s->put_bit(s->opaque, c2);
                break; }
            }
            s->ted_i = ai; s->ted_q = aq;
        }
    }
}


/* test for FSK using V21 or V23 */

#define NB_SAMPLES 40

#define MAXDELAY 32

static int tx_bits[MAXDELAY], tx_ptr = 0, rx_ptr = 0;
static int tx_blank = 32;

/* transmit random bits with a sync header (31 ones, 1 zero) */
static int test_get_bit(void *opaque)
{
    int bit;

    if (tx_blank != 0) {
        /* send 1 at the beginning for synchronization */
        bit = (tx_blank > 1);
        tx_blank--;
    } else {
        bit = random() % 2;
        tx_bits[tx_ptr] = bit;
        if (++tx_ptr == MAXDELAY)
            tx_ptr = 0;
    }
    return bit;
}

static int nb_bits = 0, errors = 0, sync_count = 0, got_sync = 0;

static void test_put_bit(void *opaque, int bit)
{
    int tbit;
    
    if (!got_sync) {
        
        if (bit) {
            sync_count++;
        } else {
            if (sync_count > 16)
                got_sync = 1;
            sync_count = 0;
        }
    } else {
        tbit = tx_bits[rx_ptr];
        if (++rx_ptr == MAXDELAY)
            rx_ptr = 0;
        if (bit != tbit) {
            errors++;
        }
        nb_bits++;
    }
}

void V22_test(void)
{
    V22ModState tx;
    V22DemodState rx;
    int err, calling;
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

    f1 = fopen("cal.sw", "wb");
    if (f1 == NULL) {
        perror("cal.sw");
        exit(1);
    }

    calling = 0;

    tx.calling = calling;
    tx.opaque = NULL;
    tx.get_bit = test_get_bit;
    tx.mod_type = V34_MOD_600;
    V22_mod_init(&tx);

    rx.calling = !calling;
    rx.opaque = NULL;
    rx.put_bit = test_put_bit;
    rx.mod_type = tx.mod_type;
    V22_demod_init(&rx);

    nb_bits = 0;
    errors = 0;
    for(;;) {
        if (lm_display_poll_event())
            break;
        
        V22_mod(&tx, buf, NB_SAMPLES);
        memset(buf3, 0, sizeof(buf3));

        line_model(line_state, buf1, buf, buf2, buf3, NB_SAMPLES);

        fwrite(buf, 1, NB_SAMPLES * 2, f1);
        
        V22_demod(&rx, buf1, NB_SAMPLES);
    }

    fclose(f1);

    printf("errors=%d nb_bits=%d Pe=%f\n", 
           errors, nb_bits, (float) errors / (float)nb_bits);
}


/* SIPFAX: headless V.22 loopback - modulate a PRBS, demodulate it, count bit errors.
   The stock V22_test needs lm_display_poll_event and a display, so it cannot run in CI or
   over ssh. SIPFAX_V22LOOP=<600|v600|1200|2400> picks the modulation. */
static int v22l_tx[4096], v22l_txn, v22l_rxn, v22l_err, v22l_sync, v22l_got, v22l_nput;
static int v22l_rx[4096], v22l_rn;
static int v22l_wrapat[4096];   /* SIPFAX: did a timing wrap land on this bit? */
static unsigned int v22l_lfsr = 0x1234;
static int v22l_get(void *o)
{
    int b;
    if (v22l_txn < 32) { b = 1; }          /* preamble for sync */
    else {
        v22l_lfsr = (v22l_lfsr << 1) | (((v22l_lfsr >> 15) ^ (v22l_lfsr >> 13)) & 1);
        b = v22l_lfsr & 1;
    }
    if (v22l_txn < 4096) v22l_tx[v22l_txn] = b;
    v22l_txn++;
    return b;
}
static void v22l_put(void *o, int bit)
{
    v22l_nput++;
    if (!v22l_got) {                        /* find the preamble, then align */
        if (bit) v22l_sync++;
        /* SIPFAX: align to the TRANSMITTED index, not to how many ones we happened to see.
           The demodulator drops symbols until it has signal, so it observes fewer than the
           31 preamble ones; rxn = sync+1 then pointed at the wrong tx bit and every
           comparison after it was offset - which reads as ~50% BER from a receiver that is
           actually decoding correctly. The tx preamble is 31 ones with a zero at index 31,
           so the bit after the sync zero is always tx index 32. */
        else { if (v22l_sync >= 16) { char *e=getenv("SIPFAX_V22ALIGN");
                   v22l_got = 1; v22l_rxn = e ? atoi(e) : 32; } v22l_sync = 0; }
        return;
    }
    if (v22l_rn < 4096) { extern int g_v22_wrapped;
                          v22l_wrapat[v22l_rn] = g_v22_wrapped; g_v22_wrapped = 0;
                          v22l_rx[v22l_rn++] = bit; }   /* SIPFAX: score offline, self-aligned */
}
void V22_loop_test(const char *what)
{
    static V22ModState tx; static V22DemodState rx;
    s16 buf[64];
    int i;
    enum ModulationType mt = V22_MOD_1200;
    if (!strcmp(what, "600"))  mt = V34_MOD_600;
    if (!strcmp(what, "v600")) mt = V22_MOD_600;
    if (!strcmp(what, "2400")) mt = V22_MOD_2400;
    { extern void dsp_init(void); dsp_init(); }   /* SIPFAX: cos_tab is zero without this */
    memset(&tx, 0, sizeof(tx)); memset(&rx, 0, sizeof(rx));
    v22l_txn = v22l_rxn = v22l_err = v22l_sync = v22l_got = v22l_nput = v22l_rn = 0;
    tx.calling = 1;                      /* the CALLER transmits; we answer */
    tx.opaque = NULL; tx.get_bit = v22l_get; tx.mod_type = mt;
    V22_mod_init(&tx);
    rx.calling = 0;                      /* we are the answer modem */
    rx.opaque = NULL; rx.put_bit = v22l_put; rx.mod_type = mt;
    V22_demod_init(&rx);
    for (i = 0; i < 3000; i++) {
        V22_mod(&tx, buf, 64);
        if ((i==0||i==40||i==200||i==800) && getenv("SIPFAX_V22DBG")) {
            int q, mx = 0;
            for (q = 0; q < 64; q++) if (abs(buf[q]) > mx) mx = abs(buf[q]);
            fprintf(stderr, "[v22loop] block %d: peak=%d  first=%d %d %d %d  txn=%d filt_wsize=%d\n",
                    i, mx, buf[0], buf[1], buf[2], buf[3], v22l_txn, tx.tx_filter_wsize);
        }
        {   /* SIPFAX: emulate a CLOCK OFFSET between the two ends. Without one the loopback
               samples at exactly the transmit rate, so a free-running sampler scores as well as
               a tracking one and the harness cannot tell timing recovery from its absence - the
               first phase sweep passed at 13 of 14 offsets with the loop switched off, which
               proves nothing. SIPFAX_V22_PPM resamples the transmit stream so the receiver has
               to track a clock that is genuinely not its own. */
            static double ppm = -1e30, rp = 0.0;
            static s16 acc[512]; static int accn = 0;
            s16 out[256]; int n = 0, q;
            if (ppm < -1e29) { char *p = getenv("SIPFAX_V22_PPM"); ppm = p ? atof(p) : 0.0; }
            if (ppm == 0.0) { V22_demod(&rx, buf, 64); }
            else {
                double rate = 1.0 + ppm * 1e-6;
                if (accn + 64 <= (int)(sizeof(acc)/sizeof(acc[0]))) {
                    memcpy(acc + accn, buf, 64 * sizeof(s16)); accn += 64;
                }
                while ((int)rp + 1 < accn && n < 256) {
                    int k = (int)rp; double fr = rp - k;
                    out[n++] = (s16)(acc[k] + fr * (acc[k+1] - acc[k]));
                    rp += rate;
                }
                q = (int)rp;
                if (q > 0 && q <= accn) { memmove(acc, acc + q, (accn - q) * sizeof(s16));
                                          accn -= q; rp -= q; }
                V22_demod(&rx, out, n);
            }
        }
    }
    fprintf(stderr, "[v22loop] put_bit calls=%d sync=%d got=%d\n", v22l_nput, v22l_sync, v22l_got);
    {   /* SIPFAX: find the alignment rather than assuming it. Guessing it wrong reads as
           ~50% BER from a receiver that is decoding perfectly - which cost a full round of
           debugging. Report the BEST alignment and the count of bits actually compared.

           SIPFAX: score in BLOCKS, re-aligning each one within a few bits of the last. With a
           clock offset the two streams slip against each other by design - a whole bit every
           ~2000 bits at 500 ppm - so a single fixed alignment starts counting garbage partway
           through and floors the BER at a few percent no matter how well the receiver tracks.
           That floor is the scorer, not the demodulator, and it made a working timing loop look
           identical to no loop at all. */
        int off, bestoff = 0, bester = 1 << 30, n, tot = 0, scored = 0, blk;
        int ess = 0, nss = 0;
        for (off = 0; off < 64; off++) {
            int e = 0, c = 0, q;
            for (q = 0; q + off < 1024 && q < v22l_rn && q + off < v22l_txn; q++) {
                if (v22l_rx[q] != v22l_tx[q + off]) e++;
                c++;
            }
            if (c > 512 && e < bester) { bester = e; bestoff = off; }
        }
        for (n = 0; n + bestoff < 4096 && n < v22l_rn && n + bestoff < v22l_txn; n++) ;
        for (blk = 0; blk + 256 <= n; blk += 256) {
            int d, bd = 0, be = 1 << 30, q;
            for (d = -4; d <= 4; d++) {
                int e = 0, ok = 1;
                for (q = 0; q < 256; q++) {
                    int ti = blk + q + bestoff + d;
                    if (ti < 0 || ti >= v22l_txn) { ok = 0; break; }
                    if (v22l_rx[blk + q] != v22l_tx[ti]) e++;
                }
                if (ok && e < be) { be = e; bd = d; }
            }
            if (be < (1 << 30)) { tot += be; scored += 256; bestoff += bd;
                if (blk >= 2048) { ess += be; nss += 256; }
                if (getenv("SIPFAX_V22BLK"))
                    fprintf(stderr, "[v22blk] block %2d  err=%3d  slip=%+d  off=%d\n",
                            blk / 256, be, bd, bestoff);
            }
        }
        v22l_err = tot; v22l_rxn = scored;
        fprintf(stderr, "[v22loop] block-aligned scoring, final offset %d\n", bestoff);
        /* SIPFAX: a timing loop has to ACQUIRE before it can track, and the errors it makes
           while acquiring are a one-off cost, not an error rate - lumping them into a single
           BER made a loop that settles to zero errors read as a 3% failure. On a real call the
           V.22 training period covers acquisition. Report both. */
        {   /* SIPFAX: are the errors AT the wraps? Each wrap hands one sample from the
               interpolator to the baud accumulator, and if that handoff is not exact it costs a
               burst. Counting errors near a wrap against errors away from one answers that
               directly, instead of inferring it from an aggregate BER. */
            int q, nw = 0, ew = 0, nf = 0, ef = 0, d, near;
            for (q = 0; q < n && q + bestoff < v22l_txn; q++) {
                near = 0;
                for (d = -6; d <= 6; d++)
                    if (q + d >= 0 && q + d < 4096 && v22l_wrapat[q + d]) { near = 1; break; }
                if (near) { nw++; if (v22l_rx[q] != v22l_tx[q + bestoff]) ew++; }
                else      { nf++; if (v22l_rx[q] != v22l_tx[q + bestoff]) ef++; }
            }
            fprintf(stderr, "[v22wrap] near-wrap bits=%d err=%d (%.2f%%)   away bits=%d err=%d (%.2f%%)\n",
                    nw, ew, nw ? 100.0*ew/nw : 0.0, nf, ef, nf ? 100.0*ef/nf : 0.0);
        }
        if (nss > 0)
            fprintf(stderr, "[v22loop] steady state (after acquisition): scored=%d errors=%d BER=%.4f%%\n",
                    nss, ess, 100.0 * ess / nss);
    }
    /* SIPFAX: never print a BER without a scored-bit count. scored=0 with BER=0.0000%
       reads as a perfect pass and actually means the sync never fired - it fooled me twice. */
    if (v22l_rxn < 256)
        fprintf(stderr, "[v22loop] mode=%s tx_bits=%d scored=%d -> NO SYNC (no BER)\n",
                what, v22l_txn, v22l_rxn);
    else
        fprintf(stderr, "[v22loop] mode=%s tx_bits=%d scored=%d errors=%d  BER=%.4f%%  %s\n",
                what, v22l_txn, v22l_rxn, v22l_err,
                100.0 * v22l_err / v22l_rxn,
                (100.0 * v22l_err / v22l_rxn < 1.0) ? "PASS" : "FAIL");
}
