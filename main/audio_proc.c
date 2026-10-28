#include "audio_proc.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

void audio_proc_init(audio_proc_t *p, uint32_t in_rate, uint8_t channels,
                     uint32_t target_out_rate, float cutoff_hz, uint32_t gain_x100)
{
    memset(p, 0, sizeof(*p));
    p->channels = channels ? channels : 1;
    p->gain_x100 = gain_x100;

    uint32_t decim = 1;
    if (target_out_rate > 0 && in_rate > target_out_rate) {
        decim = (in_rate + target_out_rate / 2) / target_out_rate;
        if (decim < 1) decim = 1;
    }
    p->decim = decim;
    p->out_rate = in_rate / decim;

    float fc = cutoff_hz / (float)in_rate;                 // cycles per sample
    if (fc > 0.49f) fc = 0.49f;
    const int mid = (AUDIO_FIR_TAPS - 1) / 2;
    float sum = 0.0f;
    for (int n = 0; n < AUDIO_FIR_TAPS; n++) {
        const int m = n - mid;
        const float sinc = (m == 0) ? 2.0f * fc
                                    : sinf(2.0f * PI_F * fc * (float)m) / (PI_F * (float)m);
        const float w = 0.42f - 0.5f * cosf(2.0f * PI_F * (float)n / (AUDIO_FIR_TAPS - 1))
                        + 0.08f * cosf(4.0f * PI_F * (float)n / (AUDIO_FIR_TAPS - 1));  // Blackman
        p->h[n] = sinc * w;
        sum += p->h[n];
    }
    for (int n = 0; n < AUDIO_FIR_TAPS; n++) p->h[n] /= sum;   // unity gain at DC
}

size_t audio_proc_run(audio_proc_t *p, const int16_t *in, size_t frames, int16_t *out)
{
    const float gain = (float)p->gain_x100 / 100.0f;
    const uint32_t ch = p->channels;
    size_t n_out = 0;

    for (size_t i = 0; i < frames; i++) {
        float mono = 0.0f;
        for (uint32_t c = 0; c < ch; c++) mono += (float)in[i * ch + c];
        mono /= (float)ch;

        // DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]
        const float dc = mono - p->dc_x1 + 0.998f * p->dc_y1;
        p->dc_x1 = mono;
        p->dc_y1 = dc;

        p->x[p->pos] = dc;
        if (++p->pos == AUDIO_FIR_TAPS) p->pos = 0;
        if (++p->phase < p->decim) continue;
        p->phase = 0;

        float acc = 0.0f;
        uint32_t idx = p->pos;                              // oldest sample
        for (int k = 0; k < AUDIO_FIR_TAPS; k++) {
            acc += p->h[k] * p->x[idx];
            if (++idx == AUDIO_FIR_TAPS) idx = 0;
        }
        acc *= gain;
        if (acc > 32767.0f) acc = 32767.0f;
        if (acc < -32768.0f) acc = -32768.0f;
        out[n_out++] = (int16_t)lrintf(acc);
    }
    return n_out;
}
