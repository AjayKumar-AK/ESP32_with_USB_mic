#pragma once
// Mono mix-down + DC blocker + windowed-sinc FIR low-pass + decimation + gain.
// Same processing as the working mic firmware, packaged as a reusable block.
#include <stddef.h>
#include <stdint.h>

#define AUDIO_FIR_TAPS 63

typedef struct {
    float    h[AUDIO_FIR_TAPS];   // FIR coefficients
    float    x[AUDIO_FIR_TAPS];   // circular input history
    uint32_t pos;
    uint32_t phase;
    uint32_t decim;
    float    dc_x1, dc_y1;        // DC blocker state
    uint8_t  channels;
    uint32_t gain_x100;
    uint32_t out_rate;
} audio_proc_t;

void audio_proc_init(audio_proc_t *p, uint32_t in_rate, uint8_t channels,
                     uint32_t target_out_rate, float cutoff_hz, uint32_t gain_x100);

static inline uint32_t audio_proc_out_rate(const audio_proc_t *p) { return p->out_rate; }

// Upper bound on output samples for a given number of input frames.
static inline size_t audio_proc_max_out(const audio_proc_t *p, size_t in_frames)
{
    return in_frames / p->decim + 1;
}

// in: interleaved 16-bit frames. out: mono 16-bit. Returns output sample count.
size_t audio_proc_run(audio_proc_t *p, const int16_t *in, size_t in_frames, int16_t *out);
