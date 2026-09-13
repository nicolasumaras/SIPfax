#ifndef SIPFAX_V34CLOCK_H
#define SIPFAX_V34CLOCK_H

/* A per-equalizer-pass trailing rate estimate. Never mixes training passes. */
#define V34_CLOCK_HISTORY 2048u
typedef struct {
    double values[V34_CLOCK_HISTORY];
    unsigned next, used;
} V34ClockHistory;

static inline void v34_clock_reset(V34ClockHistory *h)
{
    h->next = h->used = 0;
}
static inline void v34_clock_push(V34ClockHistory *h, double rate)
{
    h->values[h->next] = rate;
    h->next = (h->next + 1) % V34_CLOCK_HISTORY;
    if (h->used < V34_CLOCK_HISTORY) ++h->used;
}
static inline double v34_clock_mean(const V34ClockHistory *h, unsigned count,
                                    double fallback)
{
    if (!count || count > h->used || count > V34_CLOCK_HISTORY) return fallback;
    double sum = 0;
    for (unsigned i = 0; i < count; ++i)
        sum += h->values[(h->next + V34_CLOCK_HISTORY - 1 - i) % V34_CLOCK_HISTORY];
    return sum / count;
}
#endif
