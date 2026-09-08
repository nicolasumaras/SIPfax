/*
 * V.34 Phase-2 L1/L2 line probing + channel estimation.
 *
 * L1/L2 = 21 cosine tones, 150..3750 Hz spaced 150 Hz, OMITTING 900/1200/1800/2400,
 * initial phases per Table 17.  L1 = 160 ms (24 periods of 150 Hz) at 6 dB above
 * nominal; L2 same at nominal for <=550 ms.  The receiver measures each tone's
 * magnitude to estimate the channel response, then selects symbol rate / carrier /
 * pre-emphasis for Phases 3/4.
 *
 * Standalone test: gcc -O2 -lm -o v34_probe v34_probe.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SR 8000.0
#define NTONES 21

/* Table 17: frequency (Hz) and initial phase (deg). */
static const double probe_f[NTONES] = {
    150,300,450,600,750,1050,1350,1500,1650,1950,2100,
    2250,2550,2700,2850,3000,3150,3300,3450,3600,3750
};
static const double probe_phase_deg[NTONES] = {
    0,180,0,0,0,0,0,0,180,0,0,
    180,0,180,0,180,180,180,180,0,0
};

/* symbol-rate index labels */
static const char *SR_NAME[6] = {"2400","2743","2800","3000","3200","3429"};

/* Generate L1/L2 probing signal (sum of the 21 tones) into out[n]. */
static void probe_gen(short *out, int n, double amp)
{
    int k, t;
    for (k = 0; k < n; k++) {
        double v = 0;
        for (t = 0; t < NTONES; t++)
            v += amp * cos(2*M_PI*probe_f[t]*k/SR + probe_phase_deg[t]*M_PI/180.0);
        out[k] = (short)(v / NTONES * 4.0);   /* scale to avoid clipping */
    }
}

/* Goertzel magnitude at frequency f over x[n]. */
static double goertzel(const short *x, int n, double f)
{
    double w = 2*M_PI*f/SR, c = 2*cos(w), s1 = 0, s2 = 0, s0;
    int k;
    for (k = 0; k < n; k++) { s0 = x[k] + c*s1 - s2; s2 = s1; s1 = s0; }
    return sqrt(s1*s1 + s2*s2 - c*s1*s2) / n;
}

/* Estimate the channel from a received probe and choose a symbol rate.
   Fills mags[NTONES]. Returns symbol-rate index 0..5. */
static int channel_estimate(const short *x, int n, double mags[NTONES])
{
    int t;
    double maxm = 0;
    for (t = 0; t < NTONES; t++) { mags[t] = goertzel(x, n, probe_f[t]); if (mags[t] > maxm) maxm = mags[t]; }
    double thr = maxm * 0.25;   /* usable if within ~12 dB of the strongest tone */
    /* highest usable probing frequency */
    double hi = 0;
    for (t = 0; t < NTONES; t++) if (mags[t] > thr && probe_f[t] > hi) hi = probe_f[t];
    /* map usable upper edge -> symbol rate (needs ~symrate/2 + carrier of band) */
    if (hi >= 3600) return 5;   /* 3429 */
    if (hi >= 3300) return 4;   /* 3200 */
    if (hi >= 3000) return 3;   /* 3000 */
    if (hi >= 2850) return 2;   /* 2800 */
    if (hi >= 2700) return 1;   /* 2743 */
    return 0;                   /* 2400 */
}

/* Apply a simple low-pass channel: zero/attenuate tones above cutoff Hz. */
static void channel_lowpass(short *x, int n, double cutoff)
{
    /* crude: re-synth only tones <= cutoff (models a band-limited line) */
    short *tmp = calloc(n, sizeof(short));
    int k, t;
    for (k = 0; k < n; k++) {
        double v = 0;
        for (t = 0; t < NTONES; t++) {
            if (probe_f[t] <= cutoff)
                v += cos(2*M_PI*probe_f[t]*k/SR + probe_phase_deg[t]*M_PI/180.0);
            else
                v += 0.05 * cos(2*M_PI*probe_f[t]*k/SR + probe_phase_deg[t]*M_PI/180.0); /* -26 dB */
        }
        tmp[k] = (short)(v / NTONES * 4.0 * 6000.0);
    }
    memcpy(x, tmp, n*sizeof(short)); free(tmp);
}

static void add_noise(short *x, int n, double snr_db, double sigamp)
{
    unsigned int seed = 999;
    double na = sigamp / pow(10.0, snr_db/20.0);
    int k, q;
    for (k = 0; k < n; k++) {
        double g = 0; for (q=0;q<6;q++){ seed=seed*1103515245+12345; g += ((seed>>16)&0x7fff)/32767.0 - 0.5; }
        x[k] = (short)(x[k] + na*g);
    }
}

int main(void)
{
    int n = (int)(0.160*SR);   /* 160 ms L1 */
    short *buf = malloc(n*sizeof(short));
    double mags[NTONES];
    int err = 0, sr;

    printf("=== L1/L2 probing + channel estimation ===\n");

    /* 1) clean wideband channel -> expect 3429 (index 5) */
    probe_gen(buf, n, 6000.0);
    sr = channel_estimate(buf, n, mags);
    printf("clean line       -> symbol rate %s (idx %d) %s\n", SR_NAME[sr], sr, sr==5?"OK":"WRONG");
    if (sr != 5) err++;

    /* 2) band-limited to 3000 Hz -> expect ~3000 (index 3) */
    channel_lowpass(buf, n, 3000.0);
    sr = channel_estimate(buf, n, mags);
    printf("3 kHz band-limit -> symbol rate %s (idx %d) %s\n", SR_NAME[sr], sr, sr==3?"OK":"WRONG");
    if (sr != 3) err++;

    /* 3) band-limited to 2700 Hz -> expect 2743 (index 1) */
    channel_lowpass(buf, n, 2700.0);
    sr = channel_estimate(buf, n, mags);
    printf("2.7kHz band-limit-> symbol rate %s (idx %d) %s\n", SR_NAME[sr], sr, sr==1?"OK":"WRONG");
    if (sr != 1) err++;

    /* 4) clean + 25 dB noise -> still wideband 3429 */
    probe_gen(buf, n, 6000.0);
    add_noise(buf, n, 25.0, 6000.0);
    sr = channel_estimate(buf, n, mags);
    printf("clean + 25dB AWGN-> symbol rate %s (idx %d) %s\n", SR_NAME[sr], sr, sr>=4?"OK":"WRONG");
    if (sr < 4) err++;

    /* show the channel response of the clean line */
    probe_gen(buf, n, 6000.0);
    channel_estimate(buf, n, mags);
    printf("clean response (per tone mag): ");
    for (int t = 0; t < NTONES; t += 4) printf("%.0fHz=%.0f ", probe_f[t], mags[t]);
    printf("\n\n%s (%d failures)\n", err ? "FAIL" : "ALL PASS", err);
    free(buf);
    return err ? 1 : 0;
}
