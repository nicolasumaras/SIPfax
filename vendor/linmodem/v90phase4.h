#ifndef V90PHASE4_H
#define V90PHASE4_H
#include "v90training.h"
#include "v90pcm.h"
typedef struct {
    unsigned samples,stage,rbar_end,trn_start,generated,mp_length,mp_ack,mp_announced;
    unsigned ed_frame,data_start;
    int alaw,uinfo,have_cpt,have_cp,have_ack,rx_e_logged;
    V90Training rx;
    V90Cp cpt,cp;
    V90Pcm encoder;
    uint8_t mp[132];
    int16_t frame[6];
} V90Phase4;
void v90_phase4_init(V90Phase4 *s,int alaw,int uinfo);
int16_t v90_phase4_next(V90Phase4 *s,int16_t input);
#endif
