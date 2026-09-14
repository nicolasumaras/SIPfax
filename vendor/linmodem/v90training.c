/* Streaming 3000/3200-baud V.90 upstream Ja/CP receiver. GPL-2.0.
 * Ten quarter-sample timing hypotheses; accept only CRC-validated descriptors.
 */
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "v90training.h"
void v90_training_init(V90Training *s)
{
    v90_training_init_profile(s,3200,1);
}
int v90_training_init_profile(V90Training *s,unsigned symbol_rate,unsigned high_carrier)
{
    if(!s || (symbol_rate!=3000 && symbol_rate!=3200) || high_carrier>1)return 0;
    memset(s,0,sizeof(*s));s->symbol_rate=symbol_rate;
    s->carrier=symbol_rate==3000?(high_carrier?2000:1800):(high_carrier?1920:12800.0/7);
    s->symbol_period=32000.0/symbol_rate;
    /* The 3200 low carrier needs finer timing diversity through mu-law;
     * retain the existing high-carrier acquisition path exactly. */
    s->phase_count=symbol_rate==3200 && !high_carrier?V90_RX_MAX_PHASES:V90_RX_PHASES;
    for(unsigned i=0;i<s->phase_count;++i)s->next_symbol[i]=i*s->symbol_period/s->phase_count;
    const double beta=.1,sps=8000.0/symbol_rate;
    for(int fraction=0;fraction<4;++fraction)for(int k=0;k<V90_RX_TAPS;++k) {
        double t=(k-(V90_RX_TAPS-1)/2-fraction*.25)/sps,v;
        if(fabs(t)<1e-9)v=1-beta+4*beta/M_PI;
        else if(fabs(fabs(t)-1/(4*beta))<1e-9)
            v=beta/sqrt(2)*((1+2/M_PI)*sin(M_PI/(4*beta))+(1-2/M_PI)*cos(M_PI/(4*beta)));
        else v=(sin(M_PI*t*(1-beta))+4*beta*t*cos(M_PI*t*(1+beta)))/(M_PI*t*(1-16*beta*beta*t*t));
        s->taps[fraction][k]=v;
    }
    return 1;
}
static void bit(V90Training *s,V90JaLane *lane,unsigned b)
{
    unsigned plain=((lane->scrambler>>22)^b)&1;
    lane->scrambler=(lane->scrambler<<1)&0x7fffff;
    if(b)lane->scrambler^=1|(1<<18); /* GPA */
    if(lane->count) {
        if(lane->count>=V90_JA_MAX_BITS)lane->count=0;
        else {
            lane->bits[lane->count++]=plain;
            unsigned consumed;
            int valid=s->cp_mode?v90_cp_parse(&s->cp,lane->bits,lane->count,&consumed):v90_dil_parse(&s->dil,lane->bits,lane->count,&consumed);
            if(valid==1) {
                s->found++;
                if(s->cp_mode)lane->have_data_cp=s->cp.type && !s->cp.silence;
                if(!s->cp_mode)
                fprintf(stderr,"[v90p3] CRC-valid live Ja at %.6fs: N=%u LSP=%u LTP=%u\n",
                        s->samples/8000.0,s->dil.n,s->dil.lsp,s->dil.ltp);
            }
            if(valid!=0)lane->count=0;
        }
    }
    if(!lane->count && !plain && lane->ones>=17) {
        memset(lane->bits,1,17);lane->bits[17]=0;lane->count=18;
    }
    lane->ones=plain ? lane->ones+1 : 0;
    /* 9.4.1.4 permits E instead of CP-prime. Require a CRC-valid data CP
       on this timing lane, but not its acknowledgement bit. Other timing
       hypotheses must not mistake random bits or pre-CP SCR for E. */
    if(s->cp_mode && lane->have_data_cp && lane->ones==20)s->e_seen=1;
    if(lane->ones>20)lane->ones=20;
}
static void lane_symbol(V90Training *s,unsigned index,double re,double im)
{
    V90JaLane *lane=&s->lanes[index];
    if(lane->have_previous) {
        double dot=re*lane->previous_re+im*lane->previous_im;
        double cross=im*lane->previous_re-re*lane->previous_im;
        int delta=(-(int)lrint(atan2(cross,dot)/(M_PI/2)))&3;
        bit(s,lane,delta&1);
        if(!s->found || s->cp_mode)bit(s,lane,delta>>1);
    }
    lane->previous_re=re;lane->previous_im=im;lane->have_previous=1;
}
static void symbol(V90Training *s,long time,double re,double im)
{
    if(s->symbol_rate==3200 && s->phase_count==V90_RX_PHASES){lane_symbol(s,(unsigned)(time%V90_RX_PHASES),re,im);return;}
    s->filtered_re[time%32]=re;s->filtered_im[time%32]=im;
    for(unsigned i=0;i<s->phase_count && (!s->found || s->cp_mode);++i){
        double next=s->next_symbol[i];if(next>time)continue;
        long index=(long)floor(next);double fraction=next-index;
        unsigned a=(unsigned)index%32,b=(a+1)%32;
        lane_symbol(s,i,s->filtered_re[a]+fraction*(s->filtered_re[b]-s->filtered_re[a]),
                        s->filtered_im[a]+fraction*(s->filtered_im[b]-s->filtered_im[a]));
        s->next_symbol[i]+=s->symbol_period;
    }
}
int v90_training_receive(V90Training *s,const int16_t *pcm,int count)
{
    for(int n=0;n<count && (!s->found || s->cp_mode);++n,++s->samples) {
        double phase=2*M_PI*s->carrier*s->samples/8000.0;
        s->re[s->position]=pcm[n]*cos(phase);s->im[s->position]=-pcm[n]*sin(phase);
        /* Preserve fractional matched-filter response for Ja and CP, as
           in the upstream data receiver. Stop immediately on a valid Ja. */
        for(int fraction=3;fraction>=0 && (!s->found || s->cp_mode);--fraction) {
            double re=0,im=0;
            for(int k=0;k<V90_RX_TAPS;++k) {
                int j=(s->position+V90_RX_TAPS-k)%V90_RX_TAPS;
                re+=s->taps[fraction][k]*s->re[j];
                im+=s->taps[fraction][k]*s->im[j];
            }
            if(4*s->samples>=fraction)symbol(s,4*s->samples-fraction,re,im);
        }
        s->position=(s->position+1)%V90_RX_TAPS;
    }
    return s->found;
}

