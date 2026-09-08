#ifndef V8CJ_H
#define V8CJ_H
#include <stdint.h>
typedef struct {
    unsigned clock, count, valid, bits;
    double re[2], im[2], energy;
} V8CjLane;
typedef struct {
    long samples, found_at;
    V8CjLane lanes[27];
} V8Cj;
void v8_cj_init(V8Cj *s);
int v8_cj_receive(V8Cj *s, const int16_t *input, unsigned length);
#endif
