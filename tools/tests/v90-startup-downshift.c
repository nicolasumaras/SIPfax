#include <assert.h>
#include <stdlib.h>
#include "v90startup.c"
static unsigned consumed;
static int bit(void *opaque){(void)opaque;++consumed;return 1;}
static void prepare(V90Startup *s,unsigned rate,unsigned baud,long rtd,long offset)
{
    v90_startup_init(s,0);s->upstream_max_rate=rate;s->upstream_rate_limit=rate;
    s->peer_large_constellations=1;s->peer_carriers=15;s->forced_symbol_rate=baud;
    select_upstream_rate(s);s->upstream_rate=baud==3000?3:4;s->downstream_rate=6;
    assert(select_upstream_profile(s));s->samples=20000;s->ranging_state=9;s->info0_received=1;
    s->round_trip=rtd;s->phase4_active=1;
    assert(v90_phase4_init_profile(&s->phase4,0,78,rate,baud,1));
    s->phase4.stage=4;s->phase4.rx_e_logged=1;
    V90Cp cp={0};cp.drn=9;cp.sr=1;cp.lookahead=1;cp.count=1;cp.filter[0]=63;
    unsigned u[4]={53,78,88,96};for(unsigned i=0;i<4;++i)cp.mask[0][0][u[i]]=1;
    assert(!v90_pcm_init(&s->phase4.encoder,&cp,bit,0));consumed=0;
    s->phase4.upstream.b1_seen=1;s->phase4.upstream.b1_sample=320;
    s->phase4.upstream.samples=320+80000+2*(rtd>0?rtd:0)+offset;
}
int main(void)
{
    V90Startup *s=malloc(sizeof(*s));assert(s);
    long rtds[]={-100,0,160,1000};unsigned cases=0;
    for(unsigned baud=3000;baud<=3200;baud+=200)
    for(unsigned rate=4800;rate<=(baud==3000?28800u:31200u);rate+=2400)
    for(unsigned j=0;j<4;++j)for(long offset=-1;offset<=1;++offset) {
        prepare(s,rate,baud,rtds[j],offset);int16_t in=0,out=0;
        v90_startup_process(s,&out,&in,1);
        int expected=rate>4800 && offset>=0;assert(s->retrains==(unsigned)expected);
        if(expected) {
            assert(!out && !consumed && !s->phase4_active);
            assert(s->upstream_max_rate==rate && s->upstream_rate_limit==rate-2400);
            assert(s->upstream_data_rate==rate-2400 && s->forced_symbol_rate==baud);
            unsigned char offer[109];memcpy(offer,s->info1d,109);
            begin_retrain(s,"preservation test");assert(!memcmp(offer,s->info1d,109));
        }
        ++cases;
    }
    for(unsigned guard=0;guard<9;++guard) {
        prepare(s,28800,3000,160,1);
        switch(guard){
        case 0:s->phase4_active=0;break;
        case 1:s->phase4.stage=7;break;
        case 2:s->have_upstream_data=1;break;
        case 3:s->phase4.renegotiations=1;break;
        case 4:s->phase4.rx_e_logged=0;break;
        case 5:s->phase4.cp.silence=1;break;
        case 6:s->phase4.upstream.b1_seen=0;break;
        case 7:s->phase4.upstream.lcp_seen=1;break;
        case 8:s->phase4.upstream.samples=s->phase4.upstream.b1_sample-1;break;
        }
        int16_t in=0,out=0;v90_startup_process(s,&out,&in,1);assert(!s->retrains);
        if(guard==7){assert(s->have_upstream_data);begin_retrain(s,"data preservation");assert(s->have_upstream_data);}
        ++cases;
    }
    /* Hardware controls first deliver PPP at about 5.88s after E.
       A six-second quiet interval must survive, then valid data permanently
       disarms startup downshift even beyond the later deadline. */
    prepare(s,28800,3200,160,0);
    s->phase4.upstream.samples=48000;
    int16_t in=0,out=0;v90_startup_process(s,&out,&in,1);
    assert(!s->retrains && s->upstream_rate_limit==28800);
    s->phase4.upstream.lcp_seen=1;v90_startup_process(s,&out,&in,1);
    assert(s->have_upstream_data);
    s->phase4.upstream.samples=160000;v90_startup_process(s,&out,&in,1);
    assert(!s->retrains && s->upstream_rate_limit==28800);++cases;
    /* Random FCS matches and non-LCP traffic do not prove link startup. */
    prepare(s,28800,3000,160,1);s->phase4.upstream.frames=1;
    v90_startup_process(s,&out,&in,1);
    assert(s->retrains==1 && !s->have_upstream_data && s->upstream_rate_limit==26400);++cases;
    prepare(s,28800,3000,160,1);s->phase4.upstream.frames=1;
    begin_retrain(s,"FCS-only preservation");assert(!s->have_upstream_data);++cases;
    prepare(s,28800,3000,160,1);s->phase4.upstream.odp_seen=1;
    v90_startup_process(s,&out,&in,1);assert(s->retrains==1 && !s->have_upstream_data);++cases;
    free(s);printf("PASS: %u startup downshift deadline/profile/floor/guard cases\n",cases);return 0;
}
