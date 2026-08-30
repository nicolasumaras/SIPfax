/* 
 * V8 protocol handler
 * 
 * Copyright (c) 1999,2000 Fabrice Bellard.
 *
 * This code is released under the GNU General Public License version
 * 2. Please read the file COPYING to know the exact terms of the
 * license.
 * 
 * This implementation is totally clean room. It was written by
 * reading the ITU specification and by using basic signal processing
 * knowledge.  
 */
#include "lm.h"

static void V8_mod_init(V8_mod_state *s)
{
    s->sample_rate = V8_SAMPLE_RATE;

    /* ANSam tone: 2100 Hz, amplitude modulated at 15 Hz, with phase
       reversal every 450 ms */
    s->phase = 0;
    s->phase_incr = (int) (PHASE_BASE * 2100.0 / s->sample_rate);
    s->mod_phase = 0;
    s->mod_phase_incr = (int) (PHASE_BASE * 15.0 / s->sample_rate);
    s->phase_reverse_samples = (int) (s->sample_rate * 0.450);
    s->phase_reverse_left = 0;
    /* XXX: incorrect power */
    s->amp = (int) (pow(10, s->tone_level / 20.0) * 32768.0);
}

static void V8_mod(V8_mod_state *s, s16 *samples, unsigned int nb)
{
    int amp,i;

    for(i=0;i<nb;i++) {
        /* handle phase reversal every 450 ms */
        if (s->phase_reverse_left == 0) {
            s->phase_reverse_left = s->phase_reverse_samples;
            s->phase += PHASE_BASE / 2;
        }

        amp = (dsp_cos(s->mod_phase) * (int)(0.2 * COS_BASE)) >> COS_BITS;
        amp += COS_BASE; /* between 0.8 and 1.2 */
        /* scale to ~-23 dBFS to match a real answer-tone level (was ~10 dB hot,
           which overloaded the calling modem's V.8 detector) */
        samples[i] = ((amp * dsp_cos(s->phase)) >> COS_BITS) / 5;
        s->mod_phase += s->mod_phase_incr;
        s->phase += s->phase_incr;
        s->phase_reverse_left--;   /* FIX: count down to the next 450ms phase reversal */
    }
}

/* Recognize the V8 ANSam tone. Some other tones (in particular V21
   tone) may be added later. We compute the DFT for every interesting
   frequency and do a threshold with the power. Not the best method,
   but easy to implement and quite reliable. */

static void V8_demod_init(V8_demod_state *s)
{
    int i;

    for(i=0;i<V8_N;i++) {
        s->cos_tab[i] = (int) (cos( 2 * M_PI * i / V8_N) * COS_BASE);
        s->sin_tab[i] = (int) (sin( 2 * M_PI * i / V8_N) * COS_BASE);
    }

    s->buf_ptr = 0;
    s->v8_ANSam_detected = 0;
}

static void V8_demod(V8_demod_state *s, const s16 *samples, unsigned int nb)
{
    int i, p0, p1;

    for(i=0;i<nb;i++) {
        s->buf[s->buf_ptr++] = samples[i];
        if (s->buf_ptr >= V8_N) {
            s->buf_ptr = 0;
            dsp_sar_tab(s->buf, V8_N, 8);
            p0 = dsp_norm2(s->buf, V8_N, 0);
            p1 = compute_DFT(s->cos_tab, s->sin_tab, s->buf, DFT_COEF_2100, V8_N);
            /* XXX: this test is incorrect (not homogenous) */
            if ((p0 > 1000) && (p1 > (5*p0))) {
                /* V8 tone recognized */
                s->v8_ANSam_detected = 1;
            }
        }
    }
}

/* V8 stream decoding */
static void ci_decode(V8State *s)
{
    int data = s->rx_data[0];
    if (data == 0x83) {
        fprintf(stderr, "CI: data call\n");
    } 
}

