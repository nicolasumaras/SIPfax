#ifndef V90PHASE4_H
#define V90PHASE4_H
#include "v90training.h"
#include "v90pcm.h"
#include "v90upstream.h"
typedef struct {
    unsigned samples,stage,rbar_end,trn_start,generated,mp_length,mp_ack,mp_announced;
    unsigned ed_frame,data_start,trn_frames;
    int alaw,uinfo,have_cpt,have_cp,have_ack,rx_e_logged;
    V90Training rx;
    V90Cp cpt,cp;
    V90Pcm encoder;
    V90Upstream upstream;
    unsigned data_bits;
    void *data_opaque;
    int (*get_data_bit)(void *);
    uint8_t mp[132];
    int16_t frame[6];
    V90SDetect rate_detector;
    V90Cp preceding_cp;
    unsigned renegotiations,reneg_start;
} V90Phase4;
void v90_phase4_init(V90Phase4 *s,int alaw,int uinfo);
int16_t v90_phase4_next(V90Phase4 *s,int16_t input);
#endif
