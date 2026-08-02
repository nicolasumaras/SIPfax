/*
 * V.34 Phase-2 Tone A / Tone B generation + phase-reversal detection (ranging).
 *
 * Tone A = 2400 Hz (answer modem), 1 dB below nominal, with a 1800 Hz guard tone
 * at nominal.  Tone B = 1200 Hz (call modem).  A<->Abar / B<->Bbar transitions are
 * 180-degree phase reversals.  Round-trip delay is measured from the timing between
 * a transmitted Tone-A reversal and the detected Tone-B reversal (spec 11.2.1.2).
 *
 * Standalone test: gcc -O2 -lm -o v34_tones v34_tones.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define SR 8000.0

/* Generate a tone of length n into out. carrier Hz, amplitude amp. A 180-deg
   phase reversal is inserted at sample `rev` (or -1 for none). guard!=0 adds a
   1800 Hz guard tone (for Tone A). Returns end carrier phase for continuity. */
static double tone_gen(short *out, int n, double carrier, double amp,
                       double start_phase, int rev, int guard)
{
    int k;
    double extra = 0.0;
    for (k = 0; k < n; k++) {
        if (k == rev) extra = M_PI;                 /* phase reversal */
        double v = amp * cos(2*M_PI*carrier*k/SR + start_phase + extra);
        if (guard) v += amp * cos(2*M_PI*1800.0*k/SR);   /* guard tone (no reversal) */
        out[k] = (short)v;
    }
    return start_phase + extra + 2*M_PI*carrier*n/SR;
}

/* Detect a 180-degree phase reversal in a tone at `carrier`. Returns the sample
   index of the reversal (best estimate), or -1 if none. win = analysis window. */
static int reversal_detect(const short *x, int n, double carrier)
{
    int win = 80;            /* 10 ms I/Q integration */
    int step = 8;            /* 1 ms resolution */
    int nblk = (n - win) / step;
    if (nblk < 3) return -1;
    static double ph[8192];
    int b, i;
    for (b = 0; b < nblk && b < 8192; b++) {
        int s0 = b * step;
        double I = 0, Q = 0;
        for (i = 0; i < win; i++) {
            double a = 2*M_PI*carrier*(s0+i)/SR;
            I += x[s0+i]*cos(a); Q -= x[s0+i]*sin(a);
        }
        ph[b] = atan2(Q, I);
    }
    /* a reversal shows as a ~180-deg jump in the windowed phase between blocks
       separated by ~win (so the window no longer straddles the flip). */
    int gap = win / step;
    int best = -1; double bestmag = 2.0;   /* radians from pi we tolerate */
    for (b = gap; b < nblk; b++) {
        double d = ph[b] - ph[b-gap];
        while (d > M_PI) d -= 2*M_PI;
        while (d < -M_PI) d += 2*M_PI;
        double dist = fabs(fabs(d) - M_PI);
        if (dist < bestmag) { bestmag = dist; best = b; }
    }
    if (best < 0 || bestmag > 0.6) return -1;       /* ~35 deg tolerance */
    /* the flip lies ~half a window before block `best`'s center */
    return best * step + win/2 - win/2;
}

int main(void)
{
    short buf[40000];
    int n, rev, det, err = 0;

    printf("=== Tone A/B reversal detection self-test ===\n");
    /* Tone A (2400+guard), reversal at various points */
    int points[] = { 800, 1600, 4000, 8000, 12000 };
    for (int t = 0; t < 5; t++) {
        rev = points[t];
        n = rev + 4000;
        tone_gen(buf, n, 2400.0, 6000.0, 0.3, rev, 1);   /* Tone A w/ guard */
        det = reversal_detect(buf, n, 2400.0);
        int e = (det < 0) ? 9999 : abs(det - rev);
        printf("  Tone A rev@%5d -> detected@%5d (err %d samp = %.1f ms) %s\n",
               rev, det, e, e/8.0, e < 80 ? "OK" : "MISS");
        if (e >= 80) err++;
    }
    /* Tone B (1200, no guard) */
    for (int t = 0; t < 5; t++) {
        rev = points[t];
        n = rev + 4000;
        tone_gen(buf, n, 1200.0, 6000.0, 1.1, rev, 0);
        det = reversal_detect(buf, n, 1200.0);
        int e = (det < 0) ? 9999 : abs(det - rev);
        printf("  Tone B rev@%5d -> detected@%5d (err %d samp = %.1f ms) %s\n",
               rev, det, e, e/8.0, e < 80 ? "OK" : "MISS");
        if (e >= 80) err++;
    }
    /* No-reversal control: should detect nothing */
    n = 8000; tone_gen(buf, n, 2400.0, 6000.0, 0.0, -1, 1);
    det = reversal_detect(buf, n, 2400.0);
    printf("  Tone A no-reversal -> detected@%d %s\n", det, det < 0 ? "OK(none)" : "FALSE+");
    if (det >= 0) err++;

    /* === ranging timing simulation ===
       answer sends Tone A reversal at Ta; after round-trip delay RTD it appears
       in the received Tone B reversal at Ta+RTD. Recover RTD. */
    printf("=== ranging RTD recovery ===\n");
    int RTD = 240;   /* 30 ms round trip */
    int Ta = 2000;
    n = 8000;
    tone_gen(buf, n, 1200.0, 6000.0, 0.0, Ta + RTD, 0);   /* received Tone B w/ reversal delayed by RTD */
    det = reversal_detect(buf, n, 1200.0);
    int rtd_meas = det - Ta;
    printf("  sent A-rev@%d, recovered B-rev@%d -> RTD=%d samp (%.1f ms, true %.1f ms) %s\n",
           Ta, det, rtd_meas, rtd_meas/8.0, RTD/8.0, abs(rtd_meas-RTD) < 80 ? "OK" : "MISS");
    if (abs(rtd_meas - RTD) >= 80) err++;

    printf("\n%s (%d failures)\n", err ? "FAIL" : "ALL PASS", err);
    return err ? 1 : 0;
}
