#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "v34clock.h"

int main(void)
{
    V34ClockHistory h;
    v34_clock_reset(&h);
    assert(v34_clock_mean(&h,1024,99)==99);
    for(unsigned i=1;i<=6145;i++) {
        v34_clock_push(&h,(double)i);
        for(unsigned n=1024;n<=2048;n+=1024) {
            double expected=i<n ? -99 : i-(n-1)/2.0;
            assert(v34_clock_mean(&h,n,-99)==expected);
        }
    }
    assert(v34_clock_mean(&h,0,17)==17);
    assert(v34_clock_mean(&h,2049,17)==17);
    v34_clock_reset(&h);
    for(unsigned i=0;i<2048;i++)v34_clock_push(&h,i%2 ? -0.002 : 0.002);
    assert(fabs(v34_clock_mean(&h,1024,99))<1e-15);
    v34_clock_reset(&h);
    v34_clock_push(&h,1);
    assert(v34_clock_mean(&h,1024,42)==42);
    puts("PASS: 1024/2048 trailing means, multiple wraps, warmup, bounds and pass reset");
    return 0;
}
