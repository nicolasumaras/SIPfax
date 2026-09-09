#ifndef V90UPSTREAM_H
#define V90UPSTREAM_H
#include <stdint.h>
#include "v90trellis.h"
#define V90_UP_TAPS 81
#define V90_UP_PHASES 10
#define V90_UP_FRAME 4096
typedef struct {
    double a_re,a_im,previous_re,previous_im;
    unsigned have_a,have_previous,scrambler,uart_count,uart_value;
    unsigned length,escape,overflow,crc;
    uint8_t frame[V90_UP_FRAME];
} V90UpLane;
struct V90Upstream;
typedef struct {
    V90TrellisStream stream;
    V90UpLane lane;
    struct V90Upstream *up;
    unsigned previous,have_previous;
} V90UpSoftLane;
typedef struct V90Upstream {
    double taps[4][V90_UP_TAPS],re[V90_UP_TAPS],im[V90_UP_TAPS];
    unsigned position,frames;
    long samples,last_frame_sample;
    unsigned last_length;
    uint8_t last_frame[V90_UP_FRAME];
    V90UpLane lanes[V90_UP_PHASES][2];
    unsigned soft_enabled;
    V90UpSoftLane soft[V90_UP_PHASES];
    void *opaque;
    void (*receive_frame)(void *,const uint8_t *,unsigned);
} V90Upstream;
void v90_upstream_init(V90Upstream *s);
void v90_upstream_receive(V90Upstream *s,int16_t sample);
#endif
