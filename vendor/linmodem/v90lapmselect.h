/* Candidate-safe V.42 receive selection. GPL-2.0. */
#ifndef V90LAPMSELECT_H
#define V90LAPMSELECT_H
#include <stdbool.h>
#include <stdint.h>
#include "v42detect.h"
#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/async.h"
#include "spandsp/hdlc.h"
#include "spandsp/private/hdlc.h"
#include "v90upstream.h"
#define V90_LAPM_HISTORY_BITS 128
#define V90_LAPM_BUFFER_BITS 8192
typedef struct V90LapmSelect V90LapmSelect;
typedef struct {
    V90LapmSelect *owner;
    hdlc_rx_state_t hdlc;
    V42Detect detection;
    uint8_t history[V90_LAPM_HISTORY_BITS];
    uint8_t buffered[V90_LAPM_BUFFER_BITS];
    unsigned id,history_position,history_count,buffered_count,active;
    unsigned odp_pending,odp_reported,trailing_marks;
    unsigned post_bits,post_zeros,post_transitions,post_flags,post_shift,post_previous;
    unsigned hdlc_frames,hdlc_valid_frames;
} V90LapmCandidate;
struct V90LapmSelect {
    V90LapmCandidate candidate[V90_UP_CANDIDATES];
    int selected;
    unsigned detections,selections,invalidations,overflows;
    void *opaque;
    void (*odp)(void *opaque,unsigned candidate,const uint8_t *bits,unsigned count);
    void (*output)(void *opaque,int bit);
    void (*selection)(void *opaque,unsigned candidate);
};
void v90_lapm_select_init(V90LapmSelect *s,void *opaque,
                          void (*odp)(void *,unsigned,const uint8_t *,unsigned),
                          void (*output)(void *,int),
                          void (*selection)(void *,unsigned));
void v90_lapm_select_reset(V90LapmSelect *s);
void v90_lapm_select_bit(void *opaque,unsigned candidate,int bit,long source_sample);
#endif
