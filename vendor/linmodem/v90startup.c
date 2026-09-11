/* V.90 digital-side startup, ITU-T V.90 (09/98), clauses 8.2 and 9.2.
 * Capability exchange first; downstream data must not precede training.
 * GPL-2.0, consistent with the surrounding linmodem implementation.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "v90startup.h"

static unsigned crc_bits(const unsigned char *b, int n)
{
    unsigned crc = 0xffff;
    for (int i = 0; i < n; ++i) {
        unsigned feedback = (crc ^ b[i]) & 1;
        crc >>= 1;
        if (feedback) crc ^= 0x8408;
    }
    return crc;
}
static void field(unsigned char *b, int offset, int width, unsigned value)
{
    for (int i = 0; i < width; ++i) b[offset+i] = (value >> i) & 1;
}
void v90_info0d(unsigned char b[62], int alaw)
{
    static const unsigned char sync[8] = {0,1,1,1,0,0,1,0};
    memset(b, 0, 62);
    memset(b, 1, 4);
    memcpy(b+4, sync, 8);
    /* Support the implemented V.34 rates/carriers; no CME or power reduction. */
    for (int i = 12; i <= 19; ++i) b[i] = 1;
    b[25] = 1;
    field(b, 29, 4, 6);  /* nominal -12 dBm0 */
    field(b, 33, 5, 11); /* maximum -6 dBm0 */
    b[39] = !!alaw;
    b[40] = 1;
    field(b, 42, 16, crc_bits(b+12, 30));
    memset(b+58, 1, 4);
}
static void info1d_rate(unsigned char b[109],unsigned rate)
{
    static const unsigned char sync[8]={0,1,1,1,0,0,1,0};
    memset(b,0,109); memset(b,1,4); memcpy(b+4,sync,8);
    /* Flat pre-emphasis and high carrier at the implemented 3200 symbols/s.
       The projected maximum agrees with the selected per-call receiver. */
    b[61]=1; field(b,66,4,rate/2400);
    field(b,79,10,512); /* frequency estimate unavailable, as specified */
    field(b,89,16,crc_bits(b+12,77)); memset(b+105,1,4);
}
void v90_info1d(unsigned char b[109])
{
    info1d_rate(b,v90_upstream_configured_rate());
}
static void select_upstream_rate(V90Startup *s)
{
    /* V.90 Table 9 note 1: rates above 12 require large-constellation support. */
    unsigned limit=s->peer_large_constellations?31200:28800;
    s->upstream_data_rate=s->upstream_max_rate<limit?s->upstream_max_rate:limit;
    info1d_rate(s->info1d,s->upstream_data_rate);
}
void v90_startup_init(V90Startup *s, int alaw)
{
    memset(s, 0, sizeof(*s));
    s->alaw = !!alaw;
    s->reverse_due = -1;
    s->first_tx_reversal = -1;
    s->second_rx_reversal = -1;
    s->tx_symbol = -1;
    s->tx_sign = 1;
    v90_info0d(s->info0d, alaw);
    s->upstream_max_rate=v90_upstream_configured_rate();
    select_upstream_rate(s);
    /* Parallel symbol phases avoid assuming RTP and INFO boundaries coincide. */
    for (int j = 0; j < 14; ++j) s->rx[j].clock = j * 570;
    fprintf(stderr, "[v90p2] transmitting INFO0d (%s), then Tone B\n",
            alaw ? "A-law" : "mu-law");
}
static int valid_info0a(const unsigned char *b)
{
    static const unsigned char prefix[12] = {1,1,1,1,0,1,1,1,0,0,1,0};
    if (memcmp(b, prefix, 12)) return 0;
    unsigned crc = crc_bits(b+12, 17), received = 0;
    for (int i = 0; i < 16; ++i) received |= b[29+i] << i;
    /* Trailing fill is not protected by CRC and may overlap the tone transition. */
    return crc == received;
}
static void receive(V90Startup *s, int16_t input)
{
    double phase = 2 * M_PI * 2400.0 * s->samples / 8000.0;
    double re = input * cos(phase), im = -input * sin(phase);
    for (int j = 0; j < 14; ++j) {
        V90InfoRx *r = &s->rx[j];
        r->re += re; r->im += im; r->clock += 600;
        if (r->clock < 8000) continue;
        r->clock -= 8000;
        if (r->have_previous) {
            int bit = r->re * r->previous_re + r->im * r->previous_im < 0;
            memmove(r->bits, r->bits+1, 69);
            r->bits[69] = bit;
            if (r->count < 70) ++r->count;
            if (!s->info0_received && r->count >= 49 && valid_info0a(r->bits+21)) {
                s->info0_received = 1;
                s->info0_at = s->samples;
                s->peer_large_constellations=r->bits[21+25];
                select_upstream_rate(s);
                fprintf(stderr,"[v90p2] peer large constellation=%u; upstream selected=%u bit/s\n",
                        s->peer_large_constellations,s->upstream_data_rate);
                fprintf(stderr, "[v90p2] CRC-valid INFO0a at %.3fs: ack=%d 3429=%d\n",
                        s->samples / 8000.0, r->bits[49], r->bits[35]);
            }
            if (!s->info1_received && s->ranging_state >= 8 && r->count == 70) {
                unsigned char *b=r->bits;
                static const unsigned char prefix[12]={1,1,1,1,0,1,1,1,0,0,1,0};
                unsigned received=0;
                for(int k=0;k<16;++k) received|=b[50+k]<<k;
                if (!memcmp(b,prefix,12) && received==crc_bits(b+12,38)) {
                    s->info1_received=1; s->info1_received_at=s->samples; s->upstream_rate=0; s->downstream_rate=0; s->uinfo=0;
                    for(int k=0;k<3;++k) { s->upstream_rate|=b[34+k]<<k; s->downstream_rate|=b[37+k]<<k; }
                    for(int k=0;k<7;++k) s->uinfo|=b[25+k]<<k;
                    s->ranging_state=9;
                    if(s->upstream_rate==4 && s->downstream_rate==6) {
                        v90_training_init(&s->training);s->training_active=1;
                    }
                    fprintf(stderr,"[v90p2] CRC-valid INFO1a: upstream=%d downstream=%d UINFO=%d; training pending\n",
                            s->upstream_rate,s->downstream_rate,s->uinfo);
                }
            }
        }
        r->previous_re = r->re; r->previous_im = r->im;
        r->re = r->im = 0; r->have_previous = 1;
    }
}
/* Estimate the reversal boundary from the raw samples, rather than using the
   end of the detection window as its timestamp. This preserves the 40ms reply
   delay even when the boundary falls inside an RTP packet. */
