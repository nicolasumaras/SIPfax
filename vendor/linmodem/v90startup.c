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
            memmove(r->bits, r->bits+1, 48);
            r->bits[48] = bit;
            if (r->count < 49) ++r->count;
            if (!s->info0_received && r->count == 49 && valid_info0a(r->bits)) {
                s->info0_received = 1;
                s->info0_at = s->samples;
                fprintf(stderr, "[v90p2] CRC-valid INFO0a at %.3fs: ack=%d 3429=%d\n",
                        s->samples / 8000.0, r->bits[28], r->bits[14]);
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
                fprintf(stderr,"[v90p2] second A reversal at %.6fs; RTD %.3fms; receive probe\n",
                        when/8000.0,(when-s->first_tx_reversal-320)/8.0);
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
        int symbol = (s->samples * 3) / 40;
        if (symbol < 63 && symbol != s->tx_symbol) {
            s->tx_symbol = symbol;
            if (symbol && s->info0d[symbol-1]) s->tx_sign = -s->tx_sign;
        }
        if (s->ranging_state == 1 && s->samples >= s->reverse_due) {
            s->tx_sign = -s->tx_sign;
            s->first_tx_reversal = s->samples;
            s->ranging_state = 2;
            fprintf(stderr,"[v90p2] B reversal transmitted at %.6fs\n",s->samples/8000.0);
        }
        /* 0 dBm0 sine is about 8031 RMS on the 16-bit PCM scale. */
        out[i] = (int16_t)lrint(2853.0 * s->tx_sign *
                              cos(2 * M_PI * 1200.0 * s->samples / 8000.0));
        if (s->first_tx_reversal >= 0 && s->samples >= s->first_tx_reversal+80)
            out[i] = 0;
    }
}
