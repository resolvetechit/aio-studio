#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include "Biquad.h"

namespace aio
{
/*
   LAYERS: a layered sound-design instrument in the spirit of "layer engine" sample instruments.

   Nine layers sound together on every key.  Each layer is its own sound source (pad, drone, noise texture,
   impact, bell, airy choir, or a sample you load) with its own level / pan / pitch / envelope / filter /
   slow motion / drive / lo-fi.  Everything is synthesised here, nothing is sampled except what you load.
*/
constexpr int kLayers = 9;

enum class LayerSrc { Pad = 0, Drone, Texture, Impact, Bell, Air, Sample, Count };
inline const char* layerSrcName (int i)
{
    static const char* n[] = { "Pad", "Drone", "Texture", "Impact", "Bell", "Airy choir", "Sample" };
    return n[std::clamp (i, 0, 6)];
}

struct LayerCfg
{
    bool  on = false;
    int   src = 0;
    float levelDb = -8.f, pan = 0.f;           // pan -100..100
    float coarse = 0.f, fine = 0.f;            // semitones, cents
    bool  track = true;                        // follow the played key
    float attackMs = 20.f, releaseMs = 600.f;
    float cutoffHz = 12000.f, reso = 10.f;     // reso 0..100
    float motion = 20.f;                       // slow movement depth 0..100
    float drive = 0.f, lofi = 0.f;             // 0..100
    bool  loop = true;                         // sample layers
};

struct LayersSettings
{
    bool  on = false;
    float levelDb = 0.f;
    float bright = 0.f;                        // -100..100 (moves every filter, +-2 octaves)
    float motion = 100.f;                      // % scale of every layer's motion
    float relScale = 100.f;                    // % scale of every layer's release
    float velocity = 60.f;                     // % velocity sensitivity
    LayerCfg layer[kLayers];
};

struct SampleData
{
    std::vector<float> l, r;
    double sr = 44100.0;
    std::string name;
    int rootNote = 60;
    size_t size() const { return l.size(); }
};

class LayerEngine
{
public:
    static constexpr int kVoices = 8;

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        reset();
    }

    void reset()
    {
        for (auto& v : voices) v = Voice();
        age = 0; bend = 0.f; mod = 0.f; pedal = false;
        nActive.store (0);
        for (auto& m : layerMeter) m.store (0.f);
    }

    void setSample (int layer, std::shared_ptr<const SampleData> s) { if (layer >= 0 && layer < kLayers) smp[(size_t) layer] = std::move (s); }
    bool hasSample (int layer) const { return layer >= 0 && layer < kLayers && smp[(size_t) layer] && smp[(size_t) layer]->size() > 8; }

    void noteOn (int note, int vel)
    {
        Voice* v = nullptr;
        for (auto& c : voices) if (c.active && c.note == note && c.gate) { v = &c; break; }      // retrigger the same key
        if (v == nullptr) for (auto& c : voices) if (! c.active) { v = &c; break; }
        if (v == nullptr)
        {
            uint64_t best = ~0ull;
            for (auto& c : voices) if (! c.gate && c.age < best) { best = c.age; v = &c; }
            if (v == nullptr) for (auto& c : voices) if (c.age < best) { best = c.age; v = &c; }
        }
        *v = Voice();
        v->active = true; v->gate = true; v->note = note; v->vel = std::clamp (vel, 1, 127) / 127.f; v->age = ++age;
        uint32_t seed = (uint32_t) (age * 2654435761u) | 1u;
        for (int i = 0; i < kLayers; ++i)
        {
            auto& s = v->lv[(size_t) i];
            s.rng[0] = seed ^ (uint32_t) (i * 7919 + 13); s.rng[1] = seed * 1664525u + (uint32_t) i * 104729u + 1u;
            if (s.rng[0] == 0) s.rng[0] = 1;
            if (s.rng[1] == 0) s.rng[1] = 1;
            s.lfoPh = (double) ((seed >> (i + 3)) & 1023) / 1024.0;
            s.stage = 1; s.env = 0.f;
            s.ph[0] = ((seed >> 5) & 255) / 256.0; s.ph[1] = ((seed >> 9) & 255) / 256.0; s.ph[2] = ((seed >> 13) & 255) / 256.0;
        }
    }

    void noteOff (int note)
    {
        for (auto& v : voices)
            if (v.active && v.gate && v.note == note)
            {
                if (pedal) v.held = true;
                else { v.gate = false; }
            }
    }

