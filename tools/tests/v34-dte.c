/* Exercise the actual V34 DTE bridge against an originating LAPM endpoint. */
#include "lm.h"
#define TOTAL 16384u
static struct sm_state sm;
static unsigned sent, received, delivered, queued, tick;
static int sm_size_max;
int sm_size(struct sm_fifo *f) { return f->size; }
int sm_get_bit(struct sm_fifo *f) {
    if (!f->size) return -1;
    int v=*f->rptr++; if(f->rptr==f->eptr)f->rptr=f->sptr;
    --f->size;return v;
}
void sm_put_bit(struct sm_fifo *f,int v) {
    assert(f->size<f->max_size);*f->wptr++=v;
    if(f->wptr==f->eptr)f->wptr=f->sptr;
    ++f->size;
}
static void fifo(struct sm_fifo *f,unsigned char *b,int n) {
    f->sptr=f->rptr=f->wptr=b;f->eptr=b+n;f->size=0;f->max_size=n;
}
static unsigned char datum(unsigned n,unsigned side) { return (n*73+(n>>8)*19+side*31)&255; }
static int source(void *opaque,uint8_t *data,int maximum) {
    (void)opaque;int n=0;
    while(n<maximum && sent<TOTAL)data[n++]=datum(sent++,1);
    return n;
}
static void sink(void *opaque,const uint8_t *data,int n) {
    (void)opaque;
    for(int i=0;i<n;i++){assert(received<TOTAL);assert(data[i]==datum(received++,0));}
}
static void status(void *opaque,int code) {(void)opaque;(void)code;}
static void run(int retrain) {
    memset(&sm,0,sizeof(sm));sent=received=delivered=queued=tick=0;
    fifo(&sm.tx_fifo,sm.tx_fifo_buf,SM_FIFO_SIZE);
    fifo(&sm.rx_fifo,sm.rx_fifo_buf,128); /* Deliberately force partial DTE writes. */
    sm.u.v34_state.v34_tx.R=12000;v34_dte_init(&sm,1);
    v42_state_t caller;v42_init(&caller,true,true,source,sink,0);
    caller.config.comp=0;caller.tx_bit_rate=14400;
    v42_set_status_callback(&caller,status,0);v42_restart(&caller);
    unsigned up=0,down=0,reset_at=0,max_pending=0,busy=0;
    unsigned long long tx_bits=0,rx_bits=0,rx_ones=0;
    for(tick=0;tick<8000*60;tick++) {
        sm.v34_lapm_samples=tick;
        if(retrain && !reset_at && received>1024 && delivered>1024) {
            assert(sm.v34_lapm.connected);
            v34_dte_retrain(&sm);assert(sm.v34_lapm.reacquiring);
            hdlc_rx_restart(&caller.lapm.hdlc_rx);reset_at=tick;
            sm.u.v34_state.v34_tx.R=9600;
        }
        if(reset_at && tick-reset_at<8000)continue;
        while(queued<TOTAL && sm.tx_fifo.size<sm.tx_fifo.max_size)
            sm_put_bit(&sm.tx_fifo,datum(queued++,0));
        up+=14400;
        while(up>=8000){up-=8000;int b=v42_tx_bit(&caller);rx_bits++;rx_ones+=b&1;v34_dte_put_bit(&sm,b);}
        down+=sm.u.v34_state.v34_tx.R;
        while(down>=8000){down-=8000;tx_bits++;v42_rx_bit(&caller,v34_dte_get_bit(&sm));}
        v90_lapm_link_drain(&sm.v34_lapm);
        if(sm.v34_lapm.pending_count>max_pending)max_pending=sm.v34_lapm.pending_count;
        if(sm.v34_lapm.protocol.lapm.local_busy)busy=1;
        if(sm.rx_fifo.size>sm_size_max)sm_size_max=sm.rx_fifo.size;
        if(tick>8000*5)for(int j=0;j<17;j++) {
            int c=sm_get_bit(&sm.rx_fifo);if(c<0)break;
            assert(delivered<TOTAL && c==datum(delivered++,1));
        }
        if(delivered==TOTAL && received==TOTAL)break;
    }
    assert(delivered==TOTAL && received==TOTAL && sent==TOTAL && queued==TOTAL);
    assert(sm.v34_lapm.connected && !sm.v34_lapm.errors && !sm.v34_lapm.overflow);
    assert(sm.v34_lapm.protocol.tx_bit_rate==(retrain?9600:12000));
    assert(busy && max_pending<=V90_LAPM_PENDING_BYTES && sm_size_max==128);
    assert(sm.v34_lapm.resumptions==(unsigned)retrain);
    assert(sm.v34_dte_tx_bits==tx_bits && sm.v34_dte_rx_bits==rx_bits);
    assert(sm.v34_dte_rx_ones==rx_ones && sm.v34_dte_retrains==(unsigned)retrain);
    v34_dte_init(&sm,1);assert(!sm.v34_lapm.initialized);
    assert(!sm.v34_dte_tx_bits && !sm.v34_dte_rx_bits && !sm.v34_dte_rx_ones && !sm.v34_dte_retrains);
    printf("PASS: V34 LAPM 16KiB each direction, backpressure, retrain=%d\n",retrain);
}
int main(void) {run(0);run(1);return 0;}
