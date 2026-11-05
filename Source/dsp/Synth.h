#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "Biquad.h"

namespace aio
{
struct SynthParams
{
    // parts: 0 chords, 1 melody, 2 bass, 3 drums (level only), 4 synth / arp
    int   wave[5]  { 0, 1, 0, 0, 1 };      // 0 saw, 1 square, 2 triangle, 3 sine
    float level[5] { 0.6f, 0.6f, 0.7f, 0.7f, 0.5f };
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
        for (auto& d : drums) d.active = false;
        lp[0].reset(); lp[1].reset();
    }

    void setParams (const SynthParams& p)
    {
        params = p;
        for (int i = 0; i < 5; ++i)
        {
            auto& ep = envP[i];
            ep.attack = std::max (0.001f, p.attackMs * 0.001f);
            ep.decay = 0.25f; ep.sustain = 0.75f;
            ep.release = std::max (0.005f, p.releaseMs * 0.001f);
            if (i == 2) ep.attack = std::min (ep.attack, 0.008f);                      // bass stays tight
            if (i == 4) { ep.decay = 0.2f; ep.sustain = 0.55f; ep.release = std::max (0.005f, p.releaseMs * 0.0006f); }   // arps / stabs
        }
        const auto c = Coeffs::make (FilterType::LowPass, sr, p.cutoffHz, 0.0, 0.7071);
        lp[0].c = lp[1].c = c;
    }

    void noteOn (int part, int note, int vel)
    {
        if (part == 3) { drumHit (note, vel); return; }
        part = std::clamp (part, 0, 4);
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
        v->env.setParameters (envP[part]);
        v->env.reset();
        v->env.noteOn();
    }

