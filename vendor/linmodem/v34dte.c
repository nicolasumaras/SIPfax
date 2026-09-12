/* V.34 DTE framing and answerer LAPM bridge. GPL-2.0. */
#include "lm.h"

static int get_octets(void *opaque, uint8_t *data, int maximum)
{
    struct sm_state *s = opaque;
    int n = 0, value;
    while (n < maximum && (value = sm_get_bit(&s->tx_fifo)) >= 0)
        data[n++] = value;
    return n;
}

static int put_octets(void *opaque, const uint8_t *data, int length)
{
    struct sm_state *s = opaque;
    int room = s->rx_fifo.max_size - sm_size(&s->rx_fifo);
    if (length > room) length = room;
    for (int n = 0; n < length; n++) sm_put_bit(&s->rx_fifo, data[n]);
    return length;
}

void v34_dte_init(struct sm_state *s, int lapm)
{
    serial_init(s, 8, 'N');
    s->v34_lapm_requested = lapm;
    s->v34_lapm_samples = 0;
    s->v34_lapm.enabled = s->v34_lapm.initialized = 0;
    /* Initialize when DATA first requests bits, after the MP rate is known. */
}

static void prepare(struct sm_state *s)
{
    int rate = s->u.v34_state.v34_tx.R;
    if (!s->v34_lapm.initialized)
        v90_lapm_link_init(&s->v34_lapm, rate, s, get_octets, put_octets);
    else
        s->v34_lapm.protocol.tx_bit_rate = rate;
}

int v34_dte_get_bit(void *opaque)
{
    struct sm_state *s = opaque;
    if (!s->v34_lapm_requested) return serial_8n1_get_bit(s);
    prepare(s);
    return v90_lapm_link_tx_bit(&s->v34_lapm);
}

void v34_dte_put_bit(void *opaque, int bit)
{
    struct sm_state *s = opaque;
    if (!s->v34_lapm_requested) { serial_8n1_put_bit(s, bit); return; }
    prepare(s);
    /* V.34 currently supplies one demodulated stream, in 8-kHz block time. */
    v90_lapm_link_candidate_bit(&s->v34_lapm, 0, bit, s->v34_lapm_samples);
}

void v34_dte_retrain(struct sm_state *s)
{
    serial_init(s, 8, 'N');
    if (s->v34_lapm_requested && s->v34_lapm.initialized)
        v90_lapm_link_candidate_bit(&s->v34_lapm, 0, -1, s->v34_lapm_samples);
}
