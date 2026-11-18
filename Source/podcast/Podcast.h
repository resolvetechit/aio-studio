#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <functional>
#include <cstddef>

namespace aio
{
namespace podcast
{
// ---------------------------------------------------------------------------------------------------------------
// "Podcast Ready": an offline clean-up chain for a whole spoken-word file.
//
//   declip (best effort)  ->  denoise (spectral Wiener)  ->  Auto Voice, speech mode (leveler, de-ess, tone, compression)
//   ->  loudness normalise to a LUFS target  ->  look-ahead true-peak limiter
//
// Everything works on 1 or 2 channel float buffers of any length and sample rate.
// ---------------------------------------------------------------------------------------------------------------
struct Options
{
    bool  declip = true;
    float denoise = 0.5f;       // 0..1
    float level = 1.f;          // 0..1  voice leveler (evens out loud and quiet words)
    float tone = 0.7f;          // 0..1  adaptive EQ toward a clear speech curve
    float deess = 0.6f;         // 0..1
    float comp = 0.7f;          // 0..1
    float targetLufs = -16.f;   // -24 .. -12   (-16 = stereo podcast standard, -19 = mono podcast standard)
    float ceilingDb = -1.f;     // true-peak ceiling
    bool  mono = false;         // fold to mono before processing (output is a one-channel file)
};

struct Stats
{
    bool   valid = false;
    double lufs = -200.0;        // integrated, gated
    double truePeakDb = -200.0;
    double spreadDb = 0.0;       // loud-to-quiet variation of the spoken parts (95th - 10th percentile of 400 ms blocks above the relative gate)
    double noiseFloorDb = -200.0;// level of the quietest background in dBFS RMS
    double snrDb = 0.0;          // speech loudness minus background (approximate)
    long long clipped = 0;       // samples sitting on a flat clipped top
};

struct Report
{
    Stats before, after;
    long long declipped = 0;     // samples that were rebuilt
    bool   denoised = false;
    bool   denoiseEased = false; // no clear pauses were found, so the denoiser backed off
    double gainDb = 0.0;         // loudness gain applied after levelling
    double limiterPeakGrDb = 0.0;
    bool   reachedTarget = false;
};

using Progress = std::function<bool (float)>;      // return false to cancel

// ---- measurements
Stats measure (const float* l, const float* r, size_t n, double sr);
long long countClipped (const float* x, size_t n);

// ---- stages (in place)
long long declipChannel (float* x, size_t n);                                         // returns the number of samples rebuilt
bool denoise (float* l, float* r, size_t n, double sr, float amount, bool& eased, const Progress& prog);   // r may equal l (mono) or be null

// ---- the whole chain; `ch` has 1 or 2 channels and is processed in place. Returns false if cancelled.
bool process (juce::AudioBuffer<float>& ch, double sr, const Options& o, Report& rep, const Progress& prog);
} // namespace podcast
} // namespace aio
