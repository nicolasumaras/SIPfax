/* Loopback test: does the V.8 -> V.22bis answer-side handoff train, vs forced V.22bis?
 * Build against installed spandsp 0.0.6. Two endpoints (caller, answerer), clean
 * 4-wire exchange (each side rx = other side's tx only -> no echo), so this isolates
 * the handoff PROTOCOL from channel/echo effects. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <spandsp.h>

#define BLOCK 160

typedef enum { M_V8, M_V22BIS } modem_mode_t;

typedef struct {
    const char *name;
    int calling;
    v8_state_t *v8;
    v22bis_state_t *v22bis;
    modem_mode_t mode;
    int pending;
    int trained;
    long rx_data_bits;
    long tx_data_bits;
    int guard;
} side_t;

static int forced = 0;   /* 1 = skip V.8, start V.22bis immediately */

static void start_v22(side_t *s);

static void v8_result(void *u, v8_parms_t *r) {
    side_t *s = (side_t *)u;
    fprintf(stderr, "  [%s] v8 result status=%d mods=0x%x\n", s->name, r->status, r->modulations);
    if (r->status == V8_STATUS_V8_CALL)
        s->pending = 1;   /* defer handoff to main loop (after v8_rx returns) */
}

static int v22_getbit(void *u) {
    side_t *s = (side_t *)u;
    if (!s->trained) return 1;
    s->tx_data_bits++;
    return (int)(s->tx_data_bits & 1);
}
static void v22_putbit(void *u, int bit) {
    side_t *s = (side_t *)u;
    if (bit >= 0) { if (s->trained) s->rx_data_bits++; }
}
static void v22_status(void *u, int status) {
    side_t *s = (side_t *)u;
    const char *n = "?";
    switch (status) {
        case SIG_STATUS_CARRIER_UP: n="carrier-up"; break;
        case SIG_STATUS_CARRIER_DOWN: n="carrier-down"; break;
        case SIG_STATUS_TRAINING_IN_PROGRESS: n="training"; break;
        case SIG_STATUS_TRAINING_SUCCEEDED: n="TRAINED"; s->trained=1; break;
        case SIG_STATUS_TRAINING_FAILED: n="train-FAILED"; break;
    }
    fprintf(stderr, "  [%s] v22bis status %d (%s) rate=%d\n", s->name, status, n,
            (status==SIG_STATUS_TRAINING_SUCCEEDED)? v22bis_get_current_bit_rate(s->v22bis) : 0);
}

static void start_v22(side_t *s) {
    s->v22bis = v22bis_init(NULL, 2400, s->guard, s->calling, v22_getbit, s, v22_putbit, s);
    v22bis_set_modem_status_handler(s->v22bis, v22_status, s);
    s->mode = M_V22BIS;
    fprintf(stderr, "  [%s] -> V.22bis (%s, guard=%d)\n", s->name,
            s->calling?"calling":"answering", s->guard);
}

static void init_v8(side_t *s) {
    v8_parms_t p; memset(&p, 0, sizeof(p));
    p.modem_connect_tone = s->calling ? MODEM_CONNECT_TONES_NONE : MODEM_CONNECT_TONES_ANSAM_PR;
    p.send_ci = s->calling ? true : false;
    p.call_function = V8_CALL_V_SERIES;
    p.modulations = V8_MOD_V21 | V8_MOD_V22;
    p.protocol = V8_PROTOCOL_NONE;
    s->v8 = v8_init(NULL, s->calling, &p, v8_result, s);
    s->mode = M_V8;
}

static int generate(side_t *s, int16_t *amp) {
    int n = 0;
    if (s->mode == M_V8 && s->v8) n = v8_tx(s->v8, amp, BLOCK);
    else if (s->mode == M_V22BIS && s->v22bis) n = v22bis_tx(s->v22bis, amp, BLOCK);
    if (n < BLOCK) { memset(amp + n, 0, (BLOCK - n) * sizeof(int16_t)); n = BLOCK; }
    return n;
}
static void receive(side_t *s, const int16_t *amp) {
    if (s->mode == M_V8 && s->v8) v8_rx(s->v8, amp, BLOCK);
    else if (s->mode == M_V22BIS && s->v22bis) v22bis_rx(s->v22bis, amp, BLOCK);
}