int v90_s_detect_init_profile(V90SDetect *s,unsigned symbol_rate,unsigned high_carrier)
{
    if(!s || (symbol_rate!=3000 && symbol_rate!=3200) || high_carrier>1)return 0;
    memset(s,0,sizeof(*s));s->symbol_rate=symbol_rate;
    s->carrier=symbol_rate==3000?(high_carrier?2000:1800):(high_carrier?1920:12800.0/7);
    /* Whole carrier/sideband cycles: 10 ms at 3000, 8.75 ms at 3200/low,
     * and the legacy 12.5 ms at 3200/high. Five normalization blocks. */
    s->window=symbol_rate==3000?80:high_carrier?100:70;s->block=s->window/5;
    return 1;
}
void v90_s_detect_reset(V90SDetect *s)
{
    if(!s)return;
    unsigned symbol_rate=s->symbol_rate,window=s->window,block=s->block;double carrier=s->carrier;
    memset(s,0,sizeof(*s));s->symbol_rate=symbol_rate;s->window=window;s->block=block;s->carrier=carrier;
}
int v90_s_detect(V90SDetect *s,int16_t sample)
{
    unsigned window=s->window?s->window:100,block=s->block?s->block:20;
    double carrier=s->symbol_rate?s->carrier:1920,baud=s->symbol_rate?s->symbol_rate:3200;
    double freq[3]={carrier-baud/2,carrier,carrier+baud/2};
    double angle=2*M_PI*carrier*s->samples/8000.0;
    s->short_re+=sample*cos(angle);s->short_im+=sample*sin(angle);
    for(int k=0;k<3;++k) {
        angle=2*M_PI*freq[k]*s->samples/8000.0;
        s->re[k]+=sample*cos(angle);s->im[k]+=sample*sin(angle);
    }
    s->energy+=(double)sample*sample;
    s->block_energy+=(double)sample*sample;
    ++s->samples;
    int event=0;
    if(s->samples%block==0) {
        /* Hardware S ramps up from silence. Normalize each short block
           before the coherence test so the amplitude envelope does
           not look like incoherent energy. Keep an absolute noise gate. */
        double scale=sqrt(s->block_energy/block+1);
        for(int k=0;k<3;++k) {
            s->normalized_re[k]+=s->re[k]/scale;
            s->normalized_im[k]+=s->im[k]/scale;
        }
        memset(s->re,0,sizeof(s->re));memset(s->im,0,sizeof(s->im));
        s->block_energy=0;
        double dot=s->short_re*s->ref_re+s->short_im*s->ref_im;
        double p=s->short_re*s->short_re+s->short_im*s->short_im;
        double r=s->ref_re*s->ref_re+s->ref_im*s->ref_im;
        if(s->latched && !s->reversed && dot<0 && dot*dot>0.64*p*r && p>1000000) {
            s->reversed=1;event=2;
        }
        s->short_re=s->short_im=0;
    }
    if(s->samples%window==0) {
        double power[3],total=0;
        for(int k=0;k<3;++k) {
            power[k]=2*(s->normalized_re[k]*s->normalized_re[k]+s->normalized_im[k]*s->normalized_im[k])/(window*window);
            total+=power[k];
        }
        int good=s->energy>window*900 && total>0.85 && power[0]>0.10 && power[1]>0.20 && power[2]>0.10;
        if(good) {
            s->bad=0;
            if(++s->good>=2 && !s->latched) {
                s->latched=1;s->reversed=0;event=1;
                s->ref_re=s->normalized_re[1];s->ref_im=s->normalized_im[1];
            }
        } else {
            s->good=0;
            if(++s->bad>=3)s->latched=0;
        }
        memset(s->normalized_re,0,sizeof(s->normalized_re));
        memset(s->normalized_im,0,sizeof(s->normalized_im));s->energy=0;
    }
    return event;
}
