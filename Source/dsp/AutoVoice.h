#pragma once
#include "Biquad.h"
#include "Compressor.h"
#include <array>
#include <atomic>
#include <cmath>
#include <algorithm>

namespace aio
{
enum VoxMode { VoxVocal = 0, VoxSpeech = 1, VoxInstrument = 2 };

struct VoxSettings
{
    bool  on = false;
    int   mode = VoxVocal;
    float level = 1.f;     // leveler (gain rider) amount 0..1
    float tone = 0.7f;     // adaptive EQ amount
    float deess = 0.6f;    // de-esser amount
    float comp = 0.7f;     // compressor amount
    float clean = 0.5f;    // expander / background reduction amount
    float targetDb = -20.f;// RMS level the leveler aims for (dBFS)
};

/*
   Auto Voice: a self-adjusting vocal / speech / instrument channel strip.

     input -> high-pass -> [level detection, noise-floor tracking, voice activity]
           -> leveler (slow gain rider, open-loop from the measured level; holds in pauses, never boosts noise)
           -> expander (pulls the gaps between phrases down; not a spectral denoiser)
           -> de-esser (split-band, threshold relative to the voiced level)
           -> adaptive tone EQ (8 bands: measures the long-term voiced spectrum, nudges it toward a target shape)
           -> compressor (settings follow the mode)

   All detection is open-loop from the input, so nothing here can run away. Zero latency.
*/
class AutoVoice
{
public:
    static constexpr int kBands = 8;
    static constexpr int kTick = 64;
    static constexpr std::array<float, kBands> kBandHz { 100.f, 200.f, 400.f, 800.f, 1600.f, 3200.f, 6400.f, 12800.f };

    struct ModeParams
    {
        float hpf, sibHz, compRatio, compAtk, compRel, compThrOff, rangeUp, rangeDn, maxEqGain, maxExpand;
        float deessMax;
        std::array<float, kBands> shape;   // target octave-band energy shape (dB, normalised at run time)
    };

    static const ModeParams& modeParams (int m)
    {
        static const ModeParams p[3] = {
            // vocal (singing)
            { 75.f, 6800.f, 3.5f, 6.f, 100.f, 3.f, 12.f, 12.f, 5.f, 12.f, 12.f, { -4.f, 0.f, 0.f, -1.f, -4.f, -6.f, -11.f, -18.f } },
            // podcast / speech
            { 85.f, 6200.f, 3.0f, 8.f, 120.f, 3.f, 15.f, 15.f, 6.f, 15.f, 12.f, { -4.f, 0.f, 1.f, 0.f, -4.f, -8.f, -13.f, -21.f } },
            // instrument (generic)
            { 30.f, 7500.f, 2.5f, 12.f, 160.f, 4.f, 9.f, 12.f, 3.f, 6.f, 8.f,  { 0.f, 0.f, -1.f, -2.f, -3.5f, -5.f, -8.f, -13.f } } };
        return p[std::clamp (m, 0, 2)];
    }

    // Pure function (unit-testable): corrective gains in dB from measured octave-band levels (dB, any offset).
    static void computeCorrection (const std::array<float, kBands>& measuredDb, int mode, float amount, std::array<float, kBands>& gainDb)
    {
        const auto& mp = modeParams (mode);
        auto refOf = [] (const std::array<float, kBands>& v) { return 0.25f * (v[2] + v[3] + v[4] + v[5]); };
        const float mref = refOf (measuredDb), tref = refOf (mp.shape);
        for (int k = 0; k < kBands; ++k)
        {
            const float err = (mp.shape[(size_t) k] - tref) - (measuredDb[(size_t) k] - mref);   // positive: band is too weak
            const float dead = std::max (0.f, std::abs (err) - 1.5f);
            const float g = std::copysign (dead, err) * 0.55f * amount;
            gainDb[(size_t) k] = std::clamp (g, -mp.maxEqGain, mp.maxEqGain);
        }
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        dt = (double) kTick / sr;
        kMs = 1.0 - std::exp (-1.0 / (sr * 0.100));
        kFast = 1.0 - std::exp (-1.0 / (sr * 0.015));
        kSib = 1.0 - std::exp (-1.0 / (sr * 0.002));
        kBandSlow = 1.0 - std::exp (-1.0 / (sr * 3.0));
        kDeAtk = 1.0 - std::exp (-1.0 / (sr * 0.001));
        kDeRel = 1.0 - std::exp (-1.0 / (sr * 0.040));
        kGainSm = 1.0 - std::exp (-1.0 / (sr * 0.004));
        comp.prepare (sr);
        for (int k = 0; k < kBands; ++k)
        {
            const double f = std::min ((double) kBandHz[(size_t) k], sr * 0.45);
            const auto c = Coeffs::make (FilterType::BandPass, sr, f, 0.0, 1.41);
            mb[(size_t) k].c = c;
        }
        curMode = -1;
        reset();
    }