    void allNotesOff() { for (auto& v : voices) { v.gate = false; v.held = false; } pedal = false; }
    void setBend (float semis) { bend = semis; }
    void setMod (float m) { mod = std::clamp (m, 0.f, 1.f); }
    void setSustain (bool on)
    {
        pedal = on;
        if (! on) for (auto& v : voices) if (v.held) { v.held = false; v.gate = false; }
    }
    bool anyActive() const { for (auto& v : voices) if (v.active) return true; return false; }

    // adds the sound into l / r
    void render (float* l, float* r, int n, const LayersSettings& s)
    {
        int active = 0;
        std::array<float, kLayers> pk {};
        const float master = dbToGain (s.levelDb);
        for (auto& v : voices)
        {
            if (! v.active) continue;
            ++active;
            renderVoice (v, l, r, n, s, master, pk);
        }
        nActive.store (active);
        for (int i = 0; i < kLayers; ++i)
        {
            const float old = layerMeter[(size_t) i].load();
            layerMeter[(size_t) i].store (std::max (pk[(size_t) i], old * 0.9f));
        }
    }

    std::atomic<int> nActive { 0 };
    std::array<std::atomic<float>, kLayers> layerMeter;

    static float dbToGain (float db) { return std::pow (10.f, db * 0.05f); }

private:
    struct Svf
    {
        float ic1 = 0.f, ic2 = 0.f;
        float a1 = 0.f, a2 = 0.f, a3 = 0.f, k = 1.f;
        void set (double sr, double fc, double q)
        {
            fc = std::clamp (fc, 20.0, sr * 0.45);
            const double g = std::tan (kPi * fc / sr);
            k = (float) (1.0 / std::max (0.3, q));
            const double aa1 = 1.0 / (1.0 + g * (g + (double) k));
            a1 = (float) aa1; a2 = (float) (g * aa1); a3 = (float) (g * g * aa1);
        }
        inline void tick (float x, float& lp, float& bp)
        {
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.f * v1 - ic1; ic2 = 2.f * v2 - ic2;
            lp = v2; bp = v1;
        }
    };

    struct LayerState
    {
        double ph[3] { 0, 0, 0 };
        double thumpPh = 0.0, lfoPh = 0.0, t = 0.0, smpPos = 0.0;
        float  env = 0.f;
        int    stage = 0;                       // 0 idle, 1 attack, 2 sustain, 3 release
        uint32_t rng[2] { 1u, 1u };
        Svf    tx[2][2], fm[2][3], lpf[2], hit;
        float  holdL = 0.f, holdR = 0.f; int holdCount = 0;
        float  fL = 0.f, fR = 0.f;
        float  lastMeter = 0.f;
    };

    struct Voice
    {
        bool active = false, gate = false, held = false;
        int note = 60; float vel = 1.f; uint64_t age = 0;
        std::array<LayerState, kLayers> lv;
    };

