#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <functional>
#include "Podcast.h"

namespace aio
{
class PodcastPlayer;
class FileStore;

// Loads a voice recording of any length (it is analysed and processed in chunks, results go to temporary files on disk),
// runs the Podcast Ready chain on a worker thread, streams original / result through the plug-in for auditioning
// (audio-thread safe) and exports the result. Mirrors MasterSession so the two feel the same.
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

    static constexpr double kMaxSeconds = 4.0 * 3600.0;

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
    void setPlaying (bool p);
    void seek (double normalised);
    double position() const;
    void readPlayback (float* l, float* r, int n);
    int  bufferedMs() const;                 // how much audio the player has ready (for tests and the UI)
    int  underruns() const;
    std::atomic<bool> abOriginal { false };
    std::atomic<bool> matchVolume { true };
    std::atomic<bool> loop { true };
    float matchGain() const { return matchGainLin.load(); }

private:
    class Job;
    void startJob (int initialState, std::function<void()> fn);
    void setMessage (const juce::String& m);
    void clearTemp();

    juce::AudioFormatManager formats;
    std::unique_ptr<Job> job;
    std::atomic<bool> cancelFlag { false };
    std::atomic<int> stateV { Idle };
    std::atomic<float> progressV { 0.f };
    mutable juce::CriticalSection lockInfo;
    Info infoV;
    juce::String msg, exportMsg;
    bool exportDone = false;

    juce::File sourceFile, tempDir;
    std::shared_ptr<podcast::Analysis> analysis;
    bool sourceDual = false;
    std::shared_ptr<FileStore> finalStore;

    std::unique_ptr<PodcastPlayer> player;
    std::atomic<bool> playing { false };
    std::atomic<float> matchGainLin { 1.f };
    std::atomic<bool> resultPlayable { false };
    double hostSrCache = 44100.0;
};
} // namespace aio
