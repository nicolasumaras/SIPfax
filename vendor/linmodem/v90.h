#ifndef V90_H
#define V90_H

#include "v90priv.h"

typedef struct {
    int dummy;
} V90_params;

void V90_test(void);
void V90_init(struct V90State *s, int calling);
int V90_process(struct V90State *s, s16 *output, s16 *input, int nb_samples);

#endif