    static inline float noise (uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (float) (int32_t) s * (1.0f / 2147483648.0f); }
    static inline float blep (double t, double dt)
    {
        if (t < dt)          { t /= dt;           return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt)    { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.f;
    }
    static inline float sawBL (double ph, double dt) { return (float) (2.0 * ph - 1.0) - blep (ph, dt); }

    void renderVoice (Voice& v, float* outL, float* outR, int n, const LayersSettings& S, float master, std::array<float, kLayers>& pk)
    {
        bool anyLive = false;
        const double bendSt = (double) bend;
        const float velGain = 1.f + (v.vel - 1.f) * std::clamp (S.velocity * 0.01f, 0.f, 1.f);
        const double brightMul = std::exp2 ((double) S.bright * 0.02 + (double) mod * 1.0);

        for (int li = 0; li < kLayers; ++li)
        {
            const LayerCfg& c = S.layer[(size_t) li];
            LayerState& st = v.lv[(size_t) li];
            if (! c.on || st.stage == 0) continue;
            const bool oneShot = c.src == (int) LayerSrc::Impact;

            // ---- layer constants
            const double st0 = (double) c.coarse + (double) c.fine * 0.01;
            const double baseFreq = c.track ? 440.0 * std::exp2 (((double) v.note - 69.0 + st0 + bendSt) / 12.0)
                                            : (oneShot ? 55.0 : 261.6256) * std::exp2 ((st0 + (oneShot ? 0.0 : bendSt)) / 12.0);
            const double f0 = std::clamp (baseFreq, 8.0, sr * 0.45);
            const float level = dbToGain (c.levelDb) * master * velGain;
            const float panA = (float) ((c.pan * 0.01 + 1.0) * 0.25 * kPi);
            const float gL = std::cos (panA) * 1.4142f, gR = std::sin (panA) * 1.4142f;
            const double attackN = std::max (1.0, (double) c.attackMs * 0.001 * sr);
            const double relMs = std::max (10.0, (double) c.releaseMs * (double) S.relScale * 0.01);
            const float relCoef = (float) std::exp (std::log (0.01) / (relMs * 0.001 * sr));
            const double lfoRate = 0.06 + 0.045 * (double) li;
            const double motion = (double) c.motion * 0.01 * (double) S.motion * 0.01;
            const double cutBase = (double) c.cutoffHz * brightMul;
            const float q = 0.6f + c.reso * 0.045f;
            const float driveG = 1.f + c.drive * 0.06f, driveN = 1.f / std::sqrt (driveG);
            const int hold = c.lofi > 0.5f ? 1 + (int) (c.lofi * 0.14f) : 1;
            const float qLevels = c.lofi > 0.5f ? std::exp2 (std::max (3.f, 15.f - c.lofi * 0.11f)) : 0.f;
            const auto& smpPtr = smp[(size_t) li];
            const bool smpOk = c.src == (int) LayerSrc::Sample && smpPtr && smpPtr->size() > 8;
            const double smpStep = smpOk ? (smpPtr->sr / sr) * std::exp2 (((c.track ? (double) (v.note - smpPtr->rootNote) + bendSt : 0.0) + st0) / 12.0) : 0.0;
            const double hitTau = std::max (0.05, (double) c.releaseMs * 0.001 * (double) S.relScale * 0.01 * 0.35);

            if (! v.gate && ! oneShot && st.stage != 3) st.stage = 3;

            float peak = 0.f;
            int pos = 0;
            while (pos < n)
            {
                const int m = std::min (16, n - pos);

                // ---- control rate: LFO, filter coefficients
                st.lfoPh += lfoRate * (double) m / sr; if (st.lfoPh >= 1.0) st.lfoPh -= 1.0;
                const double lfo = std::sin (2.0 * kPi * st.lfoPh);
                const double lfo2 = std::sin (2.0 * kPi * (st.lfoPh * 1.37 + 0.31));
                const double cut = std::clamp (cutBase * std::exp2 (lfo * motion * 1.6), 40.0, sr * 0.45);
                st.lpf[0].set (sr, cut, q); st.lpf[1].k = st.lpf[0].k; st.lpf[1].a1 = st.lpf[0].a1; st.lpf[1].a2 = st.lpf[0].a2; st.lpf[1].a3 = st.lpf[0].a3;
                double tex1 = 0, tex2 = 0, vow = 0;
                float texGain = 0.f;
                const double fDrift = std::exp2 (lfo * motion * 0.02);       // up to +-24 cents of drift for drones / pads
                if (c.src == (int) LayerSrc::Texture)
                {
                    const double wob = std::exp2 (lfo * motion * 0.9);
                    tex1 = f0 * wob; tex2 = f0 * 2.76 * std::exp2 (lfo2 * motion * 0.7);
                    const double tq = 9.0 + 14.0 * (double) c.reso * 0.01;
                    for (int ch = 0; ch < 2; ++ch) { st.tx[ch][0].set (sr, tex1, tq); st.tx[ch][1].set (sr, tex2, tq); }
                    texGain = (float) (0.207 * std::sqrt (sr * 0.5 / std::max (5.0, tex1 / tq)));
                }
                else if (c.src == (int) LayerSrc::Air)
                {
                    vow = std::clamp (0.5 + 0.5 * lfo * (0.4 + motion), 0.0, 1.0);           // ah -> oo
                    const double F1 = 800.0 + (325.0 - 800.0) * vow, F2 = 1150.0 + (700.0 - 1150.0) * vow, F3 = 2900.0 + (2700.0 - 2900.0) * vow;
                    for (int ch = 0; ch < 2; ++ch) { st.fm[ch][0].set (sr, F1, 7.0); st.fm[ch][1].set (sr, F2, 9.0); st.fm[ch][2].set (sr, F3, 11.0); }
                }
                else if (c.src == (int) LayerSrc::Impact)
                    st.hit.set (sr, std::clamp (cutBase * 0.8, 60.0, sr * 0.4), 0.8);

                const double fa = f0 * fDrift;
                const double dtA = fa / sr;
                const double detune = std::exp2 ((6.0 + 14.0 * motion) / 1200.0);      // about 6 .. 20 cents
                const double dtB = dtA * detune, dtC = dtA / detune;

                for (int i = 0; i < m; ++i)
                {
                    float xl = 0.f, xr = 0.f;
                    switch ((LayerSrc) c.src)
                    {
                        case LayerSrc::Pad:
                        {
                            for (int k = 0; k < 3; ++k) { st.ph[k] += (k == 0 ? dtA : (k == 1 ? dtB : dtC)); if (st.ph[k] >= 1.0) st.ph[k] -= 1.0; }
                            const float s0 = sawBL (st.ph[0], dtA), s1 = sawBL (st.ph[1], dtB), s2 = sawBL (st.ph[2], dtC);
                            xl = 0.22f * s0 + 0.26f * s1 + 0.07f * s2;
                            xr = 0.22f * s0 + 0.07f * s1 + 0.26f * s2;
                            break;
                        }
                        case LayerSrc::Drone:
                        {
                            st.ph[0] += dtA; if (st.ph[0] >= 1.0) st.ph[0] -= 1.0;
                            st.ph[1] += dtA * 0.5; if (st.ph[1] >= 1.0) st.ph[1] -= 1.0;
                            const double w = 2.0 * kPi * st.ph[0];
                            const float a2 = (float) (0.5 + 0.5 * lfo), a3 = (float) (0.5 + 0.5 * lfo2);
                            const float fund = (float) std::sin (w) * 0.30f + (float) std::sin (2.0 * kPi * st.ph[1]) * 0.22f;
                            xl = fund + (float) std::sin (2.0 * w + 0.4) * 0.16f * a2 + (float) std::sin (3.0 * w) * 0.10f * a3 + (float) std::sin (5.0 * w) * 0.04f * (1.f - a3);
                            xr = fund + (float) std::sin (2.0 * w) * 0.16f * (1.f - a2) + (float) std::sin (3.0 * w + 0.7) * 0.10f * (1.f - a3) + (float) std::sin (5.0 * w) * 0.04f * a3;
                            break;
                        }
                        case LayerSrc::Texture:
                        {
                            float lp, bp, lp2, bp2;
                            st.tx[0][0].tick (noise (st.rng[0]), lp, bp);  st.tx[0][1].tick (noise (st.rng[0]), lp2, bp2);
                            xl = (bp + 0.6f * bp2) * texGain * st.tx[0][0].k;
                            st.tx[1][0].tick (noise (st.rng[1]), lp, bp);  st.tx[1][1].tick (noise (st.rng[1]), lp2, bp2);
                            xr = (bp + 0.6f * bp2) * texGain * st.tx[1][0].k;
                            break;
                        }
                        case LayerSrc::Impact:
                        {
                            const double tt = st.t;
                            const double fe = f0 * (1.0 + 3.2 * std::exp (-tt / 0.045));
                            st.thumpPh += fe / sr; if (st.thumpPh >= 1.0) st.thumpPh -= 1.0;
                            const float thump = (float) std::sin (2.0 * kPi * st.thumpPh) * (float) std::exp (-tt / hitTau);
                            float lp, bp;
                            st.hit.tick (noise (st.rng[0]), lp, bp);
                            const float burst = lp * (float) (std::exp (-tt / (hitTau * 0.22)) * 0.9 + std::exp (-tt / (hitTau * 0.8)) * 0.18);
                            xl = xr = 0.75f * thump + 0.45f * burst;
                            break;
                        }
                        case LayerSrc::Bell:
                        {
                            const double tt = st.t;
                            st.ph[0] += dtA; if (st.ph[0] >= 1.0) st.ph[0] -= 1.0;
                            st.ph[1] += dtA * 3.5; if (st.ph[1] >= 1.0) st.ph[1] -= 1.0;
                            st.ph[2] += dtA * 2.0 * 1.0007; if (st.ph[2] >= 1.0) st.ph[2] -= 1.0;
                            const double idx = (1.2 + 2.6 * (double) v.vel) * std::exp (-tt / 0.5);
                            const double dec = std::exp (-tt / (0.25 + c.releaseMs * 0.0006));
                            const float b1 = (float) std::sin (2.0 * kPi * st.ph[0] + idx * std::sin (2.0 * kPi * st.ph[1]));
                            const float b2 = (float) std::sin (2.0 * kPi * st.ph[2]) * 0.35f;
                            xl = xr = (b1 + b2) * 0.30f * (float) dec;
                            xl *= 1.f + 0.1f * (float) lfo; xr *= 1.f - 0.1f * (float) lfo;
                            break;
                        }
                        case LayerSrc::Air:
                        {
                            st.ph[0] += dtA * 1.0015; if (st.ph[0] >= 1.0) st.ph[0] -= 1.0;
                            st.ph[1] += dtA * 0.9985; if (st.ph[1] >= 1.0) st.ph[1] -= 1.0;
                            const float exL = sawBL (st.ph[0], dtA) * 0.9f + noise (st.rng[0]) * 0.12f;
                            const float exR = sawBL (st.ph[1], dtA) * 0.9f + noise (st.rng[1]) * 0.12f;
                            float lp, b1, b2, b3;
                            st.fm[0][0].tick (exL, lp, b1); st.fm[0][1].tick (exL, lp, b2); st.fm[0][2].tick (exL, lp, b3);
                            xl = (b1 * st.fm[0][0].k * 0.55f + b2 * st.fm[0][1].k * 0.32f + b3 * st.fm[0][2].k * 0.16f) * 4.2f;
                            st.fm[1][0].tick (exR, lp, b1); st.fm[1][1].tick (exR, lp, b2); st.fm[1][2].tick (exR, lp, b3);
                            xr = (b1 * st.fm[1][0].k * 0.55f + b2 * st.fm[1][1].k * 0.32f + b3 * st.fm[1][2].k * 0.16f) * 4.2f;
                            break;
                        }
                        case LayerSrc::Sample:
                        {
                            if (smpOk)
                            {
                                const size_t len = smpPtr->size();
                                size_t p0 = (size_t) st.smpPos;
                                if (p0 + 1 >= len)
                                {
                                    if (c.loop) { st.smpPos = std::fmod (st.smpPos, (double) (len - 1)); p0 = (size_t) st.smpPos; }
                                    else { st.stage = 3; st.env = 0.f; break; }
                                }
                                const float fr = (float) (st.smpPos - (double) p0);
                                const size_t p1 = p0 + 1 < len ? p0 + 1 : 0;
                                xl = smpPtr->l[p0] + (smpPtr->l[p1] - smpPtr->l[p0]) * fr;
                                xr = smpPtr->r.empty() ? xl : smpPtr->r[p0] + (smpPtr->r[p1] - smpPtr->r[p0]) * fr;
                                st.smpPos += smpStep;
                            }
                            break;
                        }
                        case LayerSrc::Count: break;
                    }

                    // ---- filter
                    float lpL, lpR, bp;
                    st.lpf[0].tick (xl, lpL, bp);
                    st.lpf[1].tick (xr, lpR, bp);
                    xl = lpL; xr = lpR;

                    // ---- drive / lo-fi
                    if (c.drive > 0.5f) { xl = std::tanh (xl * driveG) * driveN; xr = std::tanh (xr * driveG) * driveN; }
                    if (hold > 1 || qLevels > 0.f)
                    {
                        if (st.holdCount <= 0)
                        {
                            st.holdCount = hold;
                            st.holdL = xl; st.holdR = xr;
                            if (qLevels > 0.f) { st.holdL = std::round (st.holdL * qLevels) / qLevels; st.holdR = std::round (st.holdR * qLevels) / qLevels; }
                        }
                        --st.holdCount; xl = st.holdL; xr = st.holdR;
                    }

                    // ---- envelope
                    float e;
                    if (oneShot) { e = 1.f; }
                    else
                    {
                        if (st.stage == 1) { st.env += (float) (1.0 / attackN); if (st.env >= 1.f) { st.env = 1.f; st.stage = 2; } }
                        else if (st.stage == 3) { st.env *= relCoef; if (st.env < 1.0e-4f) { st.env = 0.f; st.stage = 0; } }
                        e = st.env;
                        if (st.stage == 1) e = e * e * (3.f - 2.f * e);
                    }
                    st.t += 1.0 / sr;

                    const float oL = xl * e * level * gL, oR = xr * e * level * gR;
                    outL[pos + i] += oL; outR[pos + i] += oR;
                    peak = std::max (peak, std::max (std::abs (oL), std::abs (oR)));
                    if (st.stage == 0) break;
                }
                pos += m;
                if (st.stage == 0) break;
                if (oneShot && st.t > hitTau * 12.0) { st.stage = 0; break; }
            }
            if (oneShot && st.t > hitTau * 12.0) st.stage = 0;
            pk[(size_t) li] = std::max (pk[(size_t) li], peak);
            if (st.stage != 0) anyLive = true;
        }

        // layers that are switched off never keep a voice alive
        if (! anyLive) v.active = false;
    }

    double sr = 44100.0;
    std::array<Voice, kVoices> voices;
    std::array<std::shared_ptr<const SampleData>, kLayers> smp;
    uint64_t age = 0;
    float bend = 0.f, mod = 0.f;
    bool pedal = false;
};
} // namespace aio
