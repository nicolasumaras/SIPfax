/* Experimental V.34 upstream receiver for V.90: 4800..31200 bit/s at 3200, 4800..28800 at 3000 symbols/s.
 * Decode 4D pairs, GPA, 8N1, then verify PPP FCS before delivering a frame.
 * Ten timing phases and both pair alignments allow CRC-based acquisition.
 * Default receiver hard-slices; SIPFAX_V90_SOFT_RX=1 enables experimental
 * streaming trellis correction. The eight-point path tracks symbol timing
 * with a normalized Gardner loop over quarter-sample filter outputs. GPL-2.0. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v90upstream.h"
static void soft_pair(void *,unsigned,unsigned);
static void qam_bits(void *,const uint8_t *);
unsigned v90_upstream_configured_rate(void)
{
    const char *rate=getenv("SIPFAX_V90_UPSTREAM_RATE");
    return rate && !strcmp(rate,"31200")?31200:rate && !strcmp(rate,"28800")?28800:rate && !strcmp(rate,"26400")?26400:rate && !strcmp(rate,"24000")?24000:rate && !strcmp(rate,"21600")?21600:rate && !strcmp(rate,"19200")?19200:rate && !strcmp(rate,"16800")?16800:rate && !strcmp(rate,"14400")?14400:rate && !strcmp(rate,"12000")?12000:rate && !strcmp(rate,"9600")?9600:rate && !strcmp(rate,"7200")?7200:4800;
}
void v90_upstream_init(V90Upstream *s)
{
    v90_upstream_init_rate(s,v90_upstream_configured_rate());
}
void v90_upstream_init_rate(V90Upstream *s,unsigned rate)
{
    v90_upstream_init_profile(s,rate>=4800 && rate<=31200 && rate%2400==0?rate:4800,3200,1);
}
int v90_upstream_init_profile(V90Upstream *s,unsigned rate,unsigned symbol_rate,unsigned high_carrier)
{
    V90Mapping mapping;
    if(!s || high_carrier>1 || !v90_mapping_init(&mapping,rate,symbol_rate))return 0;
    memset(s,0,sizeof(*s));s->last_frame_sample=-1000;
    const char *guard=getenv("SIPFAX_V90_ERASURE_GUARD");
    s->erasure_guard=guard && !strcmp(guard,"1");
    s->rate=rate;s->symbol_rate=symbol_rate;s->high_carrier=high_carrier;s->symbol_period=32000.0/symbol_rate;
    s->carrier=high_carrier?mapping.high_carrier:mapping.low_carrier;
    if(s->rate!=4800 || s->symbol_rate==3000)for(unsigned i=0;i<V90_UP_PHASES;++i) {
        V90UpQamLane *l=&s->qam[i];l->up=s;l->phase=i;l->lane.crc=0xffff;
        l->next_symbol=i*s->symbol_period/V90_UP_PHASES;
        v90_qam_stream_init_profile(&l->stream,s->rate,s->symbol_rate);
        l->stream.opaque=l;l->stream.receive_bits=qam_bits;
    }
    /* V.34 10.1.3.1: one frame of scrambled ones, zero encoder state,
       inversion as the last frame of the J=7 superframe. */
    unsigned bits[192],state=0,previous=0;
    for(unsigned i=0;i<192;++i)
        bits[i]=1^(i>=5?bits[i-5]:0)^(i>=23?bits[i-23]:0);
    for(unsigned i=0;i<64;++i) {
        unsigned a=(previous+bits[3*i+1]+2*bits[3*i+2])&3;
        unsigned b=(a+2*bits[3*i]+((state&1)^(i==0)))&3;
        s->b1_labels[2*i]=a;s->b1_labels[2*i+1]=b;previous=a;
        unsigned y1=((a&1)&((b&1)^1))^(a>>1)^(b>>1),y2=a&1,u=state&1;
        state=(state>>1)^y1^(y2<<1)^((y2^u)<<2)^(u<<3);
    }
    const char *option=getenv("SIPFAX_V90_SOFT_RX");
    s->soft_enabled=option && !strcmp(option,"1");
    if(s->soft_enabled)for(unsigned i=0;i<V90_UP_PHASES;++i) {
        V90UpSoftLane *l=&s->soft[i];l->up=s;l->phase=i;l->lane.crc=0xffff;
        v90_trellis_stream_init(&l->stream);
        l->stream.opaque=l;l->stream.receive_pair=soft_pair;
    }
    double beta=.1,sps=8000.0/s->symbol_rate;
    for(int fraction=0;fraction<4;++fraction)for(int k=0;k<V90_UP_TAPS;++k) {
        double *taps=s->taps[fraction];
        double t=(k-(V90_UP_TAPS-1)/2-fraction*.25)/sps;
        if(fabs(t)<1e-9)taps[k]=1-beta+4*beta/M_PI;
        else if(fabs(fabs(t)-1/(4*beta))<1e-9)
            taps[k]=beta/sqrt(2)*((1+2/M_PI)*sin(M_PI/(4*beta))+(1-2/M_PI)*cos(M_PI/(4*beta)));
        else taps[k]=(sin(M_PI*t*(1-beta))+4*beta*t*cos(M_PI*t*(1+beta)))/(M_PI*t*(1-16*beta*beta*t*t));
    }
    for(int i=0;i<V90_UP_PHASES;++i)for(int j=0;j<2;++j)s->lanes[i][j].crc=0xffff;
    return 1;
}
/* RFC 1661 sections 5 and 6.6: initial Configure packets have an
 * uncompressed ff03/c021 header. Length excludes FCS and permits padding.
 * Keep forwarding other FCS-valid frames to pppd for protocol handling. */
