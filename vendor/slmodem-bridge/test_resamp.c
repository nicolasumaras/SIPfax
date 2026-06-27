/* Objective resampler quality test: round-trip 8000->9600->8000 of modem-band
 * signals, measure SNR (transparency) and frequency-response flatness.
 * Compares linear vs polyphase-FIR variants so we deploy a measured winner. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---------------- linear (current bridge resampler) ---------------- */
typedef struct { double frac, step, last; int primed; } lin_t;
static void lin_init(lin_t *r, int in, int out){ r->frac=0; r->last=0; r->primed=0; r->step=(double)in/out; }
static int lin_run(lin_t *r, const double *in, int n, double *out, int cap){
    int o=0;
    for(int i=0;i<n;i++){ double x=in[i]; if(!r->primed){r->last=x;r->primed=1;}
        while(r->frac<1.0 && o<cap){ out[o++]=r->last*(1.0-r->frac)+x*r->frac; r->frac+=r->step; }
        r->frac-=1.0; r->last=x; }
    return o;
}

/* ---------------- polyphase FIR ---------------- */
typedef struct { int L,M,taps,plen,phase; double *proto,*hist; } fir_t;
static int igcd(int a,int b){ while(b){int t=a%b;a=b;b=t;} return a; }
static void fir_init(fir_t *r, int in, int out, double fc, int taps){
    int g=igcd(in,out); r->L=out/g; r->M=in/g; r->taps=taps; r->plen=r->L*taps; r->phase=0;
    r->proto=malloc(sizeof(double)*r->plen); r->hist=calloc(taps,sizeof(double));
    double Fsup=(double)in*r->L, c=(r->plen-1)/2.0, sum=0;
    for(int i=0;i<r->plen;i++){ double x=2*fc/Fsup*(i-c);
        double s=fabs(x)<1e-9?1.0:sin(M_PI*x)/(M_PI*x);
        double w=0.54-0.46*cos(2*M_PI*i/(r->plen-1));
        r->proto[i]=2*fc/Fsup*s*w; sum+=r->proto[i]; }
    double sc=(double)r->L/sum; for(int i=0;i<r->plen;i++)r->proto[i]*=sc;
}
static int fir_run(fir_t *r, const double *in, int n, double *out, int cap){
    int o=0;
    for(int i=0;i<n;i++){ for(int k=r->taps-1;k>0;k--)r->hist[k]=r->hist[k-1]; r->hist[0]=in[i];
        while(r->phase<r->L && o<cap){ double acc=0; int br=r->phase;
            for(int k=0;k<r->taps;k++) acc+=r->proto[br+k*r->L]*r->hist[k];
            out[o++]=acc; r->phase+=r->M; }
        r->phase-=r->L; }
    return o;
}

/* round-trip a signal through up(8000->9600) then down(9600->8000) */
#define MAXN 200000
static int roundtrip_lin(const double *in, int n, double *out){
    lin_t up, dn; lin_init(&up,8000,9600); lin_init(&dn,9600,8000);
    static double mid[MAXN]; int o=0;
    for(int i=0;i<n;i+=160){ int b=(n-i)<160?(n-i):160;
        double tmp[400]; int nu=lin_run(&up,in+i,b,tmp,400);
        o+=lin_run(&dn,tmp,nu,out+o,MAXN-o); }
    (void)mid; return o;
}
static int roundtrip_fir(const double *in, int n, double *out, double fc, int taps){
    fir_t up, dn; fir_init(&up,8000,9600,fc,taps); fir_init(&dn,9600,8000,fc,taps);
    int o=0;
    for(int i=0;i<n;i+=160){ int b=(n-i)<160?(n-i):160;
        double tmp[400]; int nu=fir_run(&up,in+i,b,tmp,400);
        o+=fir_run(&dn,tmp,nu,out+o,MAXN-o); }
    free(up.proto);free(up.hist);free(dn.proto);free(dn.hist); return o;
}

/* SNR with best integer-delay + LS-gain alignment (skip startup transient) */
static double measure_snr(const double *in, const double *out, int n){
    int skip=3000, len=n-skip-3000;
    double best=-999;
    for(int d=0; d<=160; d++){
        double sx=0,sy=0,sxy=0,syy=0;
        for(int i=skip;i<skip+len;i++){ double x=in[i], y=out[i+d]; sx+=x*x; sy+=y; sxy+=x*y; syy+=y*y; (void)sy; }
        if(syy<1e-9) continue;
        double gain=sxy/syy;        /* scale out to match in */
        double res=0;
        for(int i=skip;i<skip+len;i++){ double e=in[i]-gain*out[i+d]; res+=e*e; }
        double snr=10*log10(sx/(res+1e-12));
        if(snr>best) best=snr;
    }
    return best;
}

static double tone_gain_db(double freq, double fc, int taps, int use_fir){
    int n=16000; static double in[MAXN], out[MAXN];
    for(int i=0;i<n;i++) in[i]=10000.0*sin(2*M_PI*freq/8000.0*i);
    int m = use_fir ? roundtrip_fir(in,n,out,fc,taps) : roundtrip_lin(in,n,out);
    (void)m;
    /* RMS of in vs out (mid region) */
    double pi=0,po=0; for(int i=3000;i<13000;i++){ pi+=in[i]*in[i]; po+=out[i+8]*out[i+8]; }
    return 10*log10((po/pi)+1e-12);
}

int main(void){
    int n=32000;
    static double in[MAXN], out[MAXN];
    /* stationary multitone across the voiceband (delay-robust for SNR) */
    double tones[]={400,800,1200,1600,2000,2400,2800,3200,3500};
    int nt=sizeof(tones)/sizeof(*tones);
    for(int i=0;i<n;i++){ double s=0; for(int k=0;k<nt;k++) s+=sin(2*M_PI*tones[k]/8000.0*i);
        in[i]=(9000.0/nt)*s; }

    printf("=== round-trip SNR (multitone 400-3500Hz), higher is more transparent ===\n");
    int m;
    m=roundtrip_lin(in,n,out); printf("linear              : %.1f dB  (out=%d)\n", measure_snr(in,out,n<m?n:m), m);
    m=roundtrip_fir(in,n,out,3850,32); printf("FIR fc=3850 taps=32 : %.1f dB\n", measure_snr(in,out,n<m?n:m));
    m=roundtrip_fir(in,n,out,3900,48); printf("FIR fc=3900 taps=48 : %.1f dB\n", measure_snr(in,out,n<m?n:m));
    m=roundtrip_fir(in,n,out,3950,64); printf("FIR fc=3950 taps=64 : %.1f dB\n", measure_snr(in,out,n<m?n:m));

    printf("\n=== frequency response (gain dB; 0=flat, modem cares about flatness to ~3400+) ===\n");
    double freqs[]={500,1000,1500,2000,2500,3000,3200,3400,3600,3700};
    printf("freq   linear   FIR3850/32  FIR3900/48  FIR3950/64\n");
    for(unsigned i=0;i<sizeof(freqs)/sizeof(*freqs);i++){ double f=freqs[i];
        printf("%5.0f  %6.2f   %9.2f   %9.2f   %9.2f\n", f,
            tone_gain_db(f,0,0,0), tone_gain_db(f,3850,32,1),
            tone_gain_db(f,3900,48,1), tone_gain_db(f,3950,64,1));
    }
    return 0;
}
