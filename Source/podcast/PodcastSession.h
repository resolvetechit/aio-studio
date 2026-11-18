#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <array>
#include <memory>
#include <functional>
#include "Podcast.h"

namespace aio
{
// Loads a whole spoken-word file, runs the Podcast Ready chain on a worker thread, plays original / result through the
// plug-in for auditioning (audio-thread safe) and exports the result. Mirrors MasterSession so the two feel the same.
class PodcastSession
{
public:
    enum State { Idle = 0, Loading, Processing, Exporting, Failed };

    struct Info
    {
        bool hasFile = false, hasResult = false;
        juce::String name;
        double sampleRate = 0, seconds = 0;
        int channels = 0;
        podcast::Stats before;
        podcast::Report report;
        podcast::Options used;
        std::vector<float> waveform, waveformAfter;     // min,max pairs
    };

    static constexpr long long kMaxSamples = 80000000LL;   // frames x channels per buffer (~320 MB)

    PodcastSession();
    ~PodcastSession();

    std::function<void()> onWake;

    // ---- message thread
    void loadFile (const juce::File&);
    void run (const podcast::Options&);
    void exportFile (const juce::File& out, int bitDepth);      // 16 / 24 (WAV or FLAC by extension) / 32 (WAV float)
    void cancel();
    State state() const { return (State) stateV.load(); }
    bool  busy() const { const auto s = state(); return s == Loading || s == Processing || s == Exporting; }
    float progress() const { return progressV.load(); }
    juce::String message() const;
    Info info() const;
    bool takeExportDone (juce::String& msg);

    // ---- playback (audio thread safe)
    void prepare (double hostSampleRate);
    bool isPlaying() const { return playing.load(); }
    void setPlaying (bool p) { if (p && playPtrOrig.load() == nullptr) return; playing.store (p); }
    void seek (double normalised);
    double position() const;
    void readPlayback (float* l, float* r, int n);
    std::atomic<bool> abOriginal { false };
    std::atomic<bool> matchVolume { true };
    std::atomic<bool> loop { true };
    float matchGain() const { return matchGainLin.load(); }

private:
    class Job;
    void startJob (int initialState, std::function<void()> fn);
    void setMessage (const juce::String& m);
    void rebuildPlayback();
    static std::shared_ptr<juce::AudioBuffer<float>> resampled (const std::shared_ptr<juce::AudioBuffer<float>>& src, double srcSr, double dstSr);
    static void makeWaveform (const juce::AudioBuffer<float>& b, std::vector<float>& out);

    juce::AudioFormatManager formats;
    std::unique_ptr<Job> job;
    std::atomic<bool> cancelFlag { false };
    std::atomic<int> stateV { Idle };
    std::atomic<float> progressV { 0.f };
    mutable juce::CriticalSection lockInfo;
    Info infoV;
    juce::String msg, exportMsg;
    bool exportDone = false;
    std::shared_ptr<juce::AudioBuffer<float>> original, processed;
    double hostSr = 44100.0;

    std::atomic<const juce::AudioBuffer<float>*> playPtrOrig { nullptr }, playPtrProc { nullptr };
    std::array<std::shared_ptr<juce::AudioBuffer<float>>, 6> keep;
    int keepPos = 0;
    std::atomic<bool> playing { false };
    std::atomic<long long> playPos { 0 };
    std::atomic<float> matchGainLin { 1.f };
};
} // namespace aio