int main(int argc, char **argv) {
    int guard = V22BIS_GUARD_TONE_NONE;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "forced")) forced = 1;
        else if (!strcmp(argv[i], "v8")) forced = 0;
        else if (!strcmp(argv[i], "guard1800")) guard = V22BIS_GUARD_TONE_1800HZ;
    }
    side_t caller = { .name="CALLER", .calling=1, .guard=guard };
    side_t answer = { .name="ANSWER", .calling=0, .guard=guard };

    fprintf(stderr, "=== MODE: %s, guard=%s ===\n", forced?"FORCED v22bis":"V.8 negotiated",
            guard==V22BIS_GUARD_TONE_1800HZ?"1800Hz":"none");
    if (forced) { start_v22(&caller); start_v22(&answer); }
    else { init_v8(&caller); init_v8(&answer); }

    /* near-end echo model: rx = remote tx + echo_gain * own tx delayed */
    double echo_gain = 0.0;
    int echo_delay = 0;
    const char *eg = getenv("ECHO_GAIN"); if (eg) echo_gain = atof(eg);
    const char *ed = getenv("ECHO_DELAY"); if (ed) echo_delay = atoi(ed);
    fprintf(stderr, "echo_gain=%.2f echo_delay=%d samples\n", echo_gain, echo_delay);
    #define DLINE 4096
    static int16_t cdl[DLINE], adl[DLINE]; int dpos = 0;

    int16_t ampC[BLOCK], ampA[BLOCK], rxC[BLOCK], rxA[BLOCK];
    int both_trained_at = -1;
    long N = 8000L * 25 / BLOCK;   /* up to 25 seconds */
    for (long blk = 0; blk < N; blk++) {
        generate(&caller, ampC);
        generate(&answer, ampA);
        for (int i = 0; i < BLOCK; i++) {
            int de = (dpos + i - echo_delay + DLINE) % DLINE;
            double ce = echo_gain * cdl[de];   /* caller's own delayed tx */
            double ae = echo_gain * adl[de];   /* answer's own delayed tx */
            int rc = (int)(ampA[i] + ce);      /* caller rx = answer tx + own echo */
            int ra = (int)(ampC[i] + ae);      /* answer rx = caller tx + own echo */
            rxC[i] = rc>32767?32767:(rc<-32768?-32768:rc);
            rxA[i] = ra>32767?32767:(ra<-32768?-32768:ra);
        }
        for (int i = 0; i < BLOCK; i++) { cdl[(dpos+i)%DLINE]=ampC[i]; adl[(dpos+i)%DLINE]=ampA[i]; }
        dpos = (dpos + BLOCK) % DLINE;
        receive(&answer, rxA);     /* answerer hears caller (+own echo) */
        receive(&caller, rxC);     /* caller hears answerer (+own echo) */
        if (caller.pending) { v8_free(caller.v8); caller.v8=NULL; start_v22(&caller); caller.pending=0; }
        if (answer.pending) { v8_free(answer.v8); answer.v8=NULL; start_v22(&answer); answer.pending=0; }
        if (both_trained_at < 0 && caller.trained && answer.trained) {
            both_trained_at = (int)(blk * BLOCK / 8);   /* ms */
            fprintf(stderr, ">>> BOTH TRAINED at ~%d ms\n", both_trained_at);
        }
        if (both_trained_at >= 0 && blk * BLOCK / 8 > both_trained_at + 2000) break; /* 2s of data */
    }
    fprintf(stderr, "RESULT: caller.trained=%d answer.trained=%d  data bits rx: caller=%ld answer=%ld\n",
            caller.trained, answer.trained, caller.rx_data_bits, answer.rx_data_bits);
    printf("%s\n", (caller.trained && answer.trained && caller.rx_data_bits>0 && answer.rx_data_bits>0)
                   ? "PASS (handoff trained, data flowed both ways)"
                   : "FAIL (did not complete)");
    return 0;
}
