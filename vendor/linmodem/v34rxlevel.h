#ifndef SIPFAX_V34_RX_LEVEL_H
#define SIPFAX_V34_RX_LEVEL_H
#include <stdint.h>

/* The legacy V.34 acquisition/equalizer uses five times the PCM input level.
 * Keep live input and the offline training decoder in the same units. */
static inline int16_t v34_rx_level(int16_t sample)
{
    int32_t value = (int32_t)sample * 5;
    if (value > INT16_MAX) value = INT16_MAX;
    if (value < INT16_MIN) value = INT16_MIN;
    return (int16_t)value;
}
#endif