    void reset()
    {
        for (auto& b : hp) b.reset();
        for (auto& b : bp) b.reset();
        for (auto& b : mb) b.reset();
        for (auto& ch : eqf) for (auto& b : ch) b.reset();
        ms = fast = sibP = 0.0;
        for (auto& v : bandPow) v = 0.0;
        voicedSeconds = 0.0;
        nf = -100.0; rmsMax = -100.0; voiced = false; unvoicedTicks = 0;
        gLevDb = 0.0; gExpDb = 0.0; gLin = 1.0; gLinTarget = 1.0;
        deLin = 1.0; deTarget = 1.0; refDb = -20.0;
        tick = 0; eqCounter = 0;
        eqGain.fill (0.f); eqGainTarget.fill (0.f);
        for (auto& cur : eqCur) cur = -999.f;
        comp.reset();
        levelerDb.store (0.f); deEssDb.store (0.f); noiseFloorDb.store (-100.f); inRmsDb.store (-100.f); voicedFlag.store (false);
        for (auto& v : bandShape) v.store (0.f);
        for (auto& v : bandApplied) v.store (0.f);
    }

    void process (float* l, float* r, int n, const VoxSettings& s)
    {
        if (s.mode != curMode) setMode (s.mode);
        const auto& mp = modeParams (curMode);
        const double tgtDb = s.targetDb;
        const bool doDeEss = s.deess > 0.01f;
        float minDe = 0.f;

        for (int i = 0; i < n; ++i)
        {
            const float xl = hp[0].tick (l[i]), xr = hp[1].tick (r[i]);
            const double m = 0.5 * ((double) xl + (double) xr), m2 = m * m;
            ms += kMs * (m2 - ms);
            fast += kFast * (m2 - fast);

            const float bl = bp[0].tick (xl), br = bp[1].tick (xr);
            const double bm = 0.5 * ((double) bl + (double) br);
            sibP += kSib * (bm * bm - sibP);

            for (int k = 0; k < kBands; ++k)
            {
                const double y = mb[(size_t) k].tick ((float) m);
                if (voiced) bandPow[(size_t) k] += kBandSlow * (y * y - bandPow[(size_t) k]);
            }
            if (voiced) voicedSeconds += 1.0 / sr;

            if (++tick >= kTick)
            {
                tick = 0;
                controlTick (s, mp, tgtDb, doDeEss);
            }

            gLin += kGainSm * (gLinTarget - gLin);
            deLin += (deTarget < deLin ? kDeAtk : kDeRel) * (deTarget - deLin);
            minDe = std::min (minDe, (float) (20.0 * std::log10 (std::max (deLin, 1.0e-4))));

            const float g = (float) gLin, de = (float) (1.0 - deLin);
            l[i] = (xl - de * bl) * g;
            r[i] = (xr - de * br) * g;

            if (++eqCounter >= 512) { eqCounter = 0; updateEq (s, mp); }
        }

        // adaptive tone EQ
        for (int k = 0; k < kBands; ++k)
        {
            if (std::abs (eqGain[(size_t) k]) < 0.05f) continue;
            for (int i = 0; i < n; ++i) { l[i] = eqf[0][(size_t) k].tick (l[i]); r[i] = eqf[1][(size_t) k].tick (r[i]); }
        }
        // inactive bands still need their state flushed when they become active again; reset cheaply if idle
        for (int k = 0; k < kBands; ++k)
            if (std::abs (eqGain[(size_t) k]) < 0.05f) { eqf[0][(size_t) k].reset(); eqf[1][(size_t) k].reset(); }

        // compressor
        if (s.comp > 0.01f)
        {
            CompSettings cs;
            cs.threshDb = (float) tgtDb + mp.compThrOff;
            cs.ratio = 1.f + (mp.compRatio - 1.f) * s.comp;
            cs.attackMs = mp.compAtk; cs.releaseMs = mp.compRel; cs.kneeDb = 8.f;
            cs.makeupDb = 1.5f * s.comp; cs.mix = 1.f;
            comp.process (l, r, n, cs);
        }
        else comp.grMeter.store (0.f);

        levelerDb.store ((float) (gLevDb + gExpDb));
        deEssDb.store (minDe);
        noiseFloorDb.store ((float) nf);
        inRmsDb.store ((float) (10.0 * std::log10 (ms + 1.0e-12)));
        voicedFlag.store (voiced);
    }

