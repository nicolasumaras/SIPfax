#!/usr/bin/env python3
# Apply the modem's decoded INFO1c directives to linmodem's Phase-3 TX:
#  1. Pre-emphasis filter #2 (14 dB template tilt) as 1st-order FIR y=g*(x[n]-a*x[n-1]),
#     a=0.58 g=0.811 fitted offline to the V.34 Fig-1 template + slmodem's TRN level
#     (also folds in the requested 2 dB power reduction -> target rms ~2160).
#  2. Skip the second S/S-bar pair (we announce MD=0; spec 11.3.1.1.1: with MD=0 the
#     caller expects S, S-bar, then PP directly).
def rd(p): return open(p, encoding="latin-1").read()
def wr(p, s): open(p, "w", encoding="latin-1").write(s)

c = rd("/root/linmodem/v34.c")

# --- 1. pre-emphasis + power in the post-handoff TX path (extend the level-match block) ---
old = """    {   /* level-match: V34_mod output is ~rms 11500, scale to ~2300 like V.8/Phase2 */
        int _i; for (_i = 0; _i < nb_samples; _i++) output[_i] = (s16)(output[_i] / 5);
    }"""
new = """    {   /* level-match (rms 11500 -> ~2300), then PRE-EMPHASIS FILTER #2 + 2 dB power cut
           as directed by the modem's decoded INFO1c (S=3429: highcarrier=1 preemph=2
           power=2). Filter: y = 0.811*(x[n] - 0.58*x[n-1]) -- fitted offline to the
           V.34 Figure-1 alpha=14dB template and slmodem's measured TRN (rms ~2160). */
        int _i;
        for (_i = 0; _i < nb_samples; _i++) {
            double xx = (double)output[_i] / 5.0;
            double yy = 0.811 * (xx - 0.58 * s->p3x1);
            s->p3x1 = xx;
            output[_i] = (s16)yy;
        }
    }"""
if "PRE-EMPHASIS FILTER" in c:
    print("v34.c: preemph already applied")
else:
    assert old in c, "level-match block not found"
    c = c.replace(old, new, 1)
    print("v34.c: pre-emphasis + power applied")

# --- 2. S, S-bar, then PP directly (MD=0 path) ---
old2 = """        case V34_STARTUP3_SINV1:
            V34_send_Sinv(s);
            s->state = V34_STARTUP3_S2;
            break;"""
new2 = """        case V34_STARTUP3_SINV1:
            V34_send_Sinv(s);
            /* we announce MD=0 in INFO1a: per 11.3.1.1.1 the caller expects S, S-bar,
               then PP directly (no second S/S-bar pair) */
            s->state = V34_STARTUP3_PP;
            break;"""
if "no second S/S-bar" in c:
    print("v34.c: S2/SINV2 already skipped")
else:
    assert old2 in c, "SINV1 state not found"
    c = c.replace(old2, new2, 1)
    print("v34.c: SINV1 -> PP (skip S2/SINV2)")

if "s->p3x1 = 0;" not in c:
    c = c.replace("s->p3n = 0;", "s->p3n = 0; s->p3x1 = 0;", 1)
    print("v34.c: p3x1 init at handoff")
wr("/root/linmodem/v34.c", c)

# --- p3x1 state field ---
h = rd("/root/linmodem/v34priv.h")
if "double p3x1;" not in h:
    assert "long p3n;" in h
    h = h.replace("long p3n;", "long p3n;\n    double p3x1;           /* pre-emphasis filter memory */", 1)
    wr("/root/linmodem/v34priv.h", h)
    print("v34priv.h: p3x1 added")
