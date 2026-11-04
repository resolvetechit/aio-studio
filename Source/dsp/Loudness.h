#pragma once
#include "Biquad.h"
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <limits>

namespace aio
{
// ---------------------------------------------------------------------------------------------------------------
// ITU-R BS.1770 K-weighting (two biquads), coefficients derived for any sample rate with the bilinear transform.
// At 48 kHz they reproduce the published coefficients.
struct KWeighting
{
    Coeffs shelf, highpass;

    static KWeighting make (double sr)
    {
        KWeighting k;
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / sr);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            k.shelf = { (Vh + Vb * K / Q + K * K) / a0, 2.0 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                        2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            k.highpass = { 1.0, -2.0, 1.0, 2.0 * (K * K - 1.0) / a0, (1.0 - K / Q + K * K) / a0 };
        }
        return k;
    }
};

struct KFilter
{
    Biquad s, h;
    void set (const KWeighting& k) { s.c = k.shelf; h.c = k.highpass; }
    void reset() { s.reset(); h.reset(); }
    inline float tick (float x) noexcept { return h.tick (s.tick (x)); }
};

constexpr double kLufsOffset = -0.691;
inline double powerToLufs (double p) { return kLufsOffset + 10.0 * std::log10 (std::max (p, 1.0e-12)); }
inline double lufsToPower (double l) { return std::pow (10.0, (l - kLufsOffset) / 10.0); }

// ---------------------------------------------------------------------------------------------------------------
// Gated integrated loudness (400 ms blocks, 75 % overlap, -70 LUFS absolute gate, -10 LU relative gate).
class LoudnessMeter
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        sub = std::max (1, (int) std::lround (sr * 0.1));
        const auto k = KWeighting::make (sr);
        fl.set (k); fr.set (k);
        reset();
    }

    void reset()
    {
        fl.reset(); fr.reset();
        accL = accR = 0.0; cnt = 0;
        ring.fill (0.0); ringN = 0; ringPos = 0;
        blocks.clear();
        maxMomentary = -200.0;
    }

    void push (const float* l, const float* r, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            const double a = fl.tick (l[i]), b = fr.tick (r[i]);
            accL += a * a; accR += b * b;
            if (++cnt >= sub)
            {
                ring[(size_t) ringPos] = (accL + accR) / (double) sub;   // sum of channel mean-squares, 100 ms
                ringPos = (ringPos + 1) & 3; ringN = std::min(ringN + 1, 4);
                accL = accR = 0.0; cnt = 0;
                if (ringN == 4)
                {
                    const double p = 0.25 * (ring[0] + ring[1] + ring[2] + ring[3]);
                    blocks.push_back ((float) p);
                    maxMomentary = std::max (maxMomentary, powerToLufs (p));
                }
            }
        }
    }

    // gated integrated loudness in LUFS (-200 if nothing above the gate)
    double integrated() const
    {
        const double absThr = lufsToPower (-70.0);
        double s = 0; size_t c = 0;
        for (float p : blocks) if (p > absThr) { s += p; ++c; }
        if (c == 0) return -200.0;
        const double relThr = (s / (double) c) * 0.1;   // -10 LU
        double s2 = 0; size_t c2 = 0;
        for (float p : blocks) if (p > absThr && p > relThr) { s2 += p; ++c2; }
        if (c2 == 0) return -200.0;
        return powerToLufs (s2 / (double) c2);
    }

    // loudness (LUFS) at a given percentile (0..1) of the blocks above the absolute gate
    double percentile (double q) const
    {
        const double absThr = lufsToPower (-70.0);
        std::vector<float> v;
        v.reserve (blocks.size());
        for (float p : blocks) if (p > absThr) v.push_back (p);
        if (v.empty()) return -200.0;
        std::sort (v.begin(), v.end());
        const size_t i = std::min (v.size() - 1, (size_t) std::floor (q * (double) (v.size() - 1) + 0.5));
        return powerToLufs (v[i]);
    }

    double maxMomentaryLufs() const { return maxMomentary; }
    size_t numBlocks() const { return blocks.size(); }
    const std::vector<float>& blockPowers() const { return blocks; }

private:
    double sr = 44100.0;
    int sub = 4410, cnt = 0, ringN = 0, ringPos = 0;
    KFilter fl, fr;
    double accL = 0, accR = 0, maxMomentary = -200.0;
    std::array<double, 4> ring {};
    std::vector<float> blocks;
};

// ---------------------------------------------------------------------------------------------------------------
// Real-time loudness estimate (exponential window). tau = 0.4 s approximates momentary, 3 s short-term.
class LoudnessFollower
{
public:
    void prepare (double sampleRate, double tauSeconds = 3.0)
    {
        sr = sampleRate;
        coef = 1.0 - std::exp (-1.0 / (sr * tauSeconds));
        const auto k = KWeighting::make (sr);
        fl.set (k); fr.set (k);
        reset();
    }
    void reset() { fl.reset(); fr.reset(); ms = 0.0; }

    void push (const float* l, const float* r, int n)
    {
        double m = ms;
        for (int i = 0; i < n; ++i)
        {
            const double a = fl.tick (l[i]), b = fr.tick (r[i]);
            m += coef * (a * a + b * b - m);
        }
        ms = m;
    }
    double lufs() const { return ms > 1.0e-10 ? powerToLufs (ms) : -200.0; }

private:
    double sr = 44100.0, coef = 0.0, ms = 0.0;
    KFilter fl, fr;
};

// ---------------------------------------------------------------------------------------------------------------
// Inter-sample ("true") peak estimate: 4x oversampled with a windowed-sinc interpolator (12 taps per phase).
class TruePeak
{
public:
    TruePeak()
    {
        for (int p = 1; p < 4; ++p)
            for (int k = 0; k < kTaps; ++k)
            {
                const double x = (double) (k - (kTaps / 2 - 1)) - (double) p / 4.0;   // distance from the interpolation point
                const double sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (kPi * x) / (kPi * x);
                const double w = 0.42 + 0.5 * std::cos (kPi * x / (kTaps / 2 + 0.5)) + 0.08 * std::cos (2.0 * kPi * x / (kTaps / 2 + 0.5));
                h[(size_t) p][(size_t) k] = (float) (sinc * w);
            }
        reset();
    }
    void reset() { hist.fill (0.f); peak = 0.f; samplePeak = 0.f; }

    // feed one channel, call once per channel with its own instance
    inline void tick (float x) noexcept
    {
        for (int k = 0; k < kTaps - 1; ++k) hist[(size_t) k] = hist[(size_t) k + 1];
        hist[kTaps - 1] = x;
        samplePeak = std::max (samplePeak, std::abs (x));
        peak = std::max (peak, std::abs (hist[kTaps / 2]));
        for (int p = 1; p < 4; ++p)
        {
            float s = 0.f;
            for (int k = 0; k < kTaps; ++k) s += h[(size_t) p][(size_t) k] * hist[(size_t) k];
            peak = std::max (peak, std::abs (s));
        }
    }
    float truePeak() const { return peak; }
    float samplePk() const { return samplePeak; }

private:
    static constexpr int kTaps = 12;
    std::array<std::array<float, kTaps>, 4> h {};
    std::array<float, kTaps> hist {};
    float peak = 0.f, samplePeak = 0.f;
};
} // namespace aio
