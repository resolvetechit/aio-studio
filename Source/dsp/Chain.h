#pragma once
#include <juce_dsp/juce_dsp.h>
#include "EQ.h"
#include "Compressor.h"
#include "Saturator.h"
#include "Delay.h"
#include "Reverb.h"
#include "PitchCorrector.h"
#include "AutoVoice.h"
#include "Limiter.h"
#include "Loudness.h"

namespace aio
{
struct ChainParams
{
    float inGain = 1.f, outGain = 1.f, width = 1.f, monoBassHz = 0.f;
    bool  limOn = true;  float limCeil = -0.3f;

    bool voxOn = false;   VoxSettings vox;
    bool tuneOn = false;  TunerSettings tune;
    bool eqOn = true;     std::array<EQBand, ParametricEQ::kBands> eq;
    bool compOn = false;  CompSettings comp;
    bool satOn = false;   SatSettings sat;
    bool choOn = false;   float choRate = 0.8f, choDepth = 0.4f, choMix = 0.35f;
    bool dlyOn = false;   DelaySettings dly;
    bool revOn = false;   ReverbSettings rev;

    bool  followOn = false;       // live loudness target (slow gain ride before the limiter)
    float followTargetLufs = -14.f;

    // what the file player / offline master export uses: tonal + dynamics + limiter only
    ChainParams masterOnly() const
    {
        ChainParams p = *this;
        p.inGain = 1.f; p.voxOn = false; p.tuneOn = false; p.tune.on = false; p.choOn = false; p.dlyOn = false; p.revOn = false; p.followOn = false;
        return p;
    }
};

/*
   The complete signal chain in one place so the plug-in (real time) and the offline master renderer share exactly the same DSP.
     stageInput : input gain -> Auto Voice
     (tuner is run by the caller so it can be driven in short sub-blocks)
     stagePost  : EQ -> compressor -> saturation -> chorus -> delay -> reverb -> width / mono-bass -> output gain
                  -> loudness follower -> look-ahead limiter
*/
class Chain
{
public:
    void prepare (double sampleRate, int maxBlock, bool withMeters = true)
    {
        sr = sampleRate;
        meters = withMeters;
        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };
        tuner.prepare (sampleRate);
        vox.prepare (sampleRate);
        eq.prepare (sampleRate);
        comp.prepare (sampleRate);
        sat.prepare (sampleRate, maxBlock);
        chorus.prepare (spec);
        delay.prepare (sampleRate);
        reverb.prepare (sampleRate, maxBlock);
        limiter.prepare (sampleRate);
        lufsIn.prepare (sampleRate, 3.0);
        lufsOut.prepare (sampleRate, 3.0);
        followCoef = 1.0 - std::exp (-1.0 / (sampleRate * 0.02));
        reset();
    }

    void reset()
    {
        vox.reset(); eq.reset(); comp.reset(); limiter.reset(); lufsIn.reset(); lufsOut.reset();
        sideHp.reset(); lastMonoBass = -1.f;
        followDb = 0.0; followLin = 1.0; limWasOn = false;
        followGainDb.store (0.f);
    }

    int latencySamples (const ChainParams& p, bool tunerActive) const
    {
        return (tunerActive ? tuner.getLatencySamples() : 0) + (p.satOn ? sat.getLatencySamples() : 0) + (p.limOn ? limiter.getLatency() : 0);
    }

    // returns the input peak
    float stageInput (float* l, float* r, int n, const ChainParams& p)
    {
        float pk = 0.f;
        for (int i = 0; i < n; ++i)
        {
            l[i] *= p.inGain; r[i] *= p.inGain;
            pk = std::max ({ pk, std::abs (l[i]), std::abs (r[i]) });
        }
        if (meters) { lufsIn.push (l, r, n); inLufs.store ((float) lufsIn.lufs()); }
        if (p.voxOn) vox.process (l, r, n, p.vox);
        return pk;
    }

