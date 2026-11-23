#pragma once
#include <array>
#include <vector>
#include <memory>
#include <cstddef>
#include <functional>

namespace aio
{
namespace podcast
{
// ---------------------------------------------------------------------------------------------------------------
// Speaker detection for one-track recordings (a ceremony, a speech night: celebrant, groom, bride, all on one mic).
//
//   1. The file is cut into speech "chunks" at the pauses (about a second each, at most 6 s).
//   2. Each chunk gets a voice fingerprint: its pitch (median F0) and its timbre (mel-cepstrum, the same idea speaker-ID uses)
//      plus the long-term octave-band spectrum of its voiced frames (that is what the per-speaker EQ is built from).
//   3. The chunks are grouped into voices (k-means with a "people don't change every second" smoothing); the number of voices is
//      picked automatically or fixed by the user, and any chunk can be re-assigned by hand.
//
// It is a statistical guess, not magic: a man and a woman are separated reliably, two women of similar voice are best-effort.
// ---------------------------------------------------------------------------------------------------------------
constexpr int kMaxSpeakers = 4;
constexpr int kSpkBands = 8;      // octave bands 100 Hz .. 12.8 kHz, the same as Auto Voice
constexpr int kCepDims = 6;

struct SpeakerChunk
{
    long long a = 0, b = 0;                    // sample range of the speech run
    float f0 = 0.f;                            // Hz
    float levelDb = -100.f;                    // mean power over the speech frames
    float seconds = 0.f;                       // voiced seconds in the chunk
    float feat[kCepDims + 1] {};               // cepstral coefficients c1..c6, log2 F0
    std::array<float, kSpkBands> bandPow {};   // mean power per band over voiced frames (noise-reduced)
};

struct SpeakerInfo
{
    bool  valid = false;
    float f0 = 0.f, levelDb = -100.f, seconds = 0.f;
    std::array<float, kSpkBands> bandDb {};
};

struct SpeakerMap
{
    bool valid = false;
    int count = 0;                             // voices found (1..4)
    int requested = 0;                         // 0 = automatic
    float separation = 0.f;                    // silhouette of the grouping: how distinct the voices are (0..1)
    long long length = 0;
    double sampleRate = 0.0;
    std::vector<SpeakerChunk> chunks;
    std::vector<int> label;                    // voice of every chunk
    std::array<SpeakerInfo, kMaxSpeakers> speaker {};
    std::vector<long long> cutPos;             // time-ordered: from cutPos[i] on, the voice is cutSpk[i]
    std::vector<int> cutSpk;

    int speakerAt (long long pos) const;
    int chunkAt (long long pos) const;         // the chunk containing pos or the nearest one (-1 if there are none)
    void rebuild();                            // after labels changed: voice summaries and cut points
};

// groups the chunks into voices. requested = 0: pick automatically, 1..4: use exactly that many (1 switches the feature off)
void clusterSpeakers (SpeakerMap& m, int requested);

// what the processing does for each voice
struct SpeakerPlan
{
    bool active = false;
    int count = 0;
    std::array<std::array<float, kSpkBands>, kMaxSpeakers> eqDb {};
    std::array<float, kMaxSpeakers> trimDb {};
};
SpeakerPlan makeSpeakerPlan (const SpeakerMap& m, float tone, float balance);

// Applies the plan to a stream: each voice gets its own EQ curve and level trim, the switch between voices is cross-faded
// (about 40 ms, in the pause between them). pos = source index of the first sample of each call; calls must be consecutive.
class SpeakerEq
{
public:
    void prepare (double sampleRate, bool stereo);
    void setPlan (const SpeakerPlan& p);
    bool active() const { return plan.active; }
    void process (const SpeakerMap& m, float* l, float* r, int n, long long pos);
private:
    struct Chain;
    double sr = 44100.0;
    bool stereo = true;
    SpeakerPlan plan;
    std::shared_ptr<Chain> chains;       // one cascade per voice
    float w[kMaxSpeakers] {};
    float trimLin[kMaxSpeakers] {};
    double alpha = 0.0;
    size_t idx = 0;
};
} // namespace podcast
} // namespace aio
