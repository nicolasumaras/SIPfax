#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "v34rxlevel.h"
int main(void)
{
    int previous = INT16_MIN;
    for (int sample = INT16_MIN; sample <= INT16_MAX; sample++) {
        int value = v34_rx_level((int16_t)sample);
        assert(value >= previous);
        assert(sample <= 0 ? value <= 0 : value > 0);
        if (sample >= -6553 && sample <= 6553) assert(value == sample * 5);
        if (sample > 6553) assert(value == INT16_MAX);
        if (sample < -6553) assert(value == INT16_MIN);
        previous = value;
    }
    assert(v34_rx_level(0) == 0);
    puts("PASS: all signed 16-bit levels preserve sign, linear gain and saturating rails");
    return 0;
}
