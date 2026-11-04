#pragma once
#include "Biquad.h"
#include <atomic>

namespace aio
{
struct CompSettings
{
    float threshDb = -18.f, ratio = 3.f, attackMs = 10.f, releaseMs = 120.f, kneeDb = 6.f, makeupDb = 0.f, mix = 1.f;
};

// Stereo-linked feed-forward compressor, soft knee, log-domain smoothing, parallel mix.
class Compressor
{
public:
    void prepare (double sampleRate) { sr = sampleRate; reset(); }
    void reset() { grState = 0.0; grMeter.store (0.f); }

    void process (float* l, float* r, int n, const CompSettings& s)
    {
        const double aCoef = std::exp (-1.0 / (sr * std::max (0.00005, (double) s.attackMs * 0.001)));
        const double rCoef = std::exp (-1.0 / (sr * std::max (0.001,   (double) s.releaseMs * 0.001)));
        const double slope = 1.0 / std::max (1.0f, s.ratio) - 1.0;
        const double knee  = std::max (0.0f, s.kneeDb);
        const float makeup = dbToGain (s.makeupDb);
        const float mix = s.mix;
        double grMin = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r[i];
            const double a = std::max (std::abs ((double) inL), std::abs ((double) inR));
            const double lev = 20.0 * std::log10 (std::max (a, 1.0e-7));
            const double over = lev - s.threshDb;

            double target;   // desired gain change in dB (<= 0)
            if (2.0 * over < -knee)            target = 0.0;
            else if (2.0 * std::abs (over) <= knee) target = slope * (over + knee * 0.5) * (over + knee * 0.5) / (2.0 * std::max (knee, 1.0e-6));
            else                                target = slope * over;

            const double coef = (target < grState) ? aCoef : rCoef;
            grState = coef * grState + (1.0 - coef) * target;
            grMin = std::min (grMin, grState);

            const float g = (float) std::exp (grState * 0.115129254649702) * makeup;
            l[i] = inL * (1.0f - mix) + inL * g * mix;
            r[i] = inR * (1.0f - mix) + inR * g * mix;
        }
        grMeter.store ((float) grMin);
    }

    std::atomic<float> grMeter { 0.f };   // most negative gain reduction in the last block (dB)

private:
    double sr = 44100.0, grState = 0.0;
};
} // namespace aio