/* CM or JM decoding */
static void cm_decode(V8State *s)
{
    u8 *p;
    int c;

    if (s->got_cm)
        return;

    if (s->cm_count > 0) {
        /* we must receive two identical CM sequences */
        if (s->cm_count == s->rx_data_ptr &&
            !memcmp(s->cm_data, s->rx_data, s->rx_data_ptr)) {
            /* got CM !! */
            s->got_cm = 1;
            /* decode it */
            /* XXX: this decoding is sufficient for modulations, but
               not exhaustive */
            s->decoded_modulations = 0;
            p = s->cm_data;
            /* zero is used to indicate the end */
            s->cm_data[s->cm_count] = 0;

            c = *p++;
            /* call function */
            if ((c & 0xf8) != 0x80) 
                return;
            if (c != V8_CALL_FUNC_DATA)
                return;

            /* modulation */
            c = *p++;
            if ((c & 0xf8) != V8_MODN0) 
                return;

            if (c & V8_MODN0_V90)
                s->decoded_modulations |= V8_MOD_V90;
            if (c & V8_MODN0_V34)
                s->decoded_modulations |= V8_MOD_V34;

            c = *p++;
            if ((c & 0x1c) == V8_EXT) {
                /* ignored */
                c = *p++;
                if ((c & 0x1c) == V8_EXT) {
                    if (c & V8_MODN2_V23)
                        s->decoded_modulations |= V8_MOD_V23;
                    if (c & V8_MODN2_V21)
                        s->decoded_modulations |= V8_MOD_V21;
                    /* skip other extensions */
                    do {
                        c = *p++;
                    } while ((c & 0x1c) == V8_EXT);
                }
            }
            return;
        }
    }
            
    /* save the current CM sequence */
    s->cm_count = s->rx_data_ptr;
    memcpy(s->cm_data, s->rx_data, s->rx_data_ptr);
}

static void put_bit(void *opaque, int bit)
{
    V8State *s = opaque;
    int new_state, i;

    /* wait ten ones & synchro */
    s->bit_sync = ((s->bit_sync << 1) | bit) & ((1 << 20) - 1);
    if (s->bit_sync == ((V8_TEN_ONES << 10) | V8_CI_SYNC)) {
        new_state = V8_CI_SYNC;
        goto data_init;
    } else if (s->bit_sync == ((V8_TEN_ONES << 10) | V8_CM_SYNC)) {
        new_state = V8_CM_SYNC;
    data_init:
        /* debug */
        if (s->data_state == V8_CI_SYNC) {
            fprintf(stderr, "CI: ");
        } else if (s->data_state == V8_CM_SYNC) {
            if (s->calling)
                fprintf(stderr, "JM: ");
            else
                fprintf(stderr, "CM: ");
        }
        for(i=0;i<s->rx_data_ptr;i++) fprintf(stderr, " %02x", s->rx_data[i]);
        fprintf(stderr, "\n");
        
        /* decode previous sequence */
        switch(s->data_state) {
        case V8_CI_SYNC:
            ci_decode(s);
            break;
        case V8_CM_SYNC:
            cm_decode(s);
            break;
        }
        s->data_state = new_state;
        s->bit_buf = 0;
        s->bit_cnt = 0;
        s->rx_data_ptr = 0;
    }
    
    /* parse octets with 1 bit start, 1 bit stop */
    s->bits_since_octet++;
    if (s->data_state) {
        s->bit_buf = ((s->bit_buf << 1) | bit) & ((1 << 10) - 1);
        s->bit_cnt++;
        /* start, stop ? */
        if ((s->bit_buf & 0x201) == 0x001 && s->bit_cnt >= 10) {
            int data;
            /* store the available data */
            data = (s->bit_buf >> 1) & 0xff;
            fprintf(stderr, "[v8] rx data st=%d %02x\n", s->data_state, data); fflush(stderr);
            /* CJ detection */
            if (data == 0) {
                if (++s->data_zero_count == 3) {
                    s->got_cj = 1;
                    fprintf(stderr, "[v8] got CJ\n"); fflush(stderr);
                }
            } else {
                s->data_zero_count = 0;
            }

            if (s->rx_data_ptr < (sizeof(s->rx_data)-1)) {
                s->rx_data[s->rx_data_ptr++] = data;
            }
            s->bits_since_octet = 0;

            /* Some modems send ten-ones+sync ONCE and then repeat the CM payload
               cycle with no re-sync -- cm_decode (sync-triggered) never runs and V.8
               hangs in CM_WAIT. Detect a repeating payload cycle directly: 3 identical
               L-cycles ending at the stream head; rotate to the call-function octet
               (0x8x) and feed the existing decoder. */
            if (!s->got_cm && s->data_state == V8_CM_SYNC && !s->calling && s->rx_data_ptr >= 9) {
                int L, k, st;
                u8 *e = s->rx_data + s->rx_data_ptr;
                for (L = 3; L <= 10; L++) {
                    if (s->rx_data_ptr < 3*L) continue;
                    if (memcmp(e-L, e-2*L, L) || memcmp(e-2*L, e-3*L, L)) continue;
                    for (st = 0; st < L; st++) if ((e[-L+st] & 0xf8) == 0x80) break;
                    if (st == L) continue;
                    for (k = 0; k < L; k++) s->cm_data[k] = e[-L + ((st + k) % L)];
                    s->cm_count = L;
                    memcpy(s->rx_data, s->cm_data, L);
                    s->rx_data_ptr = L;
                    fprintf(stderr, "[v8] CM payload cycle detected (L=%d, rot=%d) -> decode\n", L, st); fflush(stderr);
                    cm_decode(s);
                    break;
                }
            }

            s->bit_cnt = 0;
        }
    }
}

