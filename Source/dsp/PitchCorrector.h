#pragma once
#include <vector>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include "Biquad.h"
#include "PitchDetector.h"

namespace aio
{
struct TunerSettings
{
    bool     on = false;
    uint16_t mask = 0x0FFF;   // allowed pitch classes, bit 0 = C
    float    retuneMs = 20.f; // 0 = instant ("hard tune")
    float    amount = 1.f;    // 0..1
    float    fineCents = 0.f; // global offset of the target pitch
    float    minHz = 100.f, maxHz = 800.f;
    float    gateDb = -55.f;
    int      forcePc = -1;    // >= 0: pull the voice to this pitch class (any octave) instead of using the scale mask
};

/*
   Monophonic pitch corrector.

   Detection : YIN on a decimated copy of the mono sum (cheap), 3-point median, hysteresis on the target note.
   Shifting  : time-domain pitch-synchronous overlap-add (PSOLA-style). Grains are two periods long, are taken
               at integer multiples of the detected period (so they stay phase-aligned with the waveform) and
               are re-laid at the corrected period. Output is OLA-normalised.
   Latency   : fixed (see getLatencySamples) so the host can compensate it. When the voice is unpitched
               (breaths, consonants, silence) the delayed dry signal is crossfaded in, so nothing is lost.
*/
class PitchCorrector
{
public:
    void prepare (double sampleRate)
    {
        sr = sampleRate;
        latency = (int) std::ceil (0.032 * sr);
        inMask = 32767; accMask = 16383;
        for (auto& b : inBuf) b.assign ((size_t) inMask + 1, 0.f);
        for (auto& b : acc)   b.assign ((size_t) accMask + 1, 0.f);
        wsum.assign ((size_t) accMask + 1, 0.f);
        det.prepare (sr);
        reset();
    }

    int getLatencySamples() const { return latency; }

    void reset()
    {
        for (auto& b : inBuf) std::fill (b.begin(), b.end(), 0.f);
        for (auto& b : acc)   std::fill (b.begin(), b.end(), 0.f);
        std::fill (wsum.begin(), wsum.end(), 0.f);
        det.reset();
        n = 0; nextCenter = 0.0; prevA = 0.0; havePrevA = false;
        periodIn = sr / 200.0;
        voiced = false; voicedMix = 0.f;
        shiftTarget = shiftSm = 0.0;
        haveTarget = false; targetNote = 0;
        med[0] = med[1] = med[2] = 0.0; medN = 0;
        detMidi.store (-1.f); tgtMidi.store (-1.f);
    }

    void process (float* l, float* r, int numSamples, const TunerSettings& s)
    {
        const double retuneCoef = (s.retuneMs <= 0.5f) ? 1.0 : 1.0 - std::exp (-1.0 / (sr * (double) s.retuneMs * 0.001));
        const double voicedStep = 1.0 / (sr * 0.008);

        for (int i = 0; i < numSamples; ++i)
        {
            const float xl = l[i], xr = r[i];
            inBuf[0][(size_t) n & inMask] = xl;
            inBuf[1][(size_t) n & inMask] = xr;

            // ---- detection ----
            if (det.push (0.5 * ((double) xl + (double) xr), s.gateDb, s.minHz, s.maxHz))
                onEstimate (s);

            // ---- smooth correction amount (semitones) ----
            shiftSm += retuneCoef * (shiftTarget - shiftSm);
            const double ratio = std::exp2 (shiftSm / 12.0);

            // ---- schedule grains whose left edge reaches the current output index ----
            const double Pin = std::max (20.0, periodIn);
            const int H = (int) std::ceil (Pin);
            while (nextCenter - H - 1 <= (double) n)
            {
                const double T = nextCenter;
                const long long Ti = (long long) std::llround (T);
                double a;
                const double aTgt = T - latency;
                if (! havePrevA) a = aTgt;
                else             a = prevA + Pin * std::round ((aTgt - prevA) / Pin);

                // keep the grain's right edge inside what we have received so far
                if (a + H > (double) n)
                    a -= Pin * std::ceil ((a + H - (double) n) / Pin);
                // and inside the ring
                if (a - H < (double) (n - (long long) inMask + 64))
                    a = (double) (n - latency);

                const long long ai = (long long) std::llround (a);
                for (int k = -H; k <= H; ++k)
                {
                    const long long oi = Ti + k;
                    if (oi < n) continue;                       // already emitted
                    const float w = (float) (0.5 + 0.5 * std::cos (kPi * (double) k / Pin));
                    const size_t ao = (size_t) oi & accMask, ii = (size_t) (ai + k) & inMask;
                    acc[0][ao] += w * inBuf[0][ii];
                    acc[1][ao] += w * inBuf[1][ii];
                    wsum[ao]   += w;
                }
                prevA = a; havePrevA = true;
                nextCenter += Pin / ratio;
            }

            // ---- emit ----
            const size_t ao = (size_t) n & accMask;
            const float ws = wsum[ao];
            const size_t di = (size_t) (n - latency) & inMask;
            const float dryL = inBuf[0][di], dryR = inBuf[1][di];
            float oL = dryL, oR = dryR;

            voicedMix += (float) (((voiced && s.on) ? 1.0 : 0.0) - voicedMix) * (float) std::min (1.0, voicedStep * 4.0);
            if (ws > 1.0e-3f)
            {
                const float inv = 1.0f / ws;
                const float pL = acc[0][ao] * inv, pR = acc[1][ao] * inv;
                oL = dryL + (pL - dryL) * voicedMix;
                oR = dryR + (pR - dryR) * voicedMix;
            }
            acc[0][ao] = acc[1][ao] = 0.f;
            wsum[ao] = 0.f;

            l[i] = oL; r[i] = oR;
            ++n;
        }
    }

