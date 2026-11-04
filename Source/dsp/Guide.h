#pragma once
#include <cmath>
#include <algorithm>
#include "Biquad.h"

namespace aio
{
// Small monophonic "ooh" voice used as the guide tone in the Sing tab: sine + soft 2nd/3rd harmonics, gentle glide and envelope.
class GuideVoice
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        aCoef = 1.0 - std::exp (-1.0 / (sr * 0.008));
        rCoef = 1.0 - std::exp (-1.0 / (sr * 0.045));
        gCoef = 1.0 - std::exp (-1.0 / (sr * 0.012));
        reset();
    }
    void reset() { phase = 0.0; env = 0.0; freq = 220.0; target = -1; }

    // midi < 0 releases; the note is folded into a comfortable range so it is always audible and singable
    void setNote (int midi)
    {
        if (midi < 0) { target = -1; return; }
        while (midi > 76) midi -= 12;
        while (midi < 50) midi += 12;
        target = midi;
    }

    void render (float* l, float* r, int n, float level)
    {
        const double fT = target >= 0 ? 440.0 * std::pow (2.0, (target - 69) / 12.0) : freq;
        for (int i = 0; i < n; ++i)
        {
            freq += gCoef * (fT - freq);
            env += (target >= 0 ? aCoef : rCoef) * ((target >= 0 ? 1.0 : 0.0) - env);
            phase += freq / sr;
            if (phase >= 1.0) phase -= 1.0;
            const double p = 2.0 * kPi * phase;
            const float v = (float) ((std::sin (p) + 0.22 * std::sin (2.0 * p) + 0.08 * std::sin (3.0 * p)) * env * level * 0.5);
            l[i] += v; r[i] += v;
        }
    }

private:
    double sr = 44100.0, aCoef = 0.01, rCoef = 0.01, gCoef = 0.01;
    double phase = 0, env = 0, freq = 220.0;
    int target = -1;
};
} // namespace aio
