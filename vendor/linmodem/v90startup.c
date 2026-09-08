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
                fprintf(stderr, "[v90p2] CRC-valid INFO0a at %.3fs: ack=%d 3429=%d\n",
                        s->samples / 8000.0, r->bits[28], r->bits[14]);
            }
        }
        r->previous_re = r->re; r->previous_im = r->im;
        r->re = r->im = 0; r->have_previous = 1;
    }
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
        int symbol = (s->samples * 3) / 40;
        if (symbol < 63 && symbol != s->tx_symbol) {
            s->tx_symbol = symbol;
            if (symbol && s->info0d[symbol-1]) s->tx_sign = -s->tx_sign;
        }
        /* 0 dBm0 sine is about 8031 RMS on the 16-bit PCM scale. */
        out[i] = (int16_t)lrint(2853.0 * s->tx_sign *
                              cos(2 * M_PI * 1200.0 * s->samples / 8000.0));
    }
}
