#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "Biquad.h"

namespace aio
{
struct SynthParams
{
    int   wave[3]  { 0, 1, 0 };            // 0 saw, 1 square, 2 triangle, 3 sine   (chords, melody, bass)
    float level[3] { 0.6f, 0.6f, 0.7f };
    float cutoffHz = 8000.f, attackMs = 5.f, releaseMs = 250.f;
};

// Small polyphonic preview synth (2 detuned oscillators, ADSR, shared low-pass).
// Used so the Compose engine is audible without loading another instrument.
class MiniSynth
{
public:
    static constexpr int kVoices = 32;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        for (auto& v : voices) { v.env.setSampleRate (sr); v.active = false; }
        lp[0].reset(); lp[1].reset();
    }

    void setParams (const SynthParams& p)
    {
        params = p;
        juce::ADSR::Parameters ep;
        ep.attack = std::max (0.001f, p.attackMs * 0.001f);
        ep.decay = 0.25f; ep.sustain = 0.75f;
        ep.release = std::max (0.005f, p.releaseMs * 0.001f);
        for (auto& v : voices) v.env.setParameters (ep);
        const auto c = Coeffs::make (FilterType::LowPass, sr, p.cutoffHz, 0.0, 0.7071);
        lp[0].c = lp[1].c = c;
    }

    void noteOn (int part, int note, int vel)
    {
        Voice* v = nullptr;
        for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr)
        {
            // steal: oldest releasing voice, else oldest overall
            uint64_t best = ~0ull;
            for (auto& c : voices) if (c.releasing && c.age < best) { best = c.age; v = &c; }
            if (v == nullptr) for (auto& c : voices) if (c.age < best) { best = c.age; v = &c; }
        }
        v->active = true; v->releasing = false; v->part = part; v->note = note;
        v->vel = (float) vel / 127.f;
        v->freq = 440.0 * std::pow (2.0, (note - 69) / 12.0);
        v->ph[0] = v->ph[1] = 0.0;
        v->age = ++counter;
        v->env.reset();
        v->env.noteOn();
    }

    void noteOff (int part, int note)
    {
        for (auto& v : voices)
            if (v.active && ! v.releasing && v.part == part && v.note == note)
            {
                v.releasing = true;
                v.env.noteOff();
            }
    }

    void allNotesOff()
    {
        for (auto& v : voices) if (v.active && ! v.releasing) { v.releasing = true; v.env.noteOff(); }
    }

    // overwrites l/r with the synth output
    void render (float* l, float* r, int n)
    {
        for (int i = 0; i < n; ++i) { l[i] = 0.f; r[i] = 0.f; }

        for (auto& v : voices)
        {
            if (! v.active) continue;
            const int p = std::min (v.part, 2);
            const int wave = params.wave[p];
            const float lvl = params.level[p] * 0.22f * (0.35f + 0.65f * v.vel);
            const double f1 = v.freq * (p == 2 ? 1.0 : 1.0035);
            const double f2 = (p == 2) ? v.freq * 0.5 : v.freq * 0.9965;
            const double d1 = f1 / sr, d2 = f2 / sr;
            const float pan = (p == 0) ? ((v.note % 5) - 2) * 0.08f : 0.f;
            const float gl = 1.0f - std::max (0.f, pan), gr = 1.0f + std::min (0.f, pan);

            for (int i = 0; i < n; ++i)
            {
                const float e = v.env.getNextSample();
                if (! v.env.isActive()) { v.active = false; break; }

                v.ph[0] += d1; if (v.ph[0] >= 1.0) v.ph[0] -= 1.0;
                v.ph[1] += d2; if (v.ph[1] >= 1.0) v.ph[1] -= 1.0;
                const float o1 = osc (wave, v.ph[0], d1);
                const float o2 = (p == 2) ? 0.6f * osc (3, v.ph[1], d2) : osc (wave, v.ph[1], d2);
                const float s = (o1 + o2) * 0.5f * e * lvl;
                l[i] += s * gl;
                r[i] += s * gr;
            }
        }

        for (int i = 0; i < n; ++i) { l[i] = lp[0].tick (l[i]); r[i] = lp[1].tick (r[i]); }
    }

private:
    struct Voice
    {
        bool active = false, releasing = false;
        int part = 0, note = 60;
        float vel = 1.f;
        double freq = 440.0, ph[2] { 0, 0 };
        uint64_t age = 0;
        juce::ADSR env;
    };

    static inline float blep (double t, double dt)
    {
        if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.f;
    }

    static inline float osc (int wave, double t, double dt)
    {
        switch (wave)
        {
            case 0:  return (float) (2.0 * t - 1.0) - blep (t, dt);
            case 1:
            {
                float v = t < 0.5 ? 1.f : -1.f;
                v += blep (t, dt);
                double t2 = t + 0.5; if (t2 >= 1.0) t2 -= 1.0;
                v -= blep (t2, dt);
                return v * 0.8f;
            }
            case 2:  return (float) (4.0 * std::abs (t - 0.5) - 1.0);
            default: return (float) std::sin (2.0 * kPi * t);
        }
    }

    double sr = 44100.0;
    SynthParams params;
    std::array<Voice, kVoices> voices;
    uint64_t counter = 0;
    Biquad lp[2];
};
} // namespace aio
