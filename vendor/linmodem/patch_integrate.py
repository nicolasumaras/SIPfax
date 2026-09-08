#!/usr/bin/env python3
"""Wire the v34_phase2 streaming module into linmodem's V.34 datapump."""
D="/root/linmodem/"
def rd(p): return open(D+p,encoding="latin-1").read()
def wr(p,s): open(D+p,"w",encoding="latin-1").write(s)

# 1) Makefile: add v34_phase2.o
m=rd("Makefile")
if "v34_phase2.o" not in m:
    m=m.replace("linpipe.o","linpipe.o v34_phase2.o",1)
    wr("Makefile",m); print("Makefile +v34_phase2.o")
else: print("Makefile already has v34_phase2.o")

# 2) v34priv.h: add phase2 members to V34State
h=rd("v34priv.h")
if "void *phase2" not in h:
    h=h.replace("    V34DSPState v34_tx;\n    V34DSPState v34_rx;",
                "    V34DSPState v34_tx;\n    V34DSPState v34_rx;\n    void *phase2;        /* V.34 Phase-2 answer SM (opaque) */\n    int phase2_active;",1)
    wr("v34priv.h",h); print("v34priv.h +phase2 members")
else: print("v34priv.h already patched")

# 3) v34.c: forward decls + V34_init + V34_process
c=rd("v34.c")
if "v34_phase2_new" not in c:
    decls=("/* V.34 Phase-2 answer state machine (v34_phase2.c) */\n"
           "extern void *v34_phase2_new(void);\n"
           "extern int v34_phase2_run(void *p, s16 *out, s16 *in, int n);\n"
           "extern int v34_phase2_symrate(void *p);\n"
           "extern void v34_phase2_free(void *p);\n\n"
           "void V34_init(struct V34State *s, int calling)\n")
    c=c.replace("void V34_init(struct V34State *s, int calling)\n",decls,1)
    # init phase2 (answer side only) before the closing brace of V34_init
    old_init_tail="    s->calling = calling;\n    V34_mod_init(&s->v34_tx, s);\n    s->calling = !calling;\n    V34_demod_init(&s->v34_rx, s);\n    s->calling = calling;\n}"
    new_init_tail=("    s->calling = calling;\n    V34_mod_init(&s->v34_tx, s);\n    s->calling = !calling;\n    V34_demod_init(&s->v34_rx, s);\n    s->calling = calling;\n\n"
                   "    /* SIPfax answers: run the V.34 Phase-2 negotiation before Phase 3 */\n"
                   "    s->phase2 = 0; s->phase2_active = 0;\n"
                   "    if (!calling) { s->phase2 = v34_phase2_new(); s->phase2_active = 1; }\n}")
    assert old_init_tail in c, "V34_init tail not found"
    c=c.replace(old_init_tail,new_init_tail,1)
    # replace V34_process body
    old_proc=("int V34_process(struct V34State *s, s16 *output, s16 *input, int nb_samples)\n{\n"
              "    V34_demod(&s->v34_rx, input, nb_samples);\n"
              "    V34_mod(&s->v34_tx, output, nb_samples);\n"
              "    return 0;\n}")
    new_proc=("int V34_process(struct V34State *s, s16 *output, s16 *input, int nb_samples)\n{\n"
              "    if (s->phase2_active) {\n"
              "        int r = v34_phase2_run(s->phase2, output, input, nb_samples);\n"
              "        if (r == 1) {\n"
              "            int sr = v34_phase2_symrate(s->phase2);\n"
              "            if (sr >= 0) s->S = sr;   /* 0..5 == V34_S2400..V34_S3429 */\n"
              "            /* hand off to Phase 3, preserving the serial data callbacks */\n"
              "            get_bit_func gb = s->v34_tx.get_bit; void *go = s->v34_tx.opaque;\n"
              "            put_bit_func pb = s->v34_rx.put_bit; void *po = s->v34_rx.opaque;\n"
              "            s->calling = 0; V34_mod_init(&s->v34_tx, s);\n"
              "            s->v34_tx.get_bit = gb; s->v34_tx.opaque = go;\n"
              "            s->calling = 1; V34_demod_init(&s->v34_rx, s); s->calling = 0;\n"
              "            s->v34_rx.put_bit = pb; s->v34_rx.opaque = po;\n"
              "            v34_phase2_free(s->phase2); s->phase2 = 0; s->phase2_active = 0;\n"
              "        } else if (r == -1) {\n"
              "            return 1;   /* negotiation failed -> hang up */\n"
              "        }\n"
              "        return 0;\n"
              "    }\n"
              "    V34_demod(&s->v34_rx, input, nb_samples);\n"
              "    V34_mod(&s->v34_tx, output, nb_samples);\n"
              "    return 0;\n}")
    assert old_proc in c, "V34_process body not found"
    c=c.replace(old_proc,new_proc,1)
    wr("v34.c",c); print("v34.c wired (decls + V34_init + V34_process)")
else: print("v34.c already patched")
print("INTEGRATION PATCH DONE")
