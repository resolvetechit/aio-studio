#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <memory>
#include <array>
#include <atomic>

#include "PluginDefs.h"
#include "Params.h"
#include "dsp/Chain.h"
#include "dsp/Synth.h"
#include "dsp/Guide.h"
#include "dsp/PitchDetector.h"
#include "dsp/TalkBox.h"
#include "compose/Composer.h"
#include "compose/Styles.h"
#include "master/MasterEngine.h"
#include "master/MasterSession.h"
#include "sing/Sing.h"
#include "vocal/VocalSession.h"

class AIOProcessor : public juce::AudioProcessor,
                     private juce::AsyncUpdater,
                     private juce::AudioProcessorValueTreeState::Listener
{
public:
    AIOProcessor();
    ~AIOProcessor() override;

    // ---- juce::AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return true; }   // instrument build: unused; effect build: keys for the talk box
    bool producesMidi() const override { return JucePlugin_ProducesMidiOutput != 0; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 4.0; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // ---- parameters
    juce::AudioProcessorValueTreeState apvts;
    void setParam (const juce::String& id, float plainValue);

    // ---- UI-facing state (all thread-safe for the editor to poll)
    std::atomic<float> inPeak { 0.f }, outPeak { 0.f };
    const aio::Chain& getChain() const { return chain; }
    float getGainReductionDb() const    { return chain.comp.grMeter.load(); }
    float getLimiterReductionDb() const { return chain.limiter.grMeter.load(); }
    float getDetectedMidi() const       { return chain.tuner.getDetectedMidi(); }
    float getTargetMidi() const         { return chain.tuner.getTargetMidi(); }
    uint16_t getEffectiveTunerMask() const;
    int  readAnalyzer (float* dest, int maxSamples);    // pulls mono output samples for the spectrum display
    double getSampleRateForUI() const { return currentSampleRate; }

    // ---- Compose engine
    void regeneratePattern();                           // message thread only
    void newChordSeed();
    void newMelodySeed();

    // suggestions: every part has its own seed + history, so "next" / "previous" cycles through ideas
    void suggestPart (int part, int dir);               // dir +1 next idea, -1 previous (message thread)
    void applyStyleDefaults();                          // push the selected style's scale / progression / feel into the settings
    bool isPartLocked (int part) const { return part >= 0 && part < aio::kNumParts && partLocked[(size_t) part]; }
    void unlockPart (int part);
    void setPartNotes (int part, std::vector<aio::NoteEvent> notes);   // hand edit: freezes the part
    std::vector<aio::NoteEvent> getPartNotes (int part) const;
    const std::vector<aio::CustomChord>& getCustomChords() const { return customChords; }
    void addCustomChord (const aio::Chord&);
    void setCustomChords (std::vector<aio::CustomChord>);
    void clearCustomChords();
    int  harmoniseMelody();                             // chords that fit the current melody; returns how many
    void audition (int part, int note, int vel, int lenMs);   // message thread -> audio thread. lenMs > 0 one-shot, 0 hold, < 0 release
    static int midiChannelForPart (int part) { static const int ch[5] = { 1, 2, 3, 10, 4 }; return ch[std::clamp (part, 0, 4)]; }
    std::shared_ptr<const aio::Pattern> getPatternForUI() const { return uiPattern; }   // message thread only
    std::atomic<bool>   previewPlaying { false };
    std::atomic<double> uiBeat { 0.0 };                 // playhead inside the pattern, in beats
    std::atomic<bool>   composeRunning { false };
    std::atomic<double> lastBpm { 120.0 };
    juce::Result writeMidiFile (const juce::File& f) const;
    juce::File   makeTempMidiFile() const;
    aio::ComposeSettings readComposeSettings() const;

    // ---- Sing mode
    struct PitchFrame { double beat; float midi; };     // midi < 0: unvoiced
    std::atomic<bool>   singPlaying { false };          // internal clock (when the host is stopped)
    std::atomic<bool>   singRunning { false };
    std::atomic<double> singBeat { 0.0 };               // absolute beat at the start of the last block
    std::shared_ptr<const aio::SingTrack> getSingTrackForUI() const { return uiSingTrack; }   // message thread only
    int  popPitchFrames (PitchFrame* dest, int maxFrames);
    void pushPitchFrame (double beat, float midi);      // audio thread (public for tests)
    juce::String importSingMidi (const juce::File&);    // returns an error message, or empty on success
    juce::String getSingTrackLabel() const { return singLabel; }
    void rebuildSingTrack();

    // ---- Auto Master
    aio::MasterSession& getMaster() { return master; }
    aio::LiveLearn&     getLearn()  { return learn; }
    aio::MasterOptions  readMasterOptions() const;
    void applyMasterSettings (const aio::MasterSettings&);
    void analyseAndMaster();                            // offline: needs a loaded file
    void exportMaster (const juce::File& out, int bitDepth);
    void startLearning();
    bool finishLearning();                              // true if something usable was learned
    bool getLearnResult (aio::MasterAnalysis& a, aio::MasterSettings& s) const;
    aio::ChainParams readChainParams (double bpm) const;

    // ---- Vocal note editor (effect build)
    aio::VocalSession&  getVocal() { return vocal; }
    aio::AutoSettings   readVocalAuto() const;
    aio::VocalSession::PlayParams readVocalPlay() const;
    void autoEditVocal (const std::vector<char>* scope = nullptr);   // message thread: one-click correction of the notes

    // ---- Talk box (effect build)
    aio::TalkBox&       getTalk() { return talk; }
    aio::TalkSettings   readTalkSettings() const;
    void setTalkKey (int note)  { talkKey.store (note); }            // on-screen key (-1 = released)
    static int  talkPresetCount();
    static const char* talkPresetName (int index);
    void applyTalkPreset (int index);                                // message thread

private:
    static BusesProperties makeBuses();
    struct Tx;
    struct Ev { int sample; bool on; int part; int note; int vel; };

    void handleAsyncUpdate() override;
    void parameterChanged (const juce::String& id, float) override;

    void processChunk (float* l, float* r, int n, int chunkOffset, const aio::ChainParams& cp, const Tx& tx, juce::MidiBuffer& midi);
    void detectSing (const float* l, const float* r, int n, const Tx& tx);
    int  gatherEvents (const aio::Pattern& p, double b0, double b1, int n, Ev* out, int maxEv) const;
    float f (const char* id) const { return apvts.getRawParameterValue (id)->load(); }
    float f (const juce::String& id) const { return apvts.getRawParameterValue (id)->load(); }
    void  releaseAllNotes (juce::MidiBuffer& midi, int sample);

    // talk box
    aio::TalkBox talk;
    std::atomic<int> talkKey { -1 };
    int talkKeyApplied = -1, talkSourceApplied = -1;
    struct TbEvt { int sample; int kind; int a; int b; };          // kind: 0 off, 1 on, 2 bend (a = -8192..8191), 3 mod (a = 0..127), 4 all off
    std::array<TbEvt, 256> tbEv {};
    int tbEvN = 0;

    // cached raw parameter pointers (no string work on the audio thread)
    std::array<std::array<std::atomic<float>*, 5>, 6> eqPtr {};
    std::array<std::atomic<float>*, 12> notePtr {};

    double currentSampleRate = 44100.0;
    int    chunkSize = 1024;

    // DSP
    aio::Chain chain;
    aio::MiniSynth synth;
    juce::AudioBuffer<float> synthBuf;
    aio::PitchDetector singDet;
    aio::GuideVoice guide;
    double medBuf[3] { 0, 0, 0 }; int medN = 0;

    bool tunerWasOn = false;
    int  reportedLatency = 0;

    // Compose state
    std::atomic<uint32_t> chordSeed { 20240611u }, melodySeed { 1337u }, bassSeed { 4242u }, drumSeed { 909u }, synthSeed { 303u };
    std::atomic<uint32_t>& seedOf (int part) { std::atomic<uint32_t>* a[5] = { &chordSeed, &melodySeed, &bassSeed, &drumSeed, &synthSeed }; return *a[std::clamp (part, 0, 4)]; }
    struct SeedHist { std::vector<uint32_t> seeds; int pos = -1; };
    std::array<SeedHist, 5> seedHist;
    std::array<bool, 5> partLocked {};
    std::array<std::vector<aio::NoteEvent>, 5> lockedNotes;
    std::vector<aio::CustomChord> customChords;
    std::atomic<int> lastFeelKey { -1 };
    struct Aud { int part, note, vel, lenMs; };
    struct AudOff { int part = 0, note = 0, left = 0; bool used = false; };
    std::array<AudOff, 32> audOff {};
    std::array<Aud, 64> audData {};
    juce::AbstractFifo audFifo { 64 };
    std::array<std::shared_ptr<const aio::Pattern>, 6> ring;
    int ringPos = 0;
    std::shared_ptr<const aio::Pattern> uiPattern;
    std::atomic<const aio::Pattern*> activePattern { nullptr };
    std::atomic<bool> patternDirty { false };
    std::atomic<int>  pendingLatency { -1 };

    // Sing state
    std::array<std::shared_ptr<const aio::SingTrack>, 6> singRing;
    int singRingPos = 0;
    std::shared_ptr<const aio::SingTrack> uiSingTrack, importedTrack;
    std::atomic<const aio::SingTrack*> activeSing { nullptr };
    juce::String singLabel;
    std::array<PitchFrame, 2048> pfData {};
    juce::AbstractFifo pfFifo { 2048 };

    double previewBeat = 0.0, expectedBeat = 0.0;
    bool wasRunning = false, wasComposeRun = false;
    bool outActive[5][128] {};
    std::array<Ev, 1024> evBuf;

    // Auto Master
    aio::MasterSession master;
    aio::VocalSession vocal;
    aio::LiveLearn learn;
    mutable juce::CriticalSection learnLock;
    aio::MasterAnalysis learnAnalysis; aio::MasterSettings learnSettings; bool haveLearn = false;

    // analyzer FIFO
    juce::AbstractFifo anaFifo { 8192 };
    std::array<float, 8192> anaData {};

    int currentProgram = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AIOProcessor)
};
