/* Independent on-wire vectors, not an encoder/decoder round trip. */
#include "lm.h"
int sm_get_bit(struct sm_fifo *f) {
    if (!f->size) return -1;
    --f->size; return *f->rptr++;
}
void sm_put_bit(struct sm_fifo *f, int v) {
    assert(f->size < f->max_size); *f->wptr++ = v; ++f->size;
}
static struct sm_state s;
static unsigned char in[256],out[512];
static void reset(void) {
    memset(&s,0,sizeof(s)); serial_init(&s,8,'N');
    s.tx_fifo.rptr=in; s.rx_fifo.wptr=out; s.rx_fifo.max_size=sizeof(out);
}
int main(void) {
    reset();
    /* Explicit 0x53: start 0, bits 11001010, stop 1. */
    const int vector[10]={0,1,1,0,0,1,0,1,0,1};
    in[0]=0x53; s.tx_fifo.size=1;
    for(int i=0;i<10;i++) assert(serial_8n1_get_bit(&s)==vector[i]);
    for(int i=0;i<24;i++) assert(serial_8n1_get_bit(&s)==1);
    for(int i=0;i<10;i++) serial_8n1_put_bit(&s,vector[i]);
    assert(s.rx_fifo.size==1 && out[0]==0x53);
    reset();
    for(int n=0;n<256;n++) in[n]=n;
    s.tx_fifo.size=256;
    for(int n=0;n<256;n++) {
        assert(serial_8n1_get_bit(&s)==0);
        for(int b=0;b<8;b++) assert(serial_8n1_get_bit(&s)==((n>>b)&1));
        assert(serial_8n1_get_bit(&s)==1);
        /* Generate the receive wire independently, with varying idle gaps. */
        for(int j=0;j<n%5;j++) serial_8n1_put_bit(&s,1);
        serial_8n1_put_bit(&s,0);
        for(int b=0;b<8;b++) serial_8n1_put_bit(&s,(n>>b)&1);
        serial_8n1_put_bit(&s,1);
    }
    assert(s.rx_fifo.size==256 && !memcmp(in,out,256));
    /* Invalid stop is discarded, followed by idle and a valid character. */
    reset();
    for(int i=0;i<10;i++) serial_8n1_put_bit(&s,0);
    assert(!s.rx_fifo.size);
    for(int i=0;i<12;i++) serial_8n1_put_bit(&s,1);
    for(int i=0;i<10;i++) serial_8n1_put_bit(&s,vector[i]);
    assert(s.rx_fifo.size==1 && out[0]==0x53);
    /* Reinitialization discards a partially transmitted/received word. */
    reset(); in[0]=0x53; s.tx_fifo.size=1;
    serial_8n1_get_bit(&s); serial_8n1_put_bit(&s,0);
    serial_init(&s,8,'N');
    assert(serial_8n1_get_bit(&s)==1);
    for(int i=0;i<10;i++) serial_8n1_put_bit(&s,vector[i]);
    assert(s.rx_fifo.size==1 && out[0]==0x53);
    puts("PASS: independent 8N1 vectors, all octets, idle, invalid stop, reset");
}
