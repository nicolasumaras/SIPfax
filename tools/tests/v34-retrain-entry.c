/* Wire-level retrain response: V.34 11.5.2.2 and 11.2.1.2.3. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *v34_phase2_new(void);
void v34_phase2_retrain(void *);
int v34_phase2_run(void *, short *, short *, int);
void v34_phase2_free(void *);

static void check(int block) {
    void *p = v34_phase2_new();
    short in[257], out[257], wire[2560];
    assert(p);
    /* Dirty an initial handshake first: the entry must discard its INFO state. */
    memset(in, 0, sizeof(in));
    for (int i=0;i<5;i++) assert(!v34_phase2_run(p,out,in,160));
    v34_phase2_retrain(p);
    for (int at=0;at<2560;) {
        int n=block; if(n>2560-at)n=2560-at;
        for(int i=0;i<n;i++) in[i]=(short)(2400*cos(2*M_PI*1200*(at+i)/8000));
        assert(!v34_phase2_run(p,out,in,n));
        memcpy(wire+at,out,n*sizeof(*out)); at+=n;
    }
    for(int i=0;i<560;i++) assert(wire[i]==0);
    /* First 50 ms must be unmodulated Tone A, with no INFO/guard carrier. */
    for(int i=560;i<960;i++)
        assert(fabs(wire[i]-4254*cos(2*M_PI*2400*(i-560)/8000))<1.01);
    int reversal=-1;
    for(int i=960;i<2000;i++) {
        double expected=4254*cos(2*M_PI*2400*(i-560)/8000);
        if(wire[i]*expected<0){reversal=i;break;}
    }
    assert(reversal>=960 && reversal<2000);
    /* No arbitrary callback boundary can truncate the silence interval. */
    printf("PASS: block=%d, silence=560 samples, Tone A reversal at %d\n",block,reversal);
    v34_phase2_free(p);
}
static void no_caller_tone(void) {
    void *p=v34_phase2_new();
    short in[160]={0},out[160];
    assert(p);
    /* Initial startup retains INFO0a (2400-Hz DPSK plus 1800-Hz guard). */
    assert(!v34_phase2_run(p,out,in,160));
    assert(out[0]==4300);
    v34_phase2_retrain(p);
    for(int at=0;at<2400;at+=160) {
        assert(!v34_phase2_run(p,out,in,160));
        for(int i=0;i<160;i++) {
            int t=at+i;
            double expected=t<560?0:4254*cos(2*M_PI*2400*(t-560)/8000);
            assert(fabs(out[i]-expected)<1.01);
        }
    }
    v34_phase2_free(p);
    puts("PASS: initial INFO0a preserved; absent Tone B does not trigger ranging");
}
int main(void){check(80);check(160);check(257);no_caller_tone();return 0;}
