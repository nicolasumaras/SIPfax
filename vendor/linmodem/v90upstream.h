#ifndef V90UPSTREAM_H
#define V90UPSTREAM_H
#include <stdint.h>
#include "v90trellis.h"
#include "v90qam8.h"
#define V90_UP_TAPS 81
#define V90_UP_PHASES 10
#define V90_UP_FRAME 4096
#define V90_UP_RECENT 128
#define V90_UP_B1_SYMBOLS 128
typedef struct {
    double re[V90_UP_B1_SYMBOLS],im[V90_UP_B1_SYMBOLS];
    unsigned position,count;
} V90UpB1Lane;
typedef struct {
    double a_re,a_im,previous_re,previous_im;
    unsigned have_a,have_previous,scrambler,uart_count,uart_value;
    unsigned length,escape,overflow,crc;
    long source_sample;
    uint8_t frame[V90_UP_FRAME];
} V90UpLane;
struct V90Upstream;
typedef struct {
    V90TrellisStream stream;
    V90UpLane lane;
    struct V90Upstream *up;
    unsigned previous,have_previous,phase;
} V90UpSoftLane;
typedef struct {
    V90Qam8Stream stream;
    V90UpLane lane;
    struct V90Upstream *up;
    unsigned phase;
    double next_symbol,timing_frequency,previous_re,previous_im,previous_time;
    double symbol_time[256];
    unsigned have_timing_previous;
} V90UpQamLane;
typedef struct V90Upstream {
    double taps[4][V90_UP_TAPS],re[V90_UP_TAPS],im[V90_UP_TAPS];
    unsigned position,frames;
    /* A well-formed, uncompressed LCP Configure packet proves PPP startup.
     * An arbitrary FCS match must not disable initial recovery. */
    unsigned lcp_seen;
    long samples,last_frame_sample;
    unsigned last_length;
    uint8_t last_frame[V90_UP_FRAME];
    /* Acquisition can replay at most 123 minimal frames per lane in a burst. */
    struct {long sample;unsigned length;uint8_t frame[V90_UP_FRAME];} recent[V90_UP_RECENT];
    unsigned recent_count,recent_next;
    V90UpLane lanes[V90_UP_PHASES][2];
    unsigned soft_enabled;
    V90UpSoftLane soft[V90_UP_PHASES];
    unsigned rate,symbol_rate,high_carrier;
    double carrier,symbol_period;
    V90UpQamLane qam[V90_UP_PHASES];
    double filtered_re[32],filtered_im[32];
    /* Known 4800/3200 B1, observed independently of data acquisition. */
    unsigned b1_seen,require_b1;
    long b1_sample;
    double b1_score;
    uint8_t b1_labels[V90_UP_B1_SYMBOLS];
    V90UpB1Lane b1[V90_UP_PHASES];
    void *opaque;
    void (*receive_frame)(void *,const uint8_t *,unsigned);
} V90Upstream;
unsigned v90_upstream_configured_rate(void);
void v90_upstream_init(V90Upstream *s);
/* Explicit per-call rate; invalid values use the default 4800 profile. */
void v90_upstream_init_rate(V90Upstream *s,unsigned rate);
/* Explicit PCM profile; high_carrier is 0 or 1. Invalid profiles return 0
 * without changing state. No environment/negotiation selection is implied. */
int v90_upstream_init_profile(V90Upstream *,unsigned rate,unsigned symbol_rate,unsigned high_carrier);
void v90_upstream_receive(V90Upstream *s,int16_t sample);
#endif
