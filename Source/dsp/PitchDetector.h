#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include "Biquad.h"

namespace aio
{
/*
   Monophonic pitch detector (YIN) on a low-passed, decimated copy of the mono input.
   Feed it one sample at a time; push() returns true whenever a fresh estimate (voiced or not) has been produced
   (every kHop decimated samples, ~8 ms).
*/
class PitchDetector
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        decim = std::max (1, (int) std::lround (sr / 12000.0));
        srD = sr / decim;
        lpCoef = 1.0 - std::exp (-2.0 * kPi * 2200.0 / sr);
        dBuf.assign (kRing, 0.f);
        frame.assign (kRing, 0.f);
        dArr.assign ((size_t) kRing, 0.f);
        cmnd.assign ((size_t) kRing, 0.f);
        reset();
    }

    void reset()
    {
        std::fill (dBuf.begin(), dBuf.end(), 0.f);
        lp1 = lp2 = 0.0; decCount = 0; dWrite = 0; hopCount = 0; filled = 0;
        isVoiced = false; f0Hz = 0.0; conf = 0.0; rmsDbVal = -120.0;
    }

    bool push (double mono, float gateDb, float minHz, float maxHz)
    {
        lp1 += lpCoef * (mono - lp1);
        lp2 += lpCoef * (lp1 - lp2);
        if (++decCount < decim) return false;
        decCount = 0;
        dBuf[(size_t) dWrite] = (float) lp2;
        dWrite = (dWrite + 1) % kRing;
        filled = std::min (filled + 1, kRing);
        if (++hopCount < kHop) return false;
        hopCount = 0;
        return run (gateDb, minHz, maxHz);
    }

    bool   voiced() const     { return isVoiced; }
    double f0() const         { return f0Hz; }
    double confidence() const { return conf; }
    double rmsDb() const      { return rmsDbVal; }
    double midi() const       { return f0Hz > 0.0 ? 69.0 + 12.0 * std::log2 (f0Hz / 440.0) : -1.0; }

private:
    static constexpr int kRing = 1024;   // decimated history
    static constexpr int kW = 480;       // YIN integration window (decimated samples)
    static constexpr int kHop = 96;      // detection hop (decimated samples)

    bool run (float gateDb, float minHz, float maxHz)
    {
        const int tauMin = std::max (2, (int) std::floor (srD / (double) maxHz));
        const int tauMax = std::min (kRing - kW - 2, (int) std::ceil (srD / (double) minHz));
        if (filled < kW + tauMax + 2 || tauMax <= tauMin + 2) return false;

        const int total = kW + tauMax + 1;
        for (int i = 0; i < total; ++i)
            frame[(size_t) i] = dBuf[(size_t) ((dWrite - total + i + 2 * kRing) % kRing)];

        double e = 0;
        for (int i = total - kW; i < total; ++i) e += (double) frame[(size_t) i] * frame[(size_t) i];
        rmsDbVal = 10.0 * std::log10 (e / kW + 1.0e-12);

        double f = 0.0, c = 0.0;
        if (rmsDbVal > (double) gateDb)
            f = yin (tauMin, tauMax, c);

        isVoiced = f > 0.0 && c > 0.6;
        f0Hz = isVoiced ? f : 0.0;
        conf = c;
        return true;
    }

    double yin (int tauMin, int tauMax, double& cf)
    {
        const float* x = frame.data();
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double sum = 0;
            for (int j = 0; j < kW; ++j) { const double d = (double) x[j] - (double) x[j + tau]; sum += d * d; }
            dArr[(size_t) tau] = (float) sum;
        }
        cmnd[0] = 1.f;
        double run = 0;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            run += dArr[(size_t) tau];
            cmnd[(size_t) tau] = run > 1.0e-12 ? (float) (dArr[(size_t) tau] * tau / run) : 1.f;
        }

        int found = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (cmnd[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau]) ++tau;
                found = tau;
                break;
            }
        }
        if (found < 0)
        {
            int mn = tauMin;
            for (int tau = tauMin; tau < tauMax; ++tau) if (cmnd[(size_t) tau] < cmnd[(size_t) mn]) mn = tau;
            if (cmnd[(size_t) mn] < 0.35f) found = mn; else { cf = 0; return 0.0; }
        }

        double tauF = found;
        if (found > 1 && found < tauMax)
        {
            const double s0 = cmnd[(size_t) found - 1], s1 = cmnd[(size_t) found], s2 = cmnd[(size_t) found + 1];
            const double den = 2.0 * (2.0 * s1 - s2 - s0);
            if (std::abs (den) > 1.0e-12) tauF += std::clamp ((s2 - s0) / den, -1.0, 1.0);
        }
        cf = 1.0 - (double) cmnd[(size_t) found];
        return srD / tauF;
    }

    double sr = 44100.0, srD = 11025.0, lpCoef = 0.2;
    int decim = 4;
    std::vector<float> dBuf, frame, dArr, cmnd;
    double lp1 = 0, lp2 = 0;
    int decCount = 0, dWrite = 0, hopCount = 0, filled = 0;
    bool isVoiced = false;
    double f0Hz = 0.0, conf = 0.0, rmsDbVal = -120.0;
};
} // namespace aio