static void v8_decode_init(V8State *s)
{
    V21_demod_init(&s->v21_rx, s->calling, put_bit, s);
    s->data_state = 0;
    s->bit_sync = 0;
    s->cm_count = 0;
    s->got_cm = 0;

    s->got_cj = 0;
    s->data_zero_count = 0;
    s->rx_data_ptr = 0;
}


static int get_bit(void *opaque)
{
    V8State *s = opaque;
    int bit;

    bit = sm_get_bit(&s->tx_fifo);
    if (bit < 0)
        bit = 1;
    return bit;
}

static void v8_put_byte(V8State *s, int data)
{
    /* insert start & stop bits */
    sm_put_bits(&s->tx_fifo, ((data & 0xff) << 1) | 1, 10);
}


void V8_init(V8State *sm, int calling, int mod_mask)
{
    sm->debug_laststate = -1;
    sm->calling = calling;
    if (sm->calling) {
        sm->state = V8_WAIT_1SECOND;
        sm_set_timer(&sm->v8_start_timer, 1000);
    } else {
        /* wait 200 ms */
        {   /* SIPFAX: V.25 requires the answering DCE to hold 1.8-2.5 s of silence after
           connection before the answer tone; the old 200 ms violated it (slmodem waits
           2.03 s). ANSam's phase reversals are also the network echo-canceller disable
           signal, so its placement matters to path equipment, not just the peer.
           SIPFAX_ANSAM_DELAY_MS overrides. */
        int d = 1900; char *e = getenv("SIPFAX_ANSAM_DELAY_MS");
        if (e && atoi(e) > 0) d = atoi(e);
        sm_set_timer(&sm->v8_connect_timer, d);
    }
        sm->state = V8_WAIT;
    }
    sm_init_fifo(&sm->rx_fifo, sm->rx_buf, sizeof(sm->rx_buf));
    sm_init_fifo(&sm->tx_fifo, sm->tx_buf, sizeof(sm->tx_buf));
    sm->modulation_mask = mod_mask;
}

