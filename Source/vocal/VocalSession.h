#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <array>
#include <vector>
#include "VocalEngine.h"

namespace aio
{
/*
   Holds one vocal clip (recorded from the plug-in input or loaded from a file), its notes and the rendered
   result, and plays it back through the plug-in.

   Threads
     message thread : everything that edits (doc()), loads, records, exports
     worker thread  : decoding, pitch analysis, rendering (always on a copy of the notes)
     audio thread   : capture() and process() only (lock free)
*/
class VocalSession : private juce::Thread
{
public:
    enum RecMode { RecIdle = 0, RecArmed, RecRecording, RecDone };

    struct PlayParams
    {
        bool follow = true;          // replace the input with the edited clip while the host plays
        bool loop = true;
        bool original = false;       // A/B: play the untouched clip
        double anchorBeat = 0.0;     // host beat at which the clip starts
    };

    VocalSession();
    ~VocalSession() override;

    void prepare (double hostSampleRate);

    // ---------------------------------------------------------------- message thread
    void loadFile (const juce::File&);
    void startRecording (bool waitForHost);          // false: start now, true: start when the host starts playing
    void stopRecording();
    int    recMode() const { return recModeV.load(); }
    double recSeconds() const { return (double) recLenV.load() / std::max (1.0, hostSr.load()); }
    double maxRecordSeconds() const { return kMaxRecSeconds; }

    void redetect (const AnalysisOptions& = {}, const SegmentOptions& = {});
    void clear();

    VocalDoc& doc() { return docV; }
    const VocalDoc& doc() const { return docV; }
    void notesChanged();                             // call after editing doc().notes
    void setRenderOptions (const RenderOptions& o);  // glide etc.; re-renders when changed
    bool hasClip() const { return hasClipV.load(); }
    double clipSeconds() const { return docV.clipSeconds; }
    double clipSampleRate() const { return docV.sr; }
    double renderedSeconds() const;                  // length of the edited result

    bool takeNewClip() { const bool b = clipArrived; clipArrived = false; return b; }   // true once after a new clip's notes arrived
    bool poll();                                     // message thread: true once when something new arrived
    bool takeAnchor (double& beat);                  // beat position of a finished recording
    juce::String status() const;
    bool busy() const { return busyV.load(); }
    bool rendering() const { return renderDone.load() < renderSerial.load(); }
    float progress() const { return progressV.load(); }

    juce::Result exportWav (const juce::File&, int bitDepth = 24) const;
    juce::File dragFile() const;                     // writes the edited clip to a temp wav and returns it (empty on failure)

    juce::MemoryBlock saveState();
    void loadState (const void* data, size_t size);

    // transport (UI)
    void play (bool p) { playing.store (p); if (p) stopAt.store (-1); }
    bool isPlaying() const { return playing.load(); }
    void seekSeconds (double s) { playPos.store ((long long) std::llround (std::max (0.0, s) * hostSr.load())); }
    void auditionRange (double s0, double s1);       // plays the edited clip between two times, once
    double playheadSeconds() const { return playheadSec.load(); }

    // ---------------------------------------------------------------- audio thread
    void capture (const float* l, const float* r, int n, bool hostPlaying, double hostBeat);
    // fills l / r with the clip and returns true when it should replace the plug-in input
    bool process (float* l, float* r, int n, bool hostPlaying, double hostBeat, double bpm, const PlayParams&);

    // test hooks
    std::vector<float> renderedCopy() const;

private:
    struct RenderJob { std::vector<VocalNote> notes; RenderOptions opts; int serial = 0; int hop = 256; double sr = 44100.0; };

    void run() override;
    void doLoad (const juce::File&);
    void doAnalyse();
    void doRender();
    void doLoadState();
    void finishRecording();
    void publishBuffer (const std::vector<float>& mono, bool edited);
    void buildOriginal();
    void setStatus (const juce::String&);
    void wake() { notify(); }
    juce::MemoryBlock encodeSource() const;

    static constexpr double kMaxRecSeconds = 180.0;

    juce::AudioFormatManager formats;
    VocalDoc docV;
    std::atomic<double> hostSr { 44100.0 };
    std::atomic<bool> hasClipV { false };

    // shared with the worker
    std::mutex mx;
    std::shared_ptr<const std::vector<float>> source;      // mono, at docV.sr
    double sourceSr = 44100.0;
    bool clipArrived = false;                        // message thread only
    juce::File pendingFile;
    bool wantLoad = false, wantAnalyse = false, wantRender = false, wantState = false;
    AnalysisOptions pendingA; SegmentOptions pendingS;
    RenderJob pendingRender;
    juce::MemoryBlock pendingState;
    std::atomic<int> renderSerial { 0 }, renderDone { 0 };
    std::atomic<bool> cancelRender { false };
    std::atomic<bool> cancelAll { false };
    juce::MemoryBlock flacCache;
    bool flacValid = false;
    RenderOptions renderOpts;
    double origBuiltSr = 0.0;
    juce::int64 stopRequestedAt = 0;

    // results for the message thread
    std::mutex resMx;
    bool resChanged = false;
    bool resNewClip = false;                               // doc replaced by the worker (load / analysis / state)
    VocalDoc resDoc;
    double resAnchor = 0.0; bool resAnchorValid = false;
    std::shared_ptr<juce::AudioBuffer<float>> editedShared, origShared;
    juce::String statusMsg;
    std::atomic<bool> busyV { false };
    std::atomic<float> progressV { 0.f };

    // playback buffers (host sample rate)
    std::array<std::shared_ptr<juce::AudioBuffer<float>>, 6> keep;
    int keepPos = 0;
    std::atomic<const juce::AudioBuffer<float>*> editedPtr { nullptr }, origPtr { nullptr };
    std::atomic<bool> playing { false };
    std::atomic<long long> playPos { 0 };
    std::atomic<long long> stopAt { -1 };
    std::atomic<double> playheadSec { 0.0 };

    // recording
    std::vector<float> recBuf;
    float* recData = nullptr;
    int recCap = 0;
    int recPos = 0;
    std::atomic<int> recModeV { RecIdle };
    std::atomic<int> recLenV { 0 };
    std::atomic<bool> stopReq { false };
    bool recWaitHost = false, recAnchorPending = false, lastHostPlaying = false;
    double recAnchor = 0.0;
    std::atomic<double> recAnchorOut { 0.0 };
};
} // namespace aio
