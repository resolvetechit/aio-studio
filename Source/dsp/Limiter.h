#pragma once
#include "Biquad.h"
#include <vector>
#include <atomic>
#include <cmath>
#include <algorithm>

namespace aio
{
/*
   Look-ahead brick-wall limiter (stereo-linked).

   need[t]  : gain required so that input sample t (and the half-sample point just before it, estimated with a cubic) stays <= ceiling
   m[t]     : sliding minimum of need over the last la+1 samples
   box[t]   : moving average of m over la+1 samples  ->  box[t] <= need[t-la]  for the sample being output, always
   g[t]     : min(box[t], release-limited recovery)  ->  smooth attack, adjustable release, ceiling is never exceeded

   The output is the input delayed by `la` samples (2 ms), which the processor reports as latency.
*/
class LookaheadLimiter
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        la = std::max (8, (int) std::lround (sr * 0.002));
        dl.assign ((size_t) la, 0.f); dr.assign ((size_t) la, 0.f);
        cap = la + 2;
        dqVal.assign ((size_t) cap, 1.f); dqId.assign ((size_t) cap, 0LL);
        mbuf.assign ((size_t) la + 1, 1.f);
        reset();
    }

    void reset()
    {
        std::fill (dl.begin(), dl.end(), 0.f); std::fill (dr.begin(), dr.end(), 0.f);
        std::fill (mbuf.begin(), mbuf.end(), 1.f);
        wpos = 0; front = 0; count = 0; idx = 0LL; mpos = 0;
        sum = (double) (la + 1);
        gain = 1.f;
        x1l = x2l = x3l = x1r = x2r = x3r = 0.f;
        grMeter.store (0.f);
    }

    int getLatency() const { return la; }

    void process (float* l, float* r, int n, float ceilingDb, float releaseMs = 80.f)
    {
        const float ceil = std::pow (10.0f, ceilingDb * 0.05f);
        const float relCoef = std::exp (-1.0f / (float) (sr * std::max (1.f, releaseMs) * 0.001));
        float grMin = 1.f;

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];

            // ---- required gain (sample peak + half-sample cubic estimate between the previous and this sample)
            float pk = std::max (std::abs (inL), std::abs (inR));
            {
                const float hl = (-x3l + 9.f * x2l + 9.f * x1l - inL) * (1.f / 16.f);   // point between x2 and x1 (i.e. just before x1)
                const float hr = (-x3r + 9.f * x2r + 9.f * x1r - inR) * (1.f / 16.f);
                pk = std::max (pk, std::max (std::abs (hl), std::abs (hr)));
            }
            x3l = x2l; x2l = x1l; x1l = inL; x3r = x2r; x2r = x1r; x1r = inR;
            const float need = pk > ceil ? ceil / pk : 1.f;

            // ---- sliding minimum (monotonic deque)
            while (count > 0 && dqVal[(size_t) ((front + count - 1) % cap)] >= need) --count;
            {
                const int pos = (front + count) % cap;
                dqVal[(size_t) pos] = need; dqId[(size_t) pos] = idx;
                ++count;
            }
            while (dqId[(size_t) front] <= idx - (long long) (la + 1)) { front = (front + 1) % cap; --count; }
            const float m = dqVal[(size_t) front];
            ++idx;

            // ---- moving average of the minimum (attack ramp)
            sum += (double) m - (double) mbuf[(size_t) mpos];
            mbuf[(size_t) mpos] = m;
            mpos = (mpos + 1) % (la + 1);
            const float box = (float) (sum / (double) (la + 1));

            // ---- release
            const float rising = gain + (1.f - gain) * (1.f - relCoef);
            gain = std::min (box, rising);

            // ---- delay line
            const float outL = dl[(size_t) wpos], outR = dr[(size_t) wpos];
            dl[(size_t) wpos] = inL; dr[(size_t) wpos] = inR;
            wpos = (wpos + 1) % la;

            l[i] = outL * gain; r[i] = outR * gain;
            grMin = std::min (grMin, gain);
        }
        grMeter.store (20.f * std::log10 (std::max (grMin, 1.0e-4f)));
    }

    std::atomic<float> grMeter { 0.f };   // most negative gain applied in the last block (dB)

private:
    double sr = 44100.0, sum = 0.0;
    int la = 88, cap = 90, wpos = 0, front = 0, count = 0, mpos = 0;
    long long idx = 0;
    float gain = 1.f;
    float x1l = 0, x2l = 0, x3l = 0, x1r = 0, x2r = 0, x3r = 0;
    std::vector<float> dl, dr, dqVal, mbuf;
    std::vector<long long> dqId;
};
} // namespace aio
