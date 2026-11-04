#pragma once
#include <juce_dsp/juce_dsp.h>
#include <vector>
#include "Biquad.h"

namespace aio
{
struct ReverbSettings
{
    float size = 0.6f, damp = 0.5f, width = 1.f, preMs = 15.f, lowCutHz = 120.f, highCutHz = 12000.f, mix = 0.25f;
};

// Reverb with pre-delay, wet low/high cut and a true dry/wet crossfade.
class ReverbFx
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        rev.setSampleRate (sr);
        wet.setSize (2, maxBlock, false, true, false);
        size_t sz = 1;
        while ((double) sz < sr * 0.3) sz <<= 1;
        for (auto& b : pre) b.assign (sz, 0.f);
        preMask = sz - 1;
        reset();
    }

    void reset()
    {
        rev.reset();
        for (auto& b : pre) std::fill (b.begin(), b.end(), 0.f);
        pw = 0;
        for (auto& f : hp) f.reset();
        for (auto& f : lp) f.reset();
    }

    void process (float* l, float* r, int n, const ReverbSettings& s)
    {
        float* ch[2] = { l, r };
        const size_t delay = (size_t) std::clamp ((double) s.preMs * 0.001 * sr, 0.0, (double) preMask - 2);

        for (int i = 0; i < n; ++i)
        {
            pre[0][pw] = l[i]; pre[1][pw] = r[i];
            const size_t rp = (pw - delay) & preMask;
            wet.setSample (0, i, pre[0][rp]);
            wet.setSample (1, i, pre[1][rp]);
            pw = (pw + 1) & preMask;
        }

        juce::Reverb::Parameters p;
        p.roomSize = s.size;
        p.damping = s.damp;
        p.width = s.width;
        p.wetLevel = 0.33f;
        p.dryLevel = 0.f;
        p.freezeMode = 0.f;
        rev.setParameters (p);

        float* wch[2] = { wet.getWritePointer (0), wet.getWritePointer (1) };
        rev.processStereo (wch[0], wch[1], n);

        const auto chp = Coeffs::make (FilterType::HighPass, sr, s.lowCutHz, 0.0, 0.7071);
        const auto clp = Coeffs::make (FilterType::LowPass, sr, s.highCutHz, 0.0, 0.7071);
        for (int c = 0; c < 2; ++c) { hp[c].c = chp; lp[c].c = clp; }

        const float mix = s.mix;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                const float w = lp[c].tick (hp[c].tick (wch[c][i]));
                ch[c][i] = ch[c][i] * (1.0f - mix) + w * mix;
            }
    }

private:
    double sr = 44100.0;
    juce::Reverb rev;
    juce::AudioBuffer<float> wet;
    std::vector<float> pre[2];
    size_t preMask = 0, pw = 0;
    Biquad hp[2], lp[2];
};
} // namespace aio