/* send CM or JM */
static void cm_send(V8State *s, int mod_mask)
{
    int val;

    sm_put_bits(&s->tx_fifo, V8_TEN_ONES, 10);
    sm_put_bits(&s->tx_fifo, V8_CM_SYNC, 10);
    
    /* data call */
    v8_put_byte(s, V8_CALL_FUNC_DATA);
    
    /* supported modulations */
    val = V8_MODN0;
    if (mod_mask & V8_MOD_V90)
        val |= V8_MODN0_V90;
    if (mod_mask & V8_MOD_V34)
        val |= V8_MODN0_V34;
    v8_put_byte(s, val);
    /* SIPFAX: make our JM byte-identical to slmodem's, which this caller accepts and
       completes a call against: wire C1 45 11 10 2A 0D. Bit-exact V.8 forensics of the
       working vs failing captures showed our old JM (C1 45 10 94) differed in three
       ways - no V.32/V.32bis claim, spurious V.23/V.21 claims, and, the meaningful
       ones, NO protocols octet (LAPM/V.42) and NO GSTN-access octet, both commented
       out below since Bellard's day. A caller firmware that gates post-handshake
       behaviour on "answerer declared V.42" would explain a Phase-4 refusal that
       survives every signal-level comparison. v8_put_byte transmits LSB-first, so code
       constants are the bit-reverse of the wire octets (V8_DATA_LAPM=0x54 -> wire 2A,
       V8_DATA_NOCELULAR=0xB0 -> wire 0D, checked against slmodem's decoded JM). */
    v8_put_byte(s, V8_EXT | 0x80);      /* wire 0x11: V.32/V.32bis, as slmodem */
    v8_put_byte(s, V8_EXT);             /* wire 0x10: no second-ext claims, as slmodem */
    v8_put_byte(s, V8_DATA_LAPM);       /* wire 0x2A: LAPM (V.42) */
    v8_put_byte(s, V8_DATA_NOCELULAR);  /* wire 0x0D: GSTN standard analogue */
}

/* selection the modulation according to V8 priority from the bits in 'mask' */
static int select_modulation(int mask)
{
    int val;

    val = V8_MOD_HANGUP;
    /* use modulations in this order */
    if (mask & V8_MOD_V21)
        val = V8_MOD_V21;
    if (mask & V8_MOD_V23)
        val = V8_MOD_V23;
    if (mask & V8_MOD_V34)
        val = V8_MOD_V34;
    if (mask & V8_MOD_V90)
        val = V8_MOD_V90;
    return val;
}


/* V8 protocol handler */
/* SIPFAX: V.8 TIMING. The [phase] log added for phases 3/4 starts at V.34 entry, so the
   first 5 s of every call - all of V.8 - has never been accounted for. It matters: we reach
   data mode at t=15.5 s and this caller abandons at t=16.0 s, while slmodem gets there at
   t=14.0 s, so the whole problem is a 1.5 s deficit and V.8 is the largest unexamined block.
   Timestamp every state change off the V.8 sample clock and record when the caller's CM and
   CJ actually arrive, so the time can be attributed to us or to the caller instead of guessed
   at. JM_SEND in particular has a deliberate 400 ms floor (8.2.3 needs >=2 JM sequences before
   a CJ is credible) and a comment claiming the peer always CJs at ~3 s - this measures whether
   that is so. */
