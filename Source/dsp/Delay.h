#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "Biquad.h"

namespace aio
{
struct DelaySettings
{
    float timeMs = 350.f;     // already resolved from tempo-sync by the caller
    float feedback = 0.35f;
    bool  pingPong = true;
    float toneHz = 8000.f;
    float mix = 0.25f;
};

// Stereo feedback delay with tone filter in the loop and optional ping-pong.
class StereoDelay
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        size_t sz = 1;
        while ((double) sz < sr * 4.1) sz <<= 1;
        buf[0].assign (sz, 0.f);
        buf[1].assign (sz, 0.f);
        mask = sz - 1;
        reset();
    }

    void reset()
    {
        std::fill (buf[0].begin(), buf[0].end(), 0.f);
        std::fill (buf[1].begin(), buf[1].end(), 0.f);
        w = 0; lp[0] = lp[1] = 0.f; dSmooth = -1.0;
    }

    void process (float* l, float* r, int n, const DelaySettings& s)
    {
        const double target = std::clamp ((double) s.timeMs * 0.001 * sr, 8.0, sr * 4.0);
        if (dSmooth < 0) dSmooth = target;
        const float fb = std::clamp (s.feedback, 0.f, 0.97f);
        const float a = 1.0f - std::exp (-(float) (2.0 * kPi * s.toneHz / sr));
        const float mix = s.mix;

        for (int i = 0; i < n; ++i)
        {
            dSmooth += (target - dSmooth) * 0.0006;
            const float dl = readAt (0, dSmooth), dr = readAt (1, dSmooth);

            lp[0] += a * (dl - lp[0]);
            lp[1] += a * (dr - lp[1]);

            float wl, wr;
            if (s.pingPong) { wl = 0.5f * (l[i] + r[i]) + fb * lp[1]; wr = fb * lp[0]; }
            else            { wl = l[i] + fb * lp[0];                  wr = r[i] + fb * lp[1]; }

            buf[0][w] = wl; buf[1][w] = wr;
            w = (w + 1) & mask;

            l[i] = l[i] * (1.0f - mix) + dl * mix;
            r[i] = r[i] * (1.0f - mix) + dr * mix;
        }
    }

private:
    inline float readAt (int c, double delay) const
    {
        const double rp = (double) w - delay;
        const double fl = std::floor (rp);
        const float fr = (float) (rp - fl);
        const size_t i0 = ((size_t) (long long) fl) & mask, i1 = (i0 + 1) & mask;
        return buf[c][i0] * (1.0f - fr) + buf[c][i1] * fr;
    }

    double sr = 44100.0, dSmooth = -1.0;
    std::vector<float> buf[2];
    size_t mask = 0, w = 0;
    float lp[2] { 0.f, 0.f };
};
} // namespace aio
