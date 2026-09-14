/* V.42 LAPM bridge for the digital V.90 server. GPL-2.0. */
#ifndef V90LAPMLINK_H
#define V90LAPMLINK_H

#include <stdbool.h>
#include <stdint.h>
#include "spandsp/telephony.h"
#include "spandsp/logging.h"
#include "spandsp/async.h"
#include "spandsp/hdlc.h"
#include "spandsp/v42.h"
#include "spandsp/private/logging.h"
#include "spandsp/private/async.h"
#include "spandsp/private/hdlc.h"
#include "spandsp/private/v42.h"
#include "v90lapmselect.h"

#define V90_LAPM_PENDING_BYTES 4096

typedef int (*v90_lapm_get_func)(void *opaque,uint8_t *data,int maximum);
typedef int (*v90_lapm_put_func)(void *opaque,const uint8_t *data,int length);

typedef struct {
    v42_state_t protocol;
    V90LapmSelect selector;
    hdlc_rx_state_t selected_hdlc;
    void *opaque;
    v90_lapm_get_func dte_get;
    v90_lapm_put_func dte_put;
    uint8_t pending[V90_LAPM_PENDING_BYTES];
    unsigned pending_head,pending_count;
    unsigned enabled,initialized,detected,connected,disconnected,errors;
    unsigned selected_candidate,selection_count,restarts,overflow,adp_bits;
    unsigned reacquiring,resumptions;
    unsigned selected_frames,selected_valid_frames,selected_xid_dumped;
} V90LapmLink;

void v90_lapm_link_init(V90LapmLink *s,int tx_bit_rate,void *opaque,
                        v90_lapm_get_func get,v90_lapm_put_func put);
int v90_lapm_link_tx_bit(void *opaque);
void v90_lapm_link_candidate_bit(void *opaque,unsigned candidate,int bit,long source_sample);
void v90_lapm_link_drain(V90LapmLink *s);

#endif
