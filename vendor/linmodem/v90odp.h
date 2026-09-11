/* Causal V.42-pattern equalizer targets; experimental, GPL-2.0. */
#ifndef V90ODP_H
#define V90ODP_H
#include "v42detect.h"
#include "v90mapping.h"
#define V90_ODP_PREDICTION_FRAMES 48
#if V90_ODP_PREDICTION_FRAMES < 16 || V90_ODP_PREDICTION_FRAMES > 64
#error Prediction horizon must cover trellis delay and fit the tagged ring
#endif
typedef struct {
 V42Detect detector;
 unsigned scrambler,have_anchor,gap_run;
 long long epoch,last_confirm;
 unsigned short labels[512];
 unsigned long long tags[512];
 unsigned updates,used;
} V90ODPTrainer;

/* Zero initialization resets predictions. Inputs come from a completed native
 * mapping frame. NULL bits denotes an erasure. Predictions expire by absolute
 * symbol index; they are never decoded user data. */
void v90_odp_observe(V90ODPTrainer*,const V90Mapping*,unsigned long long,
                    const uint16_t[8],const uint8_t*,unsigned);
int v90_odp_label(V90ODPTrainer*,unsigned long long,unsigned*);
#endif