    // returns the output peak
    float stagePost (float* l, float* r, int n, const ChainParams& p)
    {
        eq.setBands (p.eq);
        eq.process (l, r, n);

        if (p.compOn) comp.process (l, r, n, p.comp);
        else          comp.grMeter.store (0.f);

        if (p.satOn) sat.process (l, r, n, p.sat);

        if (p.choOn)
        {
            chorus.setRate (p.choRate);
            chorus.setDepth (p.choDepth);
            chorus.setCentreDelay (7.0f);
            chorus.setFeedback (0.0f);
            chorus.setMix (p.choMix);
            float* ch[2] = { l, r };
            juce::dsp::AudioBlock<float> blk (ch, 2, (size_t) n);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            chorus.process (ctx);
        }

        if (p.dlyOn) delay.process (l, r, n, p.dly);
        if (p.revOn) reverb.process (l, r, n, p.rev);

        // ---- width + mono-bass (side channel high-passed)
        const bool mb = p.monoBassHz > 1.f;
        if (std::abs (p.width - 1.f) > 0.001f || mb)
        {
            if (mb && p.monoBassHz != lastMonoBass)
            {
                sideHp.c = Coeffs::make (FilterType::HighPass, sr, p.monoBassHz, 0.0, 0.707);
                lastMonoBass = p.monoBassHz;
            }
            if (! mb) lastMonoBass = -1.f;
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (l[i] + r[i]);
                float s = 0.5f * (l[i] - r[i]) * p.width;
                if (mb) s = sideHp.tick (s);
                l[i] = m + s; r[i] = m - s;
            }
        }

        const float og = p.outGain;
        for (int i = 0; i < n; ++i) { l[i] *= og; r[i] *= og; }

        // ---- live loudness follower (slow integral control on the measured output loudness)
        if (p.followOn)
        {
            const double out = lufsOut.lufs();
            if (out > -50.0)
            {
                const double err = std::clamp ((double) p.followTargetLufs - out, -6.0, 6.0);
                const bool limiterWorkingHard = limiter.grMeter.load() < -9.f;
                if (! (err > 0.0 && limiterWorkingHard))
                    followDb = std::clamp (followDb + 0.25 * err * ((double) n / sr), -12.0, 18.0);
            }
            const double target = std::pow (10.0, followDb / 20.0);
            for (int i = 0; i < n; ++i)
            {
                followLin += followCoef * (target - followLin);
                l[i] *= (float) followLin; r[i] *= (float) followLin;
            }
            followGainDb.store ((float) followDb);
        }
        else
        {
            followDb = 0.0; followLin = 1.0;
            followGainDb.store (0.f);
        }

        if (p.limOn)
        {
            if (! limWasOn) limiter.reset();
            limiter.process (l, r, n, p.limCeil, 80.f);
        }
        limWasOn = p.limOn;

        float op = 0.f;
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (l[i])) l[i] = 0.f;
            if (! std::isfinite (r[i])) r[i] = 0.f;
            op = std::max ({ op, std::abs (l[i]), std::abs (r[i]) });
        }
        if (meters) { lufsOut.push (l, r, n); outLufs.store ((float) lufsOut.lufs()); }
        return op;
    }

    // file A/B: original, loudness-matched, through the limiter only
    float processBypass (float* l, float* r, int n, const ChainParams& p, float gain)
    {
        for (int i = 0; i < n; ++i) { l[i] *= gain; r[i] *= gain; }
        if (p.limOn)
        {
            if (! limWasOn) limiter.reset();
            limiter.process (l, r, n, p.limCeil, 80.f);
        }
        limWasOn = p.limOn;
        float op = 0.f;
        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (l[i])) l[i] = 0.f;
            if (! std::isfinite (r[i])) r[i] = 0.f;
            op = std::max ({ op, std::abs (l[i]), std::abs (r[i]) });
        }
        if (meters) { lufsOut.push (l, r, n); outLufs.store ((float) lufsOut.lufs()); }
        return op;
    }

    // everything in order (used by the offline renderer; the plug-in calls the stages itself)
    void processOffline (float* l, float* r, int n, const ChainParams& p)
    {
        stageInput (l, r, n, p);
        if (p.tuneOn) tuner.process (l, r, n, p.tune);
        stagePost (l, r, n, p);
    }

    PitchCorrector    tuner;
    AutoVoice         vox;
    ParametricEQ      eq;
    Compressor        comp;
    Saturator         sat;
    juce::dsp::Chorus<float> chorus;
    StereoDelay       delay;
    ReverbFx          reverb;
    LookaheadLimiter  limiter;

    std::atomic<float> inLufs { -200.f }, outLufs { -200.f }, followGainDb { 0.f };

private:
    double sr = 44100.0;
    bool meters = true, limWasOn = false;
    LoudnessFollower lufsIn, lufsOut;
    Biquad sideHp;
    float lastMonoBass = -1.f;
    double followDb = 0.0, followLin = 1.0, followCoef = 0.001;
};
} // namespace aio