static int startup_lcp(const uint8_t *p,unsigned n)
{
    if(n<10 || p[0]!=0xff || p[1]!=3 || p[2]!=0xc0 || p[3]!=0x21 || p[4]<1 || p[4]>4)return 0;
    unsigned length=((unsigned)p[6]<<8)|p[7];
    if(length<4 || length>n-6)return 0;
    unsigned end=4+length;
    for(unsigned at=8;at<end;){
        if(end-at<2 || p[at+1]<2 || p[at+1]>end-at)return 0;
        at+=p[at+1];
    }
    return 1;
}
static void byte(V90Upstream *s,V90UpLane *l,unsigned value)
{
    if(value==0x7e) {
        if((!s->require_b1 || s->b1_seen) && !l->overflow && !l->escape && l->length>=4 && l->crc==0xf0b8) {
            unsigned duplicate=0;
            for(unsigned i=0;i<s->recent_count;++i)
                if(labs(l->source_sample-s->recent[i].sample)<40 && s->recent[i].length==l->length &&
                   !memcmp(s->recent[i].frame,l->frame,l->length)){duplicate=1;break;}
            if(!duplicate) {
                unsigned i=s->recent_next;
                s->recent[i].sample=l->source_sample;s->recent[i].length=l->length;
                memcpy(s->recent[i].frame,l->frame,l->length);
                s->recent_next=(i+1)%V90_UP_RECENT;
                if(s->recent_count<V90_UP_RECENT)++s->recent_count;
                if(startup_lcp(l->frame,l->length))s->lcp_seen=1;
                ++s->frames;s->last_frame_sample=s->samples;s->last_length=l->length;
                memcpy(s->last_frame,l->frame,l->length);
                fprintf(stderr,"[v90data] CRC-valid PPP frame %u bytes at %.6fs\n",l->length,s->samples/8000.0);
                if(s->receive_frame)s->receive_frame(s->opaque,l->frame,l->length);
            }
        }
        l->length=0;l->escape=0;l->overflow=0;l->crc=0xffff;return;
    }
    if(value==0x7d){l->escape=1;return;}
    if(l->escape){value^=0x20;l->escape=0;}
    if(l->length>=V90_UP_FRAME){l->overflow=1;return;}
    l->frame[l->length++]=value;l->crc^=value;
    for(int k=0;k<8;++k)l->crc=(l->crc>>1)^((l->crc&1)?0x8408:0);
}
static void bit(V90Upstream *s,V90UpLane *l,unsigned candidate,unsigned b)
{
    unsigned plain=((l->scrambler>>22)^b)&1;
    if(s->b1_seen && !s->lcp_seen && v42_detect_bit(&l->v42,plain) && !s->odp_seen){
        s->odp_seen=1;
        fprintf(stderr,"[v42] ODP detected at upstream sample %ld\n",s->samples);
    }
    l->scrambler=(l->scrambler<<1)&0x7fffff;
    if(b)l->scrambler^=1|(1<<18);
    if(s->receive_bit)s->receive_bit(s->bit_opaque,candidate,(int)plain,l->source_sample);
    if(!l->uart_count) {if(!plain){l->uart_count=1;l->uart_value=0;}return;}
    if(l->uart_count<=8) {l->uart_value|=plain<<(l->uart_count-1);++l->uart_count;return;}
    l->uart_count=0;if(plain)byte(s,l,l->uart_value);
}
static void soft_pair(void *opaque,unsigned a,unsigned b)
{
    V90UpSoftLane *l=opaque;
    l->lane.source_sample=(long)((10*l->stream.output_symbol+l->phase)/4);
    if(l->have_previous) {
        unsigned d=(b-a)&3,q=(a-l->previous)&3;
        bit(l->up,&l->lane,V90_UP_PHASES+l->phase,d>>1);bit(l->up,&l->lane,V90_UP_PHASES+l->phase,q&1);bit(l->up,&l->lane,V90_UP_PHASES+l->phase,q>>1);
    }
    l->previous=a;l->have_previous=1;
}
static void qam_bits(void *opaque,const uint8_t *bits)
{
    V90UpQamLane *l=opaque;
    if(l->stream.output_frames==1 || !bits) {
        if(l->up->receive_bit)l->up->receive_bit(l->up->bit_opaque,l->phase,-1,l->lane.source_sample);
        memset(&l->lane,0,sizeof(l->lane));l->lane.crc=0xffff;
    }
    if(!bits)return;
    l->lane.source_sample=(long)(l->symbol_time[l->stream.output_symbol%256]/4);
    for(unsigned i=0;i<l->stream.frame_bits;++i)bit(l->up,&l->lane,l->phase,bits[i]);
}
static unsigned delta(double ar,double ai,double br,double bi)
{
    return (-(int)lrint(atan2(ai*br-ar*bi,ar*br+ai*bi)/(M_PI/2)))&3;
}
static void b1_symbol(V90Upstream *s,unsigned phase,double re,double im)
{
    if(s->b1_seen)return;
    V90UpB1Lane *l=&s->b1[phase];
    if(!isfinite(re) || !isfinite(im)) {memset(l,0,sizeof(*l));return;}
    l->re[l->position]=re;l->im[l->position]=im;
    l->position=(l->position+1)%V90_UP_B1_SYMBOLS;
    if(l->count<V90_UP_B1_SYMBOLS)++l->count;
    if(l->count<V90_UP_B1_SYMBOLS)return;
    double cr=0,ci=0,energy=0;
    for(unsigned i=0;i<V90_UP_B1_SYMBOLS;++i) {
        unsigned j=(l->position+i)%V90_UP_B1_SYMBOLS;
        double ar=l->re[j],ai=l->im[j];
        energy+=ar*ar+ai*ai;
        switch(s->b1_labels[i]) {
        case 0:cr+=ar;ci+=ai;break;
        case 1:cr-=ai;ci+=ar;break;
        case 2:cr-=ar;ci-=ai;break;
        case 3:cr+=ai;ci-=ar;break;
        }
    }
    double power=cr*cr+ci*ci;
    if(energy>0 && isfinite(energy) && isfinite(power) &&
       power>=.9*.9*V90_UP_B1_SYMBOLS*energy) {
        s->b1_seen=1;s->b1_sample=s->samples;
        s->b1_score=sqrt(power/(V90_UP_B1_SYMBOLS*energy));
    }
}
static void filtered_at(V90Upstream *s,double time,double *re,double *im)
{
    long index=(long)floor(time);double fraction=time-index;
    unsigned a=(unsigned)index%32,b=(a+1)%32;
    *re=s->filtered_re[a]+fraction*(s->filtered_re[b]-s->filtered_re[a]);
    *im=s->filtered_im[a]+fraction*(s->filtered_im[b]-s->filtered_im[a]);
}
static void symbol(V90Upstream *s,long time,double re,double im)
{
    if(s->rate!=4800 || s->symbol_rate==3000) {
        s->filtered_re[time%32]=re;s->filtered_im[time%32]=im;
        for(unsigned phase=0;phase<V90_UP_PHASES;++phase) {
            V90UpQamLane *l=&s->qam[phase];V90Qam8Stream *q=&l->stream;
            if(time<l->next_symbol)continue;
            double ar,ai;filtered_at(s,l->next_symbol,&ar,&ai);
            l->symbol_time[q->symbols%256]=l->next_symbol;
            /* Half-symbol FIR input: the earlier midpoint is available in
             * the same quarter-sample matched-filter history as this symbol. */
            if(s->rate>=26400 && l->next_symbol>=s->symbol_period/2) {
                filtered_at(s,l->next_symbol-s->symbol_period/2,&q->mid_re,&q->mid_im);q->have_mid=1;
            }
            int locked=v90_qam8_stream_symbol(q,ar,ai);
            double error=0;
            if(locked==1 && l->have_timing_previous && l->next_symbol>=s->symbol_period/2) {
                double mr,mi;filtered_at(s,(l->next_symbol+l->previous_time)/2,&mr,&mi);
                double energy=ar*ar+ai*ai+l->previous_re*l->previous_re+
                    l->previous_im*l->previous_im+2*(mr*mr+mi*mi);
                /* Gardner detector, normalized to signal energy. Positive
                 * error samples later. Keep phase and clock corrections
                 * separate; the phase term must not become clock drift. */
                if(energy>1e-12)error=((l->previous_re-ar)*mr+(l->previous_im-ai)*mi)/energy;
                /* Acquire 21.6 kbit/s with the fast loop
                 * for three seconds after B1. Then reduce integrator noise
                 * while retaining the learned frequency and phase state.
                 * The longer 24 kbit/s equalizer uses slow acquisition to
                 * avoid outrunning its decision-directed adaptation. */
                l->timing_frequency+=(s->rate==21600 && q->symbols-q->origin<9600?.0001:.00001)*error;
                if(l->timing_frequency>.002)l->timing_frequency=.002;
                if(l->timing_frequency<-.002)l->timing_frequency=-.002;
            }
            l->previous_time=l->next_symbol;
            /* Narrow the phase correction with the tracking integrator so
             * steady-state timing noise does not dominate the denser QAM. */
            /* Dense 28.8/31.2 kbit/s and 26.4/3000 startup need less Gardner phase jitter;
             * retain the existing frequency integrator and steady-state gain. */
            double phase_gain=s->rate>=19200 && q->symbols-q->origin>=9600?.02:s->rate==31200?.02:(s->rate==28800 || (s->symbol_rate==3000 && s->rate==26400))?.05:.1;
            l->next_symbol+=s->symbol_period+l->timing_frequency+phase_gain*error;
            l->previous_re=ar;l->previous_im=ai;l->have_timing_previous=locked>=0;
            if(locked==1 && !s->b1_seen) {
                s->b1_seen=1;s->b1_sample=s->samples;s->b1_score=q->score;
            }
        }
        return;
    }
    b1_symbol(s,time%V90_UP_PHASES,re,im);
    if(s->soft_enabled) {
        v90_trellis_stream_symbol(&s->soft[time%V90_UP_PHASES].stream,re,im);
        return;
    }
    for(unsigned pair=0;pair<2;++pair) {
        V90UpLane *l=&s->lanes[time%V90_UP_PHASES][pair];
        l->source_sample=s->samples;
        if(((time/V90_UP_PHASES)&1)==pair){l->a_re=re;l->a_im=im;l->have_a=1;}
        else if(l->have_a) {
            if(l->have_previous) {
                unsigned d=delta(re,im,l->a_re,l->a_im);
                unsigned q=delta(l->a_re,l->a_im,l->previous_re,l->previous_im);
                unsigned candidate=2*V90_UP_PHASES+2*(time%V90_UP_PHASES)+pair;
                bit(s,l,candidate,d>>1);bit(s,l,candidate,q&1);bit(s,l,candidate,q>>1);
            }
            l->previous_re=l->a_re;l->previous_im=l->a_im;l->have_previous=1;l->have_a=0;
        }
    }
}
void v90_upstream_receive(V90Upstream *s,int16_t input)
{
    if(s->erasure_guard) {
        double energy=(double)input*input;
        s->erasure_sum+=energy-s->erasure_energy[s->erasure_position];
        s->erasure_energy[s->erasure_position]=energy;
        s->erasure_position=(s->erasure_position+1)%8;
        if(s->erasure_count<8)++s->erasure_count;
        double power=s->erasure_sum/s->erasure_count;
        if(s->erasure_reference<=0)s->erasure_reference=power;
        if(s->b1_seen && s->erasure_count==8 && power<.05*s->erasure_reference) {
            /* Hold through the matched filter, equalizer, survivor and saved
             * feedback histories; symbol/sample clocks continue throughout. */
            s->erasure_hold=V90_UP_TAPS+(unsigned)ceil(
                (V90_EQ_MAX_TAPS+2*V90_TRELLIS_DEPTH+V90_QAM_FEEDBACK_HISTORY)*8000.0/s->symbol_rate);
        } else if(!s->erasure_hold) {
            s->erasure_reference+=(power-s->erasure_reference)/1024;
        }
        for(unsigned i=0;i<V90_UP_PHASES;++i)
            s->qam[i].stream.feedback_inhibited=s->erasure_hold!=0;
        if(s->erasure_hold)--s->erasure_hold;
    }
    double phase=2*M_PI*s->carrier*s->samples/8000.0;
    s->re[s->position]=input*cos(phase);s->im[s->position]=-input*sin(phase);
    /* Evaluate the matched filter at quarter-sample instants. Averaging
       adjacent outputs attenuates/distorts the wideband baseband signal. */
    for(int fraction=3;fraction>=0;--fraction) {
        double re=0,im=0;
        for(int k=0;k<V90_UP_TAPS;++k) {
            unsigned j=(s->position+V90_UP_TAPS-k)%V90_UP_TAPS;
            re+=s->taps[fraction][k]*s->re[j];
            im+=s->taps[fraction][k]*s->im[j];
        }
        if(4*s->samples>=fraction)
            symbol(s,4*s->samples-fraction,re,im);
    }
    s->position=(s->position+1)%V90_UP_TAPS;
    ++s->samples;
}