    // ---- meters / UI
    Compressor comp;
    std::atomic<float> levelerDb { 0.f }, deEssDb { 0.f }, noiseFloorDb { -100.f }, inRmsDb { -100.f };
    std::atomic<bool> voicedFlag { false };
    std::array<std::atomic<float>, kBands> bandShape {}, bandApplied {};
    double getVoicedSeconds() const { return voicedSeconds; }

private:
    void setMode (int m)
    {
        curMode = std::clamp (m, 0, 2);
        const auto& mp = modeParams (curMode);
        const auto h = Coeffs::make (FilterType::HighPass, sr, mp.hpf, 0.0, 0.707);
        hp[0].c = hp[1].c = h;
        const auto b = Coeffs::make (FilterType::BandPass, sr, std::min ((double) mp.sibHz, sr * 0.45), 0.0, 1.4);
        bp[0].c = bp[1].c = b;
    }

    void controlTick (const VoxSettings& s, const ModeParams& mp, double tgtDb, bool doDeEss)
    {
        const double rmsDb = 10.0 * std::log10 (ms + 1.0e-12);
        const double fastDb = 10.0 * std::log10 (fast + 1.0e-12);

        // ---- noise floor (minimum statistics: fast fall, slow rise) and peak-hold for sanity
        if (rmsDb > rmsMax) rmsMax = rmsDb; else rmsMax -= 0.3 * dt;
        if (rmsDb < nf) nf += 0.25 * (rmsDb - nf);
        else if (! voiced) nf += (dt / 1.0) * (rmsDb - nf);   // in a pause the floor learns the room within a couple of seconds
        else nf += 2.0 * dt;                                  // while the voice is on it can only creep up
        nf = std::clamp (nf, std::max (-100.0, rmsMax - 65.0), std::max (-100.0, rmsMax - 25.0));

        // ---- voice activity with hysteresis. A phrase can't be more than ~30 dB below the recent peak and must be above -75 dBFS,
        //      which stops the leveler from lifting the room noise after the voice stops.
        const bool plausible = rmsDb > rmsMax - 30.0 && rmsDb > -75.0;
        if (! voiced && plausible && rmsDb > nf + 10.0) voiced = true;
        else if (voiced && (rmsDb < nf + 6.0 || ! plausible)) voiced = false;
        unvoicedTicks = voiced ? 0 : unvoicedTicks + 1;

        // ---- leveler: open-loop required gain, limited range, asymmetric smoothing, hold in pauses
        if (voiced)
        {
            const double levelDb = std::max (rmsDb, fastDb - 3.0);   // quick to notice loud transients
            const double need = tgtDb - levelDb;
            const double gT = std::clamp (need, (double) -mp.rangeDn, (double) mp.rangeUp) * (double) s.level;
            double tc;
            if (gT < gLevDb) tc = (gLevDb - gT > 6.0) ? 0.015 : 0.080;   // reducing: attack
            else             tc = 0.450;                                   // raising: release
            gLevDb += (1.0 - std::exp (-dt / tc)) * (gT - gLevDb);
        }
        else if (gLevDb > 0.0)
            gLevDb += (1.0 - std::exp (-dt / 1.5)) * (0.0 - gLevDb);      // a boost is never held through a pause (it would lift the room noise)
        else if ((double) unvoicedTicks * dt > 3.0)
            gLevDb += (1.0 - std::exp (-dt / 4.0)) * (0.0 - gLevDb);      // a cut is held for 3 s, then drifts back to neutral

        // ---- expander for the gaps
        double redT = 0.0;
        if (! voiced && s.clean > 0.01f)
        {
            const double thr = nf + 9.0;
            if (fastDb < thr) redT = std::min ((double) mp.maxExpand, (thr - fastDb) * 0.8) * (double) s.clean;
        }
        const double gExpT = -redT;
        gExpDb += (1.0 - std::exp (-dt / (gExpT < gExpDb ? 0.060 : 0.006))) * (gExpT - gExpDb);

        const double gTot = gLevDb + gExpDb;
        gLinTarget = std::pow (10.0, gTot / 20.0);

        // ---- de-esser (threshold follows the voiced level)
        if (voiced) refDb += (dt / 0.7) * ((rmsDb + gTot) - refDb);
        if (doDeEss)
        {
            const double sibDb = 10.0 * std::log10 (sibP * gLinTarget * gLinTarget + 1.0e-12);
            const double thr = refDb - 17.0 + 6.0 * (1.0 - (double) s.deess);
            const double red = sibDb > thr ? std::min ((double) mp.deessMax, (sibDb - thr) * 0.75) : 0.0;
            deTarget = std::pow (10.0, -red / 20.0);
        }
        else deTarget = 1.0;
    }

