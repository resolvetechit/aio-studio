#pragma once
#include <juce_dsp/juce_dsp.h>
#include "Biquad.h"

namespace aio
{
enum class SatType { Tape = 0, Tube, Soft, Hard, Fold };

struct SatSettings
{
    SatType type = SatType::Tape;
    float driveDb = 6.f, toneHz = 20000.f, mix = 1.f, outDb = 0.f;
};

// Waveshaping saturator, 2x oversampled, with tone filter, DC blocker and parallel mix.
class Saturator
{
public:
    void prepare (double sampleRate, int maxBlock)
    {
        sr = sampleRate;
        // linear-phase half-band FIR: the latency is a whole number of samples, so the dry signal of the parallel mix can be delayed to match
        os = std::make_unique<juce::dsp::Oversampling<float>> (2, 1,
                juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, false);
        os->initProcessing ((size_t) maxBlock);
        lat = std::max (0, (int) std::lround (os->getLatencyInSamples()));
        for (auto& e : ext) e.assign ((size_t) (maxBlock + lat), 0.f);
        reset();
    }

    int getLatencySamples() const { return lat; }
    float getExactLatency() const { return os ? os->getLatencyInSamples() : 0.f; }

    void reset()
    {
        if (os) os->reset();
        for (auto& e : ext) std::fill (e.begin(), e.end(), 0.f);
        for (auto& f : tone) f.reset();
        for (auto& d : dc) d = 0.0;
        for (auto& d : dcIn) d = 0.0;
    }

    void process (float* l, float* r, int n, const SatSettings& s)
    {
        // dry path = input delayed by the oversampler latency (history kept in ext[c][0..lat), the new block follows)
        {
            float* in[2] = { l, r };
            for (int c = 0; c < 2; ++c)
            {
                auto& e = ext[(size_t) c];
                std::copy (in[c], in[c] + n, e.begin() + lat);
            }
        }

        float* ch[2] = { l, r };
        juce::dsp::AudioBlock<float> block (ch, 2, (size_t) n);
        auto up = os->processSamplesUp (block);

        const float drive = std::pow (10.0f, s.driveDb * 0.05f);
        const size_t un = up.getNumSamples();
        for (size_t c = 0; c < 2; ++c)
        {
            float* d = up.getChannelPointer (c);
            for (size_t i = 0; i < un; ++i)
                d[i] = shape (d[i] * drive, s.type);
        }
        os->processSamplesDown (block);

        const auto lp = Coeffs::make (FilterType::LowPass, sr, s.toneHz, 0.0, 0.7071);
        const bool toneActive = s.toneHz < 19000.f;
        tone[0].c = tone[1].c = lp;
        const float out = std::pow (10.0f, s.outDb * 0.05f);
        const float mix = s.mix;
        const double dcR = 1.0 - (2.0 * kPi * 12.0 / sr);

        for (int c = 0; c < 2; ++c)
        {
            float* d = ch[c];
            for (int i = 0; i < n; ++i)
            {
                double x = d[i];
                // DC blocker (asymmetric shapers create offset)
                const double y = x - dcIn[c] + dcR * dc[c];
                dcIn[c] = x; dc[c] = y;
                float w = (float) y;
                if (toneActive) w = tone[c].tick (w);
                w *= out;
                d[i] = ext[(size_t) c][(size_t) i] * (1.0f - mix) + w * mix;
            }
            auto& e = ext[(size_t) c];
            std::copy (e.begin() + n, e.begin() + n + lat, e.begin());       // keep the last `lat` input samples
        }
    }

private:
    static inline float shape (float x, SatType t)
    {
        switch (t)
        {
            case SatType::Tape: return std::tanh (x);
            case SatType::Tube:
            {
                // asymmetric: positive half soft, negative half harder -> even harmonics
                const float p = std::tanh (x * 0.9f);
                const float n = x / (1.0f + std::abs (x * 1.4f));
                return x >= 0.f ? p : n * 1.1f;
            }
            case SatType::Soft: return x / (1.0f + std::abs (x));
            case SatType::Hard: return std::clamp (x, -1.0f, 1.0f);
            case SatType::Fold:
            {
                // triangle wave folder
                float v = x;
                v = std::fmod (v + 1.0f, 4.0f);
                if (v < 0) v += 4.0f;
                return std::abs (v - 2.0f) - 1.0f;
            }
        }
        return x;
    }

    double sr = 44100.0;
    std::unique_ptr<juce::dsp::Oversampling<float>> os;
    std::vector<float> ext[2];
    int lat = 0;
    Biquad tone[2];
    double dc[2] {}, dcIn[2] {};
};
} // namespace aio