static long reversal_boundary(V90Startup *s)
{
    int n = s->tone_history_count < 80 ? s->tone_history_count : 80;
    double score = 0, best = 0;
    int split = 0;
    for (int i = 0; i < n; ++i) {
        long t = s->samples - n + 1 + i;
        double phase = 2*M_PI*2400.0*t/8000.0;
        double predicted = s->ref_re*cos(phase)-s->ref_im*sin(phase);
        int index = (s->tone_position + 80 - n + i) % 80;
        /* Maximum prefix correlation identifies positive -> negative change. */
        score += s->tone_history[index] * predicted;
        if (score > best) { best = score; split = i+1; }
    }
    return s->samples - n + 1 + split;
}
static void begin_retrain(V90Startup *s, const char *reason)
{
    long now=s->samples;unsigned retrains=s->retrains+1;int law=s->alaw;
    unsigned selected=s->upstream_data_rate,maximum=s->upstream_max_rate,large=s->peer_large_constellations;
    v90_startup_init(s,law);
    s->upstream_data_rate=selected;s->upstream_max_rate=maximum;s->peer_large_constellations=large;
    info1d_rate(s->info1d,selected);
    s->samples=now;s->retrains=retrains;s->retrain_mute_until=now+560;
    s->info0_received=1;s->info0_at=now-1000;s->tx_symbol=63;
    fprintf(stderr,"[v90p2] %s retrain %u at %.6fs; silence70ms then Tone B\n",
            reason,retrains,now/8000.0);
}
int v90_startup_data_retrain(V90Startup *s)
{
    if(!s->phase4_active || s->phase4.stage!=4 || !s->phase4.rx_e_logged)return 0;
    begin_retrain(s,"initiate after unanswered PPP echoes;");
    return 1;
}
static void receive_tone(V90Startup *s, int16_t input)
{
    s->tone_history[s->tone_position] = input;
    s->tone_position = (s->tone_position+1)%80;
    ++s->tone_history_count;
    if (!s->info0_received || s->samples < s->info0_at+40) return;
    double phase=2*M_PI*2400.0*s->samples/8000.0;
    s->tone_re += input*cos(phase);
    s->tone_im -= input*sin(phase);
    s->tone_energy += (double)input*input;
    if (++s->tone_count < 40) return;
    double re=s->tone_re/40, im=s->tone_im/40;
    double power=re*re+im*im;
    double coherent=2*power/(s->tone_energy/40+1);
    double refpower=s->ref_re*s->ref_re+s->ref_im*s->ref_im;
    double dot=(re*s->ref_re+im*s->ref_im)/sqrt(power*refpower+1);
    /* V.90 9.5.1.2: sustained Tone A requests retraining, including data
       mode. Reset the data pump so no DTE bits are consumed or delivered
       during retraining; the outer PTY/FIFOs and PPP session remain intact. */
    if(s->ranging_state==9) {
        if(power>40000 && coherent>.35 && refpower>40000 && dot>.95)
            ++s->retrain_tone_windows;
        else s->retrain_tone_windows=0;
        if(s->retrain_tone_windows>=11) {
            begin_retrain(s,"respond to caller");
            return;
        }
    }
    if (power > 40000 && coherent > 0.30) {
        if (s->tone_locked >= 3 && dot < -0.8) {
            long when=reversal_boundary(s);
            if (s->ranging_state == 0 && s->samples >= 840) {
                s->reverse_due=when+320;
                s->ranging_state=1;
                fprintf(stderr,"[v90p2] Tone A reversal at %.6fs; B reply due %.6fs\n",
                        when/8000.0,s->reverse_due/8000.0);
            } else if (s->ranging_state == 2 && when > s->first_tx_reversal) {
                s->second_rx_reversal=when;
                s->ranging_state=3;
                s->round_trip=when-s->first_tx_reversal-320;
                fprintf(stderr,"[v90p2] second A reversal at %.6fs; RTD %.3fms; receive probe\n",
                        when/8000.0,(when-s->first_tx_reversal-320)/8.0);
            }
            if (s->ranging_state == 4) {
                s->reverse_due=when+320; s->ranging_state=5;
                fprintf(stderr,"[v90p2] probe turnaround A reversal at %.6fs\n",when/8000.0);
            }
            s->ref_re=re; s->ref_im=im; s->tone_locked=0;
        } else if (!s->tone_locked || dot > 0.9) {
            s->ref_re=re; s->ref_im=im;
            if (s->tone_locked < 4) ++s->tone_locked;
            s->tone_bad=0;
        } else if (++s->tone_bad > 2) s->tone_locked=0;
    } else if (++s->tone_bad > 2) s->tone_locked=0;
    s->tone_re=s->tone_im=s->tone_energy=0;
    s->tone_count=0;
}
static double probe_sample(long sample)
{
    static const int frequencies[21]={150,300,450,600,750,1050,1350,1500,1650,1950,2100,2250,2550,2700,2850,3000,3150,3300,3450,3600,3750};
    static const int inverted[21]={0,1,0,0,0,0,0,0,1,0,0,1,0,1,0,1,1,1,1,0,0};
    double value=0;
    for(int j=0;j<21;++j) value+=(inverted[j]?-1:1)*cos(2*M_PI*frequencies[j]*sample/8000.0);
    return value*(2853.0/sqrt(21.0))*(sample<1280?2:1);
}
void v90_startup_history(V90Startup *s, const int16_t *in, int n)
{
    /* Receive-only pre-roll: INFO0a may overlap the final V.8 silent interval. */
    s->samples = -n;
    for (int i = 0; i < n; ++i, ++s->samples) receive(s, in[i]);
}
void v90_startup_process(V90Startup *s, int16_t *out, const int16_t *in, int n)
{
    for (int i = 0; i < n; ++i, ++s->samples) {
        receive(s, in[i]);
        receive_tone(s, in[i]);
        /* 9.6.1: E must arrive within 5s + 2 RTDs of the Rd/Rd-bar
           transition (384 samples after Rd starts, not the end of Rd-bar).
           Test before generating the next sample so the first timeout
           sample already clamps the data pump and starts the 70ms mute. */
        if(s->phase4_active && s->phase4.reneg_start && !s->phase4.rx_e_logged) {
            long rtd=s->round_trip>0?s->round_trip:0;
            unsigned elapsed=s->phase4.samples-s->phase4.reneg_start;
            if(elapsed>=384 && (long)(elapsed-384)>=40000+2*rtd)
                begin_retrain(s,"initiate after renegotiation E timeout;");
        }
        /* Local recovery policy, not the 9.6.1 E deadline: after E, a
           missing B1 must not leave the receive gate closed indefinitely.
           Count only samples actually fed to the post-E receiver; this
           also excludes CPs echo-training silence. Allow 5s + 2 RTDs,
           well beyond the 40ms B1 at our supported 4800-bit/s rate. */
        if(s->phase4_active && s->phase4.renegotiations &&
           s->phase4.rx_e_logged && !s->phase4.cp.silence &&
           !s->phase4.upstream.b1_seen) {
            long rtd=s->round_trip>0?s->round_trip:0;
            if((long)s->phase4.upstream.samples>=40000+2*rtd)
                begin_retrain(s,"initiate after renegotiation B1 timeout;");
        }
        /* 9.4.1 bounds initial final training from receipt of INFO1a.
           E alone is insufficient: require recognition of the complete B1.
           Renegotiation has its separate, shorter deadline above. */
        if(s->info1_received && s->phase4_active &&
           !s->phase4.renegotiations && !s->phase4.upstream.b1_seen) {
            long rtd=s->round_trip>0?s->round_trip:0;
            if(s->samples-s->info1_received_at>=120000+5*rtd)
                begin_retrain(s,"initiate after initial B1 timeout;");
        }
        int symbol = (s->samples * 3) / 40;
        if (!s->retrains && symbol < 63 && symbol != s->tx_symbol) {
            s->tx_symbol = symbol;
            if (symbol && s->info0d[symbol-1]) s->tx_sign = -s->tx_sign;
        }
        if (s->ranging_state == 1 && s->samples >= s->reverse_due) {
            s->tx_sign = -s->tx_sign;
            s->first_tx_reversal = s->samples;
            s->ranging_state = 2;
            fprintf(stderr,"[v90p2] B reversal transmitted at %.6fs\n",s->samples/8000.0);
        }
        if (s->ranging_state == 3) {
            long elapsed=s->samples-s->second_rx_reversal;
            if (elapsed>=80 && elapsed<2960) { s->probe_energy+=(double)in[i]*in[i]; ++s->probe_samples; }
            if (elapsed>=2960) { /*10ms tail +160ms L1 +200ms L2 */
                s->ranging_state=4; s->tone_locked=0;
                fprintf(stderr,"[v90p2] received probe RMS %.1f; Tone B requests turnaround\n",
                        sqrt(s->probe_energy/(s->probe_samples?s->probe_samples:1)));
            }
        }
        if (s->ranging_state == 5 && s->samples >= s->reverse_due) {
            s->tx_sign=-s->tx_sign; s->probe_reply=s->samples; s->ranging_state=6;
            fprintf(stderr,"[v90p2] probe reply B reversal at %.6fs\n",s->samples/8000.0);
        }
        if (s->ranging_state == 6 && s->samples >= s->probe_reply+80) {
            s->probe_start=s->samples; s->ranging_state=7; s->tone_locked=0;
            fprintf(stderr,"[v90p2] transmit L1/L2 at %.6fs\n",s->samples/8000.0);
        }
        if (s->ranging_state == 7 && s->samples-s->probe_start>=2880 && s->tone_locked>=3) {
            s->info1_start=s->samples; s->ranging_state=8; s->tx_symbol=-1;
            fprintf(stderr,"[v90p2] Tone A received; transmit INFO1d at %.6fs\n",s->samples/8000.0);
        }
        /* 0 dBm0 sine is about 8031 RMS on the 16-bit PCM scale. */
        out[i] = (int16_t)lrint(2853.0 * s->tx_sign *
                              cos(2 * M_PI * 1200.0 * s->samples / 8000.0));
        if (s->first_tx_reversal >= 0 && s->samples >= s->first_tx_reversal+80 && s->ranging_state<4) out[i]=0;
        if (s->ranging_state == 7) out[i]=(int16_t)lrint(probe_sample(s->samples-s->probe_start));
        if (s->ranging_state == 8) {
            int sym=((s->samples-s->info1_start)*3)/40;
            if (sym<110) {
                if (sym!=s->tx_symbol) {
                    s->tx_symbol=sym;
                    if(sym && s->info1d[sym-1]) s->tx_sign=-s->tx_sign;
                }
                out[i]=(int16_t)lrint(2853*s->tx_sign*cos(2*M_PI*1200*s->samples/8000.0));
            } else out[i]=0;
        }
        if (s->ranging_state == 9) {
            out[i]=0;
            if(s->training_active && !s->training.found)
                v90_training_receive(&s->training,in+i,1);
            if(s->training.found && !s->training_tx_active && s->uinfo>=67 && s->uinfo<=111) {
                v90_train_tx_init(&s->training_tx,s->alaw,s->uinfo);
                s->training_tx.dil=s->training.dil;
                s->training_tx_active=1;
                /* Before Phase4 starts its receiver is available to monitor
                   CPt. A valid CPt proves the caller has left DIL, even if
                   the preceding short S/Sbar transition was impaired. */
                v90_training_init(&s->phase4.rx);s->phase4.rx.cp_mode=1;
                fprintf(stderr,"[v90p3] transmit Sd/Sbar, TRN1d then Jd at %.6fs UINFO=%d\n",s->samples/8000.0,s->uinfo);
            }
            if(s->phase4_active) {
                out[i]=v90_phase4_next(&s->phase4,in[i]);
                /* 9.4.1 permits a retrain at any point in Phase4. Rejected
                   constellation parameters cannot produce training/data;
                   start recovery instead of waiting indefinitely in silence. */
                if(s->phase4.stage==3) {
                    begin_retrain(s,"initiate after rejected Phase4 parameters;");
                    out[i]=0;
                }
            } else if(s->training_tx_active) {
                if(s->training_tx.stage==1 && !s->training_tx.stop_dil) {
                    V90Training *monitor=&s->phase4.rx;
                    int found=monitor->found;
                    v90_training_receive(monitor,in+i,1);
                    if(monitor->found!=found && !monitor->cp.type) {
                        s->training_tx.stop_dil=1;
                        fprintf(stderr,"[v90p3] CRC-valid CPt during DIL at %.6fs; finish segment and enter Phase4\n",s->samples/8000.0);
                    }
                }
                if(s->training_tx.sample>=2544) {
                    int event=v90_s_detect(&s->s_detector,in[i]);
                    if(event==1 && !s->training_tx.jd_end) {
                        v90_train_tx_end_jd(&s->training_tx);
                        fprintf(stderr,"[v90p3] S detected at %.6fs; finish Jd then Jd-prime and DIL\n",s->samples/8000.0);
                    }
                    if(event==2) {
                        ++s->s_transitions;
                        fprintf(stderr,"[v90p3] S/Sbar transition %u at %.6fs\n",s->s_transitions,s->samples/8000.0);
                        if(s->s_transitions==2)s->training_tx.stop_dil=1;
                    }
                }
                unsigned stage=s->training_tx.stage;
                out[i]=v90_train_tx_next(&s->training_tx);
                if(stage!=s->training_tx.stage)
                    fprintf(stderr,"[v90p3] DIL stage %u at %.6fs (2=Phase4 pending)\n",s->training_tx.stage,s->samples/8000.0);
                if(s->training_tx.stage==2) {
                    v90_phase4_init_rate(&s->phase4,s->alaw,s->uinfo,s->upstream_data_rate);s->phase4_active=1;
                }
            }
        }
        if(s->samples<s->retrain_mute_until)out[i]=0;
    }
}
