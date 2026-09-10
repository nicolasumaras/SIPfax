/* Exercise feedback history wrap and reacquisition under ASan/UBSan.
 * Constellation/protocol correctness is checked by the independent Python
 * transmitter tests; this test targets storage lifetime and index bounds. */
#include <assert.h>
#include <stdlib.h>
#include "v90qam8.c"

int main(void)
{
    V90Qam8Stream *s=malloc(sizeof(*s));assert(s);
    const unsigned rates[]={14400,16800,19200,21600,24000};
    for(unsigned rate=0;rate<sizeof(rates)/sizeof(rates[0]);++rate) {
        assert(v90_qam_stream_init_rate(s,rates[rate]));
        for(unsigned repeat=0;repeat<3;++repeat) {
            for(unsigned n=0;n<4096;++n) {
                double r,i;point(s->b1.labels[n%128],&r,&i);
                int ready=v90_qam8_stream_symbol(s,r,i);
                assert(ready>=0);
            }
            assert(s->locked && s->output_frames>400);
            assert(s->equalizer.taps==(rates[rate]==24000?15:7));
            assert(v90_qam8_stream_symbol(s,NAN,0)==-1);
        }
    }
    free(s);return 0;
}