    float getDetectedMidi() const { return detMidi.load(); }
    float getTargetMidi()   const { return tgtMidi.load(); }

    static uint16_t maskFromScale (int key, const int* iv, int count)
    {
        uint16_t m = 0;
        for (int i = 0; i < count; ++i) m |= (uint16_t) (1u << ((key + iv[i]) % 12));
        return m;
    }

    // Exposed for tests / UI: nearest allowed note to a (fractional) midi value
    static int nearestAllowed (double midi, uint16_t mask)
    {
        int best = (int) std::lround (midi);
        double bd = 1e9;
        for (int k = (int) std::floor (midi) - 7; k <= (int) std::floor (midi) + 8; ++k)
        {
            if (! (mask & (1u << (((k % 12) + 12) % 12)))) continue;
            const double d = std::abs ((double) k - midi);
            if (d < bd) { bd = d; best = k; }
        }
        return best;
    }

private:
    void onEstimate (const TunerSettings& s)
    {
        voiced = det.voiced();

        if (! voiced)
        {
            shiftTarget = 0.0;
            detMidi.store (-1.f); tgtMidi.store (-1.f);
            haveTarget = false;
            medN = 0;
            return;
        }

        double midi = det.midi();
        // 3-point median against single-frame glitches
        med[2] = med[1]; med[1] = med[0]; med[0] = midi;
        if (medN < 3) ++medN;
        if (medN == 3)
        {
            const double a = med[0], b = med[1], c = med[2];
            midi = std::max (std::min (a, b), std::min (std::max (a, b), c));
        }

        periodIn += 0.6 * (sr / (440.0 * std::exp2 ((midi - 69.0) / 12.0)) - periodIn);
        detMidi.store ((float) midi);

        const bool forced = s.forcePc >= 0 && s.forcePc < 12;
        if (s.mask == 0 && ! forced) { shiftTarget = 0.0; tgtMidi.store (-1.f); return; }

        int cand;
        if (forced)
        {
            cand = nearestAllowed (midi - s.fineCents / 100.0, (uint16_t) (1u << s.forcePc));
        }
        else
        {
            cand = nearestAllowed (midi - s.fineCents / 100.0, s.mask);
            if (haveTarget && cand != targetNote)
            {
                // hysteresis: stay on the old note until the new one is clearly closer
                if (std::abs ((double) targetNote + s.fineCents / 100.0 - midi) < std::abs ((double) cand + s.fineCents / 100.0 - midi) + 0.18
                    && (s.mask & (1u << (((targetNote % 12) + 12) % 12))))
                    cand = targetNote;
            }
        }
        targetNote = cand; haveTarget = true;

        const double targetMidiVal = (double) cand + s.fineCents / 100.0;
        shiftTarget = std::clamp ((targetMidiVal - midi) * (double) s.amount, -12.0, 12.0);
        tgtMidi.store ((float) targetMidiVal);
    }

    double sr = 44100.0;
    int latency = 1411;
    size_t inMask = 32767, accMask = 16383;
    std::array<std::vector<float>, 2> inBuf, acc;
    std::vector<float> wsum;
    PitchDetector det;

    long long n = 0;
    double nextCenter = 0, prevA = 0, periodIn = 220.0;
    bool havePrevA = false, voiced = false;
    float voicedMix = 0.f;

    double shiftTarget = 0, shiftSm = 0;
    bool haveTarget = false; int targetNote = 0;
    double med[3] { 0, 0, 0 }; int medN = 0;

    std::atomic<float> detMidi { -1.f }, tgtMidi { -1.f };
};
} // namespace aio
