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
    enum State { Idle = 0, Loading, Processing, Exporting, Failed, Batching };

    struct Voice { float f0 = 0.f, seconds = 0.f, levelDb = -100.f; };
    struct Segment { double t0 = 0.0, t1 = 0.0; int voice = 0; };         // normalised 0..1 of the file

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
        // who speaks when
        int voiceCount = 0, voiceRequest = 0;           // voices found (0 = none detected), the count asked for (0 = automatic)
        float voiceSeparation = 0.f;                    // how distinct the voices are (0..1)
        std::vector<Voice> voices;
        std::vector<Segment> segments;
        bool resultStale = false;                       // the voices were edited after the last run
        bool decoded = false;                           // the audio was taken out of a video / compressed file
    };

    struct BatchStatus
    {
        int total = 0, done = 0, failed = 0;
        juce::String current;
        juce::StringArray log;                          // one line per finished file
        bool finished = false;
    };

    static constexpr double kMaxSeconds = 4.0 * 3600.0;

    PodcastSession();
    ~PodcastSession();

    std::function<void()> onWake;
    std::function<podcast::Options()> getOptions;       // supplied by the processor: the current knob settings (so the live preview is ready as soon as a file loads)

    // ---- message thread
    void loadFile (const juce::File&);
    void run (const podcast::Options&);
    void exportFile (const juce::File& out, int bitDepth);      // 16 / 24 (WAV or FLAC by extension) / 32 (WAV float)
    void cancel();
    State state() const { return (State) stateV.load(); }
    bool  busy() const { const auto s = state(); return s == Loading || s == Processing || s == Exporting || s == Batching; }
    float progress() const { return progressV.load(); }
    juce::String message() const;
    Info info() const;
    bool takeExportDone (juce::String& msg);

    // ---- voices (message thread, not while busy). 0 = find the number of voices automatically, 1 = treat it as one voice, 2..4 = exactly that many.
    void setSpeakerCount (int requested);
    // moves the speech chunk at this point of the file (0..1) to the next (dir = +1) or previous (-1) voice
    void cycleSpeakerAt (double normalisedPos, int dir);

    // ---- batch: every file in the list gets the same settings and lands in outDir as "<name> - podcast ready.wav|flac"
    void runBatch (const std::vector<juce::File>& files, const juce::File& outDir, const podcast::Options& o, int bitDepth, bool flac);
    BatchStatus batchStatus() const;

    // ---- playback (audio thread safe)
    void prepare (double hostSampleRate);
    bool isPlaying() const { return playing.load(); }
    void setPlaying (bool p);
    void seek (double normalised);
    double position() const;
    void readPlayback (float* l, float* r, int n);
    int  bufferedMs() const;                 // how much audio the player has ready (for tests and the UI)
    int  underruns() const;
    // ---- live preview: the knobs act on the playing audio (the real chain runs on the fly). The exact whole-file run is still what gets exported.
    void setLiveOptions (const podcast::Options&);      // any thread (cheap, lock-free)
    void setLive (bool on);
    bool live() const;                                   // the LIVE switch
    bool liveActive() const;                             // live and ready (a file is loaded and options were supplied)
    float liveGainDb() const;
    std::atomic<bool> abOriginal { false };
    std::atomic<bool> matchVolume { true };
    std::atomic<bool> loop { true };
    float matchGain() const { return matchGainLin.load(); }

private:
    class Job;
    void startJob (int initialState, std::function<void()> fn);
    void setMessage (const juce::String& m);
    void refreshMatchGain();
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
    BatchStatus batchV;
    void clearSourceTemp();
    void fillVoiceInfo (Info& in, const podcast::Analysis& a) const;
    void applySpeakerEdit (const std::function<void (podcast::SpeakerMap&)>& edit);

    juce::File sourceFile, tempDir, srcTempDir;     // srcTempDir holds the decoded audio of a video file for as long as it is loaded
    std::shared_ptr<podcast::Analysis> analysis;
    bool sourceDual = false;
    std::shared_ptr<FileStore> finalStore;

    std::unique_ptr<PodcastPlayer> player;
    std::atomic<bool> playing { false };
    std::atomic<float> matchGainLin { 1.f }, fileMatchGain { 1.f }, beforeLufs { -23.f }, beforePeak { -3.f }, liveTarget { -16.f };
    static constexpr bool liveDefault = true;
    std::atomic<bool> resultPlayable { false };
    double hostSrCache = 44100.0;
};
} // namespace aio