    void updateEq (const VoxSettings& s, const ModeParams& mp)
    {
        // measured shape (dB) from the long-term voiced band powers
        std::array<float, kBands> meas {};
        bool any = voicedSeconds > 0.3;
        for (int k = 0; k < kBands; ++k)
            meas[(size_t) k] = (float) (10.0 * std::log10 (bandPow[(size_t) k] + 1.0e-14));
        const float ref = 0.25f * (meas[2] + meas[3] + meas[4] + meas[5]);
        for (int k = 0; k < kBands; ++k) bandShape[(size_t) k].store (any ? meas[(size_t) k] - ref : 0.f);

        std::array<float, kBands> corr {};
        if (any && s.tone > 0.01f)
        {
            computeCorrection (meas, curMode, s.tone, corr);
            const float conf = (float) std::min (1.0, voicedSeconds / 4.0);   // trust grows with the amount of voiced audio
            for (auto& c : corr) c *= conf;
        }
        const float a = (float) (1.0 - std::exp (-(512.0 / sr) / 2.0));   // 2 s smoothing of the applied gains
        for (int k = 0; k < kBands; ++k)
        {
            eqGainTarget[(size_t) k] = std::clamp (corr[(size_t) k], -mp.maxEqGain, mp.maxEqGain);
            eqGain[(size_t) k] += a * (eqGainTarget[(size_t) k] - eqGain[(size_t) k]);
            bandApplied[(size_t) k].store (eqGain[(size_t) k]);
            if (std::abs (eqGain[(size_t) k] - eqCur[(size_t) k]) > 0.02f)
            {
                eqCur[(size_t) k] = eqGain[(size_t) k];
                const FilterType t = k == 0 ? FilterType::LowShelf : (k == kBands - 1 ? FilterType::HighShelf : FilterType::Bell);
                const double f = k == 0 ? 150.0 : (k == kBands - 1 ? 9000.0 : (double) kBandHz[(size_t) k]);
                const auto c = Coeffs::make (t, sr, f, (double) eqGain[(size_t) k], k == 0 || k == kBands - 1 ? 0.71 : 1.0);
                eqf[0][(size_t) k].c = eqf[1][(size_t) k].c = c;
            }
        }
    }

    double sr = 44100.0, dt = 64.0 / 44100.0;
    double kMs = 0, kFast = 0, kSib = 0, kBandSlow = 0, kDeAtk = 0, kDeRel = 0, kGainSm = 0;
    int curMode = -1, tick = 0, eqCounter = 0;
    long long unvoicedTicks = 0;

    Biquad hp[2], bp[2];
    std::array<Biquad, kBands> mb;
    std::array<std::array<Biquad, kBands>, 2> eqf;
    std::array<float, kBands> eqGain {}, eqGainTarget {}, eqCur {};

    double ms = 0, fast = 0, sibP = 0;
    std::array<double, kBands> bandPow {};
    double voicedSeconds = 0;
    double nf = -100, rmsMax = -100;
    bool voiced = false;
    double gLevDb = 0, gExpDb = 0, gLin = 1, gLinTarget = 1, deLin = 1, deTarget = 1, refDb = -20;
};
} // namespace aio
