#ifndef V90TRAIN_TX_H
#define V90TRAIN_TX_H
#include <stdint.h>
#include "v90dil.h"
typedef struct {
    unsigned sample,scrambler,sign;
    int alaw,uinfo;
    uint8_t jd[72];
    unsigned jd_end,dil_segment,dil_position,stage,stop_dil;
    V90Dil dil;
} V90TrainTx;
void v90_train_tx_init(V90TrainTx *s,int alaw,int uinfo);
void v90_train_tx_end_jd(V90TrainTx *s);
int16_t v90_train_tx_next(V90TrainTx *s);
#endif