    void noteOff (int part, int note)
    {
        if (part == 3) return;                                   // drums are one-shots
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
            const int p = std::clamp (v.part, 0, 4);
            const int wave = params.wave[p];
            const float lvl = params.level[p] * 0.22f * (0.35f + 0.65f * v.vel);
            const double f1 = v.freq * (p == 2 ? 1.0 : 1.0035);
            const double f2 = (p == 2) ? v.freq * 0.5 : v.freq * 0.9965;
            const double d1 = f1 / sr, d2 = f2 / sr;
            const float pan = (p == 0) ? ((v.note % 5) - 2) * 0.08f : (p == 4) ? ((v.note % 3) - 1) * 0.18f : 0.f;
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

        renderDrums (l, r, n);                                   // drums bypass the synth filter
    }

private:
    // ---- synthesised drum kit (no samples): one-shot voices summed into the centre ----------------
    enum DKind { DKick, DSnare, DClap, DHat, DOpen, DRim, DTom, DCrash };
    struct DVoice
    {
        bool active = false;
        int kind = DKick, note = 36;
        double t = 0, ph = 0, f0 = 100;
        float vel = 1.f, hp1 = 0.f, lpz = 0.f;
        uint32_t rng = 1u;
        float noise() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (float) (rng & 0xFFFFFF) / 8388608.f - 1.f; }
    };

    static int kindOf (int note)
    {
        switch (note)
        {
            case 35: case 36: return DKick;
            case 38: case 40: return DSnare;
            case 39: return DClap;
            case 42: case 44: return DHat;
            case 46: return DOpen;
            case 41: case 43: case 45: case 47: case 48: case 50: return DTom;
            case 49: case 51: case 52: case 55: case 57: return DCrash;
            default: return DRim;
        }
    }

    void drumHit (int note, int vel)
    {
        const int kind = kindOf (note);
        if (kind == DHat) for (auto& d : drums) if (d.active && d.kind == DOpen) d.active = false;   // choke
        DVoice* v = nullptr;
        for (auto& d : drums) if (! d.active) { v = &d; break; }
        if (v == nullptr) v = &drums[(size_t) (drumRr++ % (int) drums.size())];
        v->active = true; v->kind = kind; v->note = note; v->t = 0; v->ph = 0;
        v->vel = (float) vel / 127.f; v->hp1 = 0.f; v->lpz = 0.f;
        v->rng = 0x9E3779B9u * (uint32_t) (++counter) + (uint32_t) note;
        v->f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0);
    }

    void renderDrums (float* l, float* r, int n)
    {
        const double dt = 1.0 / sr;
        const float master = params.level[3] * 0.5f;
        if (master <= 0.f) { for (auto& d : drums) d.active = false; return; }
        for (auto& d : drums)
        {
            if (! d.active) continue;
            const float g = master * (0.3f + 0.7f * d.vel);
            for (int i = 0; i < n; ++i)
            {
                const double t = d.t;
                float o = 0.f;
                bool done = false;
                switch (d.kind)
                {
                    case DKick:
                    {
                        const double f = 46.0 + 125.0 * std::exp (-t / 0.028);
                        d.ph += f * dt;
                        o = (float) (std::sin (2.0 * kPi * d.ph) * std::exp (-t / 0.17)) * 1.1f
                          + d.noise() * (float) std::exp (-t / 0.0025) * 0.35f;
                        done = t > 0.5;
                        break;
                    }
                    case DSnare:
                    {
                        d.ph += (175.0 + 70.0 * std::exp (-t / 0.015)) * dt;
                        const float nz = d.noise();
                        const float hpn = nz - d.hp1; d.hp1 = nz;
                        o = (float) (std::sin (2.0 * kPi * d.ph) * std::exp (-t / 0.06)) * 0.55f
                          + hpn * (float) std::exp (-t / 0.09) * 0.75f;
                        done = t > 0.35;
                        break;
                    }
                    case DClap:
                    {
                        const float nz = d.noise();
                        const float hpn = nz - d.hp1; d.hp1 = nz;
                        d.lpz += 0.45f * (hpn - d.lpz);
                        double env = std::exp (-t / 0.085) * 0.55;
                        for (int k = 0; k < 3; ++k) { const double tk = t - 0.011 * k; if (tk >= 0) env += std::exp (-tk / 0.006) * 0.8; }
                        o = d.lpz * (float) env * 1.3f;
                        done = t > 0.3;
                        break;
                    }
                    case DHat: case DOpen:
                    {
                        const float nz = d.noise();
                        const float hpn = nz - d.hp1; d.hp1 = nz;
                        d.ph += 7300.0 * dt;
                        const float met = ((d.ph - std::floor (d.ph)) < 0.5 ? 1.f : -1.f) * 0.18f;
                        const double decay = d.kind == DHat ? 0.022 : 0.16;
                        o = (hpn * 0.7f + met) * (float) std::exp (-t / decay) * 0.6f;
                        done = t > (d.kind == DHat ? 0.1 : 0.7);
                        break;
                    }
                    case DRim:
                    {
                        d.ph += 520.0 * dt;
                        o = (float) (std::sin (2.0 * kPi * d.ph) * std::exp (-t / 0.012)) * 0.7f
                          + d.noise() * (float) std::exp (-t / 0.003) * 0.3f;
                        done = t > 0.08;
                        break;
                    }
                    case DTom:
                    {
                        d.ph += d.f0 * (1.0 + 0.8 * std::exp (-t / 0.05)) * dt;
                        o = (float) (std::sin (2.0 * kPi * d.ph) * std::exp (-t / 0.16)) * 0.9f;
                        done = t > 0.6;
                        break;
                    }
                    default: // crash
                    {
                        const float nz = d.noise();
                        const float hpn = nz - d.hp1; d.hp1 = nz;
                        d.ph += 5100.0 * dt;
                        const float met = ((d.ph - std::floor (d.ph)) < 0.5 ? 1.f : -1.f) * 0.12f;
                        o = (hpn * 0.6f + met) * (float) std::exp (-t / 0.6) * 0.45f;
                        done = t > 2.0;
                        break;
                    }
                }
                if (d.ph > 1e6) d.ph = std::fmod (d.ph, 1.0);
                d.t += dt;
                const float s = o * g;
                l[i] += s; r[i] += s;
                if (done) { d.active = false; break; }
            }
        }
    }

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
    std::array<DVoice, 20> drums;
    int drumRr = 0;
    juce::ADSR::Parameters envP[5];
    uint64_t counter = 0;
    Biquad lp[2];
};
} // namespace aio
