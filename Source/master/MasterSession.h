#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <functional>
#include <mutex>
#include "MasterEngine.h"

namespace aio
{
// ---------------------------------------------------------------------------------------------------------------
// Learns from the live signal on the bus: the audio thread pushes samples into a lock-free FIFO,
// a background thread feeds the same Analyser the offline path uses.
class LiveLearn : private juce::Thread
{
public:
    LiveLearn();
    ~LiveLearn() override;

    void prepare (double sampleRate);
    void start();                       // message thread
    MasterAnalysis stop();              // message thread; returns what was learned
    bool   isLearning() const { return learning.load(); }
    double seconds() const { return learnedSeconds.load(); }
    float  inputLevelDb() const { return levelDb.load(); }

    void feed (const float* l, const float* r, int n);   // audio thread

private:
    void run() override;
    void drain();

    static constexpr int kFifo = 1 << 17;
    juce::AbstractFifo fifo { kFifo };
    std::vector<float> bufL, bufR, tmpL, tmpR;
    std::atomic<bool> learning { false };
    std::atomic<double> learnedSeconds { 0.0 };
    std::atomic<float> levelDb { -100.f };
    std::mutex lock;
    Analyser analyser;
    double sr = 44100.0;
};

// ---------------------------------------------------------------------------------------------------------------
// Loads an audio file (WAV / MP3 / FLAC / AIFF / OGG), analyses it, runs the Auto Master, exports the result,
// and plays the file through the plug-in for auditioning. All the heavy work happens on a worker thread.
class MasterSession
{
public:
    enum State { Idle = 0, Loading, Analysing, Mastering, Exporting, Failed };

    struct Info
    {
        bool hasFile = false;
        juce::String name;
        double sampleRate = 0, seconds = 0;
        MasterAnalysis before;
        bool hasAfter = false;  RenderStats after;
        bool hasSettings = false; MasterSettings settings;
        std::vector<float> waveform;      // min,max pairs
    };

    MasterSession();
    ~MasterSession();

    std::function<void()> onWake;         // called from the worker (any thread) when a result is waiting

    // ---- message thread
    void loadFile (const juce::File&);
    void runAutoMaster (const MasterOptions&);
    void exportFile (const juce::File& out, const ChainParams& p, int bitDepth);
    void cancel();
    State state() const { return (State) stateV.load(); }
    bool  busy() const { const auto s = state(); return s == Loading || s == Analysing || s == Mastering || s == Exporting; }
    float progress() const { return progressV.load(); }
    juce::String message() const;
    Info info() const;
    bool takeResult (MasterSettings& s, RenderStats& st);
    bool takeExportDone (juce::String& msg);

    // ---- playback (audio thread safe)
    void prepare (double hostSampleRate);
    bool isPlaying() const { return playing.load(); }
    void setPlaying (bool p) { if (p && ! playBufValid()) return; playing.store (p); }
    void seek (double normalised);
    double position() const;
    void readPlayback (float* l, float* r, int n);
    std::atomic<bool> abOriginal { false };
    std::atomic<float> matchGainLin { 1.f };
    std::atomic<bool> loop { true };

private:
    class Job;
    void startJob (int initialState, std::function<void()> fn);
    void setMessage (const juce::String& m);
    bool playBufValid() const { return playPtr.load() != nullptr; }
    void rebuildPlayback();

    juce::AudioFormatManager formats;
    std::unique_ptr<Job> job;
    std::atomic<bool> cancelFlag { false };
    std::atomic<int> stateV { Idle };
    std::atomic<float> progressV { 0.f };
    mutable juce::CriticalSection lockInfo;
    Info infoV;
    juce::String msg;
    std::shared_ptr<juce::AudioBuffer<float>> original;
    double hostSr = 44100.0;

    bool resultReady = false, exportDone = false;
    MasterSettings resultSettings; RenderStats resultStats;
    juce::String exportMsg;

    // playback
    std::atomic<const juce::AudioBuffer<float>*> playPtr { nullptr };
    std::array<std::shared_ptr<juce::AudioBuffer<float>>, 3> keep;
    int keepPos = 0;
    std::atomic<bool> playing { false };
    std::atomic<long long> playPos { 0 };
};
} // namespace aio
