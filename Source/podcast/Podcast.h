#pragma once
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>
#include <functional>
#include <cstddef>
#include <memory>

namespace aio
{
namespace podcast
{
// ---------------------------------------------------------------------------------------------------------------
// "Podcast Ready": an offline clean-up chain for a whole spoken-word file of any length.
//
//   declip (best effort)  ->  denoise (spectral Wiener)  ->  Auto Voice, speech mode (leveler, de-ess, tone, compression)
//   ->  loudness normalise to a LUFS target  ->  look-ahead true-peak limiter
//
// Everything is streamed in chunks: the source is read through a ReadFn, intermediate results go through a Store (RAM
// for the in-memory helpers, a temporary file for the plug-in), so memory use does not depend on the file length.
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
    bool  mono = false;         // fold to mono (output is a one-channel file)
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

using Progress = std::function<bool (float)>;                                       // return false to cancel
using ReadFn = std::function<bool (float* l, float* r, long long pos, int n)>;      // r is filled too (a copy of l for mono)

// Sequential store for audio between stages. append() is called in order, then finishWrite(), then read() at will.
class Store
{
public:
    virtual ~Store() = default;
    virtual bool append (const float* l, const float* r, int n) = 0;
    virtual bool finishWrite() = 0;
    virtual bool read (float* l, float* r, long long pos, int n) = 0;
    virtual long long length() const = 0;
};

class MemStore : public Store
{
public:
    bool append (const float* l, const float* r, int n) override { L.insert (L.end(), l, l + n); R.insert (R.end(), r, r + n); return true; }
    bool finishWrite() override { return true; }
    bool read (float* l, float* r, long long pos, int n) override;
    long long length() const override { return (long long) L.size(); }
    std::vector<float> L, R;
};

struct ClipInfo
{
    bool on = false;             // enough flat tops to believe it
    bool pos = false, neg = false;
    float posLevel = 0.f, negLevel = 0.f;
    long long clipped = 0;
};

struct NoiseProfile
{
    bool valid = false;
    std::vector<double> psd;     // power per FFT bin, learned from the quietest stretches
    double contrastDb = 0.0;     // loud frames vs quiet frames
};

struct Analysis
{
    bool valid = false;
    double sampleRate = 0.0;
    long long length = 0;
    Stats stats;
    ClipInfo clip[2];
    NoiseProfile noise;
    std::vector<float> waveform; // min,max pairs
};

// ---- whole-file stages
Analysis analyse (const ReadFn& src, long long len, double sr, const Progress& prog);
void scanClipping (const ReadFn& src, long long len, const Progress& prog, ClipInfo out[2]);
// `dual` = the two channels are different (a true stereo source that is not folded to mono)
bool processStream (const ReadFn& src, long long len, double sr, bool dual, const Analysis& a, const Options& o,
                    Store& pre, Store& out, Report& rep, std::vector<float>* waveformAfter, const Progress& prog, int chunkFrames = 1 << 16);

// ---- live preview: the same chain, run on the fly. Feed source frames in order; the output is aligned with the input
//      (output frame i = source frame i), arrives with some latency (so push() may return fewer frames than it was given),
//      and is levelled to the target by a slowly-moving gain estimated from what it has heard so far, then limited.
//      It is an approximation of the final run (which sees the whole file); the player warms it up for a few seconds first.
class LiveChain
{
public:
    LiveChain (std::shared_ptr<const Analysis> analysis, const Options& o, double sampleRate, bool dualSource, double initialGainDb);
    ~LiveChain();
    void push (const float* l, const float* r, int n, std::vector<float>& outL, std::vector<float>& outR);
    double gainDb() const;
    const Options& options() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p;
};

// ---- in-memory helpers (the same code, with RAM stores)
Stats measure (const float* l, const float* r, size_t n, double sr);
long long countClipped (const float* x, size_t n);
long long declipChannel (float* x, size_t n);                                         // returns the number of samples rebuilt
bool denoise (float* l, float* r, size_t n, double sr, float amount, bool& eased, const Progress& prog);   // r may equal l (mono) or be null
bool process (juce::AudioBuffer<float>& ch, double sr, const Options& o, Report& rep, const Progress& prog, int chunkFrames = 1 << 16);
} // namespace podcast
} // namespace aio