long g_v8samp = 0;
int V8_process(V8State *s, s16 *output, s16 *input, int nb_samples)
{
    g_v8samp += nb_samples;
    {   extern int v34_dbg;
        static int last = -1; static long prev = 0, cm_at = -1, cj_at = -1;
        if (v34_dbg) {
            if (s->got_cm && cm_at < 0) {
                cm_at = g_v8samp;
                fprintf(stderr, "[v8t] t=%7.3fs  caller CM received\n", cm_at/8000.0);
                fflush(stderr);
            }
            if (s->got_cj && cj_at < 0) {
                cj_at = g_v8samp;
                fprintf(stderr, "[v8t] t=%7.3fs  caller CJ received (%.3fs after CM)\n",
                        cj_at/8000.0, cm_at >= 0 ? (cj_at-cm_at)/8000.0 : -1.0);
                fflush(stderr);
            }
            if (s->state != last) {
                fprintf(stderr, "[v8t] t=%7.3fs  %-16s -> %-16s (held %6.3fs)\n",
                        g_v8samp/8000.0,
                        last < 0 ? "start" : sm_states_str[last],
                        sm_states_str[s->state], (g_v8samp - prev)/8000.0);
                fflush(stderr); prev = g_v8samp; last = s->state;
            }
        }
    }
    int ret = 0;

    /* modulation part */
    switch (s->state) {
    case V8_CI_SEND:
    case V8_CM_SEND:
    case V8_JM_SEND:
    case V8_CJ_SEND:
        /* modulate with V21 */
        FSK_mod(&s->v21_tx, output, nb_samples);
        break;

    case V8_CM_WAIT:
        /* send ANSam modulation */
        V8_mod(&s->v8_tx, output, nb_samples);
        break;

    default:
        /* output nothing */ 
        memset(output, 0, nb_samples * sizeof(s16));
        break;
    }

    /* demodulation part */
    switch (s->state) {
    case V8_CI:
    case V8_CI_OFF:
    case V8_CI_SEND:
        /* detect ANSam */
        V8_demod(&s->v8_rx, input, nb_samples);
        break;
        
    case V8_CM_WAIT:
    case V8_CM_SEND:
    case V8_JM_SEND:
        /* V21 receive */
        FSK_demod(&s->v21_rx, input, nb_samples);
        break;
        
    default:
        break;
    }

    /* state machine */

    if (s->state != s->debug_laststate) {
        s->debug_laststate = s->state;
        fprintf(stderr, "[v8] %s %s (got_cm=%d got_cj=%d mods=0x%x)\n",
               s->calling ? "cal" : "ans", sm_states_str[s->state],
               s->got_cm, s->got_cj, s->decoded_modulations);
        fflush(stderr);
    }

    switch(s->state) {

    case V8_WAIT_1SECOND:
        {
            /* wait 1 second before sending the first CI packet */
            if (sm_check_timer(&s->v8_start_timer)) {
                s->state = V8_CI;
                s->v8_ci_count = 0;
                V8_demod_init(&s->v8_rx); /* init ANSam detection */
                V21_mod_init(&s->v21_tx, 1, get_bit, s);
            }
        }
        break;

        /* send the CI packets */
    case V8_CI:
        {
            int i;
            
            /* send 4 CI packets (at least 3 must be sent) */
            for(i=0;i<4;i++) {
                sm_put_bits(&s->tx_fifo, V8_TEN_ONES, 10);
                sm_put_bits(&s->tx_fifo, V8_CI_SYNC, 10);
                v8_put_byte(s, V8_CALL_FUNC_DATA);
            }
            s->state = V8_CI_SEND;
        }
        break;

    case V8_CI_SEND:
        {
            if (sm_size(&s->tx_fifo) == 0) {
                s->state = V8_CI_OFF;
                sm_set_timer(&s->v8_ci_timer, 500); /* 0.5 s off */
            }
        }
        break;
 
    case V8_CI_OFF:
        {
            /* check if an ANSam tone is detected */
            if (s->v8_rx.v8_ANSam_detected) {
		sm_set_timer(&s->v8_ci_timer, V8_TE);
                s->state = V8_GOT_ANSAM;
            } else if (sm_check_timer(&s->v8_ci_timer)) {
                if (++s->v8_ci_count == V8_MAX_CI_SEQ) {
                    ret = V8_MOD_HANGUP;
                }
                else {
                    s->state = V8_CI;
                }
            }
        }
        break;

    case V8_GOT_ANSAM:
    	{
	    if (sm_check_timer(&s->v8_ci_timer)) {
                v8_decode_init(s);
		s->state = V8_CM_SEND;
	    }
	}
	break;

    case V8_CM_SEND:
    	{
            if (s->got_cm) {
                /* if JM detected, we send CJ & wait for 75 ms before exiting V8 */

                s->selected_mod_mask = s->modulation_mask & s->decoded_modulations;
                s->selected_modulation = select_modulation(s->selected_mod_mask);

                /* flush tx queue */
                sm_flush(&s->tx_fifo);
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                /* a few more bytes to fill the time */
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                v8_put_byte(s, 0);
                s->state = V8_CJ_SEND;
            } else if (sm_size(&s->tx_fifo) == 0) {
                /* send CM */
                cm_send(s, s->modulation_mask);
            }
	}
        break;

    case V8_CJ_SEND:
        /* wait until CJ is sent */
        if (sm_size(&s->tx_fifo) == 0) {
            sm_set_timer(&s->v8_start_timer, 75);
            s->state = V8_SIGC;
        }
        break;

    case V8_SIGC:
        if (sm_check_timer(&s->v8_start_timer)) {
            /* it's OK, let's start the wanted modulation */
            ret = s->selected_modulation;
        }
        break;


        /* V8 answer */

    case V8_WAIT:
        {
            if (sm_check_timer(&s->v8_connect_timer)) {
                /* send the ANSam tone */
                s->v8_tx.tone_level = -3; /* XXX: fix it */
                V8_mod_init(&s->v8_tx);
                
                /* prepare V21 to receive CI or CM */
                v8_decode_init(s);

                /* wait at most 30 seconds for the calling modem's CM */
                sm_set_timer(&s->v8_connect_timer, 30000);
                s->state = V8_CM_WAIT;
            }
        }
        break;

    case V8_CM_WAIT:
        {
            if (sm_check_timer(&s->v8_connect_timer)) {
                /* timeout */
                ret = V8_MOD_HANGUP;
            } else {
                if (s->got_cm) {
                    /* stop sending ANSam & send JM */
                    V21_mod_init(&s->v21_tx, 0, get_bit, s);
                    /* timeout for JM */
                    sm_set_timer(&s->v8_connect_timer, 15000);
                    /* a real CJ can only follow the modem seeing >=2 JM seqs
                       (V.8 8.2.3) -> clear any spurious CJ from the CM phase and
                       require >=400ms of JM before we accept one */
                    s->got_cj = 0;
                    s->data_zero_count = 0;
                    sm_set_timer(&s->v8_start_timer, 400);
                    s->state = V8_JM_SEND;
                    s->selected_mod_mask = s->modulation_mask & s->decoded_modulations;
                    s->selected_modulation = select_modulation(s->selected_mod_mask);
                }
            }
        }
        break;

    case V8_JM_SEND:
        {
            if (sm_check_timer(&s->v8_connect_timer)) {
                /* timeout */
                ret = V8_MOD_HANGUP;
            } else if (s->got_cj && sm_check_timer(&s->v8_start_timer)) {
                /* stop sending JM & wait 75 ms */
                fprintf(stderr, "[v8] real CJ accepted -> V.34\n"); fflush(stderr);
                sm_set_timer(&s->v8_connect_timer, 75); 
                s->state = V8_SIGA;
            } else if (s->got_cm && !s->got_cj && s->bits_since_octet > 150 &&
                       sm_check_timer(&s->v8_start_timer)) {
                /* V.8 8.2.3: CM has CEASED (>=0.5s with no parsed octets) while we send
                   JM -- the modem CJ-ed (we missed the 3 zero octets to bit slips) and
                   moved on. Complete V.8. Captures show the modem ALWAYS CJs at ~3s;
                   every "flaky V.8" call was this detector missing it. */
                fprintf(stderr, "[v8] CM ceased -> CJ-equivalent accepted -> V.34\n"); fflush(stderr);
                s->got_cj = 1;
                sm_set_timer(&s->v8_connect_timer, 75);
                s->state = V8_SIGA;
            } else if (sm_size(&s->tx_fifo) == 0) {
                /* Send JM */
                cm_send(s, s->selected_mod_mask);
            }
        }
        break;

    case V8_SIGA:
        if (sm_check_timer(&s->v8_connect_timer)) {
            ret = s->selected_modulation;
        }
        break;
    }
    return ret;
}
