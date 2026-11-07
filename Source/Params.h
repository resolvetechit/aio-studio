#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginDefs.h"
#include "compose/Composer.h"
#include "compose/Styles.h"

namespace aio
{
using Layout = juce::AudioProcessorValueTreeState::ParameterLayout;

inline juce::String eqId (int band, const char* what) { return "eq" + juce::String (band + 1) + what; }

inline juce::StringArray keyChoices()  { return { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }; }

inline juce::StringArray composeScaleChoices()
{
    juce::StringArray s;
    for (auto& d : scaleDefs()) s.add (d.name);
    return s;
}

inline juce::StringArray tuneScaleChoices()
{
    juce::StringArray s { "Chromatic" };
    for (auto& d : scaleDefs()) s.add (d.name);
    s.add ("Custom");
    return s;
}

inline juce::StringArray progressionChoices()
{
    juce::StringArray s;
    for (auto& d : progDefs()) s.add (d.name);
    s.add ("Random - Safe");
    s.add ("Random - Adventurous");
    return s;
}

inline juce::StringArray chordRhythmChoices()
{
    int n = 0; auto* names = chordRhythmNames (n);
    juce::StringArray s;
    for (int i = 0; i < n; ++i) s.add (names[i]);
    return s;
}

inline juce::StringArray delayDivChoices() { return { "1/16", "1/8", "1/8 dotted", "1/4", "1/4 dotted", "1/2", "1 bar" }; }
inline double delayDivBeats (int i)        { static const double t[7] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0 }; return t[std::clamp (i, 0, 6)]; }

inline juce::StringArray eqTypeChoices()   { return { "Bell", "Low Shelf", "High Shelf", "High Pass", "Low Pass", "Notch" }; }
inline juce::StringArray satTypeChoices()  { return { "Tape", "Tube", "Soft Clip", "Hard Clip", "Fold" }; }
inline juce::StringArray styleChoices()
{
    juce::StringArray a;
    for (const auto& d : aio::styleDefs()) a.add (d.name);
    return a;
}
inline juce::StringArray drumStyleChoices()
{
    juce::StringArray a ("By style");
    for (int i = 0; i < aio::kNumDrumFamilies; ++i) a.add (aio::drumFamilyName (i));
    return a;
}
inline juce::StringArray synthStyleChoices()
{
    juce::StringArray a ("By style");
    for (int i = 0; i < aio::kNumSynthFamilies; ++i) a.add (aio::synthFamilyName (i));
    return a;
}
inline juce::StringArray waveChoices()     { return { "Saw", "Square", "Triangle", "Sine" }; }

inline void addF (Layout& l, const juce::String& id, const juce::String& name, float lo, float hi, float def,
                  float skewCentre = 0.f, float step = 0.f, const juce::String& unit = {})
{
    juce::NormalisableRange<float> r (lo, hi, step);
    if (skewCentre > 0.f) r.setSkewForCentre (skewCentre);
    const int decimals = step >= 1.f ? 0 : (step >= 0.1f ? 1 : (step > 0.f ? 2 : (hi >= 100.f ? 0 : 2)));
    l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, r, def,
                                                        juce::AudioParameterFloatAttributes()
                                                            .withLabel (unit)
                                                            .withStringFromValueFunction ([decimals] (float v, int) { return juce::String (v, decimals); })));
}
inline void addB (Layout& l, const juce::String& id, const juce::String& name, bool def)
{
    l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def));
}
inline void addC (Layout& l, const juce::String& id, const juce::String& name, const juce::StringArray& ch, int def)
{
    l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, ch, def));
}
inline void addI (Layout& l, const juce::String& id, const juce::String& name, int lo, int hi, int def)
{
    l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { id, 1 }, name, lo, hi, def));
}

inline Layout createLayout()
{
    Layout l;

    // ---- global ----
    addF (l, "inGain",  "Input Gain",  -24.f, 24.f, 0.f, 0.f, 0.1f, "dB");
    addF (l, "outGain", "Output Gain", -24.f, 24.f, 0.f, 0.f, 0.1f, "dB");
    addF (l, "width",   "Stereo Width", 0.f, 200.f, 100.f, 0.f, 1.f, "%");
    addB (l, "limOn",   "Limiter", true);
    addF (l, "limCeil", "Limiter Ceiling", -12.f, 0.f, -1.0f, 0.f, 0.1f, "dB");
    addF (l, "monoBass", "Mono Bass Below", 0.f, 300.f, 0.f, 100.f, 1.f, "Hz");   // 0 = off

    // ---- tuner ----
    addB (l, "tuneOn",     "Tune On", false);
    addC (l, "tuneKey",    "Tune Key", keyChoices(), 0);
    addC (l, "tuneScale",  "Tune Scale", tuneScaleChoices(), 1);
    addB (l, "tuneLink",   "Tune Follows Compose", false);
    addF (l, "tuneSpeed",  "Retune Speed", 0.f, 400.f, 25.f, 60.f, 1.f, "ms");
    addF (l, "tuneAmount", "Tune Amount", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addF (l, "tuneFine",   "Tune Fine", -100.f, 100.f, 0.f, 0.f, 1.f, "ct");
    addF (l, "tuneGate",   "Tune Gate", -80.f, -20.f, -55.f, 0.f, 1.f, "dB");
    addC (l, "tuneVoice",  "Voice Range", { "Low (65-500 Hz)", "Mid (100-800 Hz)", "High (150-1200 Hz)", "Wide (60-1500 Hz)" }, 1);
    {
        static const bool cMajor[12] = { true, false, true, false, true, true, false, true, false, true, false, true };
        for (int i = 0; i < 12; ++i)
            addB (l, "tuneNote" + juce::String (i), juce::String ("Custom ") + pcName (i), cMajor[i]);
    }

    // ---- EQ ----
    addB (l, "eqOn", "EQ On", true);
    {
        struct D { int type; float f, g, q; bool on; };
        static const D d[6] = {
            { 3,    30.f, 0.f, 0.71f, false },
            { 1,   100.f, 0.f, 0.71f, true },
            { 0,   400.f, 0.f, 1.0f,  true },
            { 0,  1500.f, 0.f, 1.0f,  true },
            { 0,  5000.f, 0.f, 1.0f,  true },
            { 2, 10000.f, 0.f, 0.71f, true } };
        for (int i = 0; i < 6; ++i)
        {
            const juce::String n = "EQ" + juce::String (i + 1) + " ";
            addB (l, eqId (i, "On"),   n + "On", d[i].on);
            addC (l, eqId (i, "Type"), n + "Type", eqTypeChoices(), d[i].type);
            addF (l, eqId (i, "Freq"), n + "Freq", 20.f, 20000.f, d[i].f, 1000.f, 0.f, "Hz");
            addF (l, eqId (i, "Gain"), n + "Gain", -18.f, 18.f, d[i].g, 0.f, 0.1f, "dB");
            addF (l, eqId (i, "Q"),    n + "Q", 0.1f, 18.f, d[i].q, 1.f, 0.01f);
        }
    }

    // ---- compressor ----
    addB (l, "compOn",      "Comp On", false);
    addF (l, "compThresh",  "Comp Threshold", -60.f, 0.f, -18.f, 0.f, 0.1f, "dB");
    addF (l, "compRatio",   "Comp Ratio", 1.f, 20.f, 3.f, 4.f, 0.1f, ":1");
    addF (l, "compAttack",  "Comp Attack", 0.1f, 200.f, 10.f, 15.f, 0.1f, "ms");
    addF (l, "compRelease", "Comp Release", 5.f, 1000.f, 120.f, 150.f, 1.f, "ms");
    addF (l, "compKnee",    "Comp Knee", 0.f, 24.f, 6.f, 0.f, 0.1f, "dB");
    addF (l, "compMakeup",  "Comp Makeup", 0.f, 24.f, 0.f, 0.f, 0.1f, "dB");
    addF (l, "compMix",     "Comp Mix", 0.f, 100.f, 100.f, 0.f, 1.f, "%");

    // ---- saturation ----
    addB (l, "satOn",    "Sat On", false);
    addC (l, "satType",  "Sat Type", satTypeChoices(), 0);
    addF (l, "satDrive", "Sat Drive", 0.f, 36.f, 6.f, 0.f, 0.1f, "dB");
    addF (l, "satTone",  "Sat Tone", 1000.f, 20000.f, 20000.f, 5000.f, 0.f, "Hz");
    addF (l, "satMix",   "Sat Mix", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addF (l, "satOut",   "Sat Output", -12.f, 12.f, 0.f, 0.f, 0.1f, "dB");

    // ---- chorus ----
    addB (l, "choOn",    "Chorus On", false);
    addF (l, "choRate",  "Chorus Rate", 0.05f, 5.f, 0.8f, 0.8f, 0.01f, "Hz");
    addF (l, "choDepth", "Chorus Depth", 0.f, 100.f, 40.f, 0.f, 1.f, "%");
    addF (l, "choMix",   "Chorus Mix", 0.f, 100.f, 35.f, 0.f, 1.f, "%");

    // ---- delay ----
    addB (l, "dlyOn",       "Delay On", false);
    addB (l, "dlySync",     "Delay Sync", true);
    addC (l, "dlyDiv",      "Delay Division", delayDivChoices(), 1);
    addF (l, "dlyTime",     "Delay Time", 1.f, 2000.f, 350.f, 300.f, 1.f, "ms");
    addF (l, "dlyFeedback", "Delay Feedback", 0.f, 95.f, 35.f, 0.f, 1.f, "%");
    addB (l, "dlyPing",     "Delay Ping-Pong", true);
    addF (l, "dlyTone",     "Delay Tone", 500.f, 20000.f, 8000.f, 4000.f, 0.f, "Hz");
    addF (l, "dlyMix",      "Delay Mix", 0.f, 100.f, 25.f, 0.f, 1.f, "%");

    // ---- reverb ----
    addB (l, "revOn",    "Reverb On", false);
    addF (l, "revSize",  "Reverb Size", 0.f, 100.f, 60.f, 0.f, 1.f, "%");
    addF (l, "revDamp",  "Reverb Damping", 0.f, 100.f, 50.f, 0.f, 1.f, "%");
    addF (l, "revWidth", "Reverb Width", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addF (l, "revPre",   "Reverb Pre-Delay", 0.f, 200.f, 15.f, 0.f, 1.f, "ms");
    addF (l, "revLow",   "Reverb Low Cut", 20.f, 1000.f, 120.f, 150.f, 1.f, "Hz");
    addF (l, "revHigh",  "Reverb High Cut", 1000.f, 20000.f, 12000.f, 6000.f, 1.f, "Hz");
    addF (l, "revMix",   "Reverb Mix", 0.f, 100.f, 25.f, 0.f, 1.f, "%");

    // ---- compose ----
    addB (l, "cmpOn",        "Compose On", JucePlugin_IsSynth != 0);
    addC (l, "cmpKey",       "Compose Key", keyChoices(), 0);
    addC (l, "cmpScale",     "Compose Scale", composeScaleChoices(), 0);
    addC (l, "cmpProg",      "Progression", progressionChoices(), 0);
    addC (l, "cmpBars",      "Length", { "2 bars", "4 bars", "8 bars", "16 bars" }, 1);
    addC (l, "cmpChordLen",  "Chord Length", { "1/2 bar", "1 bar", "2 bars" }, 1);
    addC (l, "cmpChordKind", "Chord Type", { "Triads", "7ths", "9ths", "Sus2", "Sus4", "Add9", "Mixed" }, 0);
    addC (l, "cmpVoicing",   "Voicing", { "Close", "Open", "Wide" }, 0);
    addB (l, "cmpSmooth",    "Smooth Voice Leading", true);
    addC (l, "cmpRhythm",    "Chord Rhythm", chordRhythmChoices(), 0);
    addI (l, "cmpChordOct",  "Chord Octave", 1, 5, 3);
    addI (l, "cmpChordVel",  "Chord Velocity", 1, 127, 85);
    addF (l, "cmpHuman",     "Humanize", 0.f, 100.f, 20.f, 0.f, 1.f, "%");
    addF (l, "cmpSwing",     "Swing", 0.f, 100.f, 0.f, 0.f, 1.f, "%");
    addB (l, "cmpMelOn",     "Melody On", true);
    addF (l, "cmpMelDensity","Melody Density", 0.f, 100.f, 50.f, 0.f, 1.f, "%");
    addC (l, "cmpMelStyle",  "Melody Style", { "Stepwise", "Leaps", "Hooky", "Arpeggio" }, 0);
    addC (l, "cmpMelRange",  "Melody Range", { "1 octave", "1.5 octaves", "2 octaves", "3 octaves" }, 2);
    addI (l, "cmpMelOct",    "Melody Octave", 3, 6, 4);
    addF (l, "cmpMelRests",  "Melody Rests", 0.f, 100.f, 25.f, 0.f, 1.f, "%");
    addI (l, "cmpMelVel",    "Melody Velocity", 1, 127, 100);
    addB (l, "cmpBassOn",    "Bass On", true);
    addC (l, "cmpBassStyle", "Bass Style", { "Sustain", "8th pulse", "Octave bounce", "Syncopated", "Root + 5th", "By style" }, 5);

    // ---- style / drums / synth part
    addC (l, "cmpStyle",     "Style", styleChoices(), 0);
    addB (l, "cmpChordsOn",  "Chords On", true);
    addB (l, "cmpUseCustom", "Use My Chords", false);
    addB (l, "cmpDrumsOn",   "Drums On", true);
    addC (l, "cmpDrumStyle", "Drum Pattern", drumStyleChoices(), 0);
    addF (l, "cmpDrumBusy",  "Drum Variation", 0.f, 100.f, 50.f, 0.f, 1.f, "%");
    addI (l, "cmpDrumVel",   "Drum Velocity", 1, 127, 100);
    addB (l, "cmpSynOn",     "Synth Part On", true);
    addC (l, "cmpSynStyle",  "Synth Pattern", synthStyleChoices(), 0);
    addI (l, "cmpSynOct",    "Synth Octave", 3, 7, 5);
    addI (l, "cmpSynVel",    "Synth Velocity", 1, 127, 80);

    // ---- built-in preview synth ----
    addC (l, "synWaveChord", "Chords Wave", waveChoices(), 0);
    addC (l, "synWaveMel",   "Melody Wave", waveChoices(), 1);
    addC (l, "synWaveBass",  "Bass Wave", waveChoices(), 0);
    addC (l, "synWaveSyn",   "Synth Part Wave", waveChoices(), 1);
    addF (l, "synLevelChord","Chords Level", 0.f, 100.f, 55.f, 0.f, 1.f, "%");
    addF (l, "synLevelMel",  "Melody Level", 0.f, 100.f, 55.f, 0.f, 1.f, "%");
    addF (l, "synLevelBass", "Bass Level", 0.f, 100.f, 65.f, 0.f, 1.f, "%");
    addF (l, "synLevelSyn",  "Synth Part Level", 0.f, 100.f, 45.f, 0.f, 1.f, "%");
    addF (l, "synLevelDrum", "Drums Level", 0.f, 100.f, 70.f, 0.f, 1.f, "%");
    addF (l, "synCutoff",    "Synth Cutoff", 200.f, 20000.f, 7000.f, 2000.f, 0.f, "Hz");
    addF (l, "synAttack",    "Synth Attack", 1.f, 2000.f, 6.f, 80.f, 1.f, "ms");
    addF (l, "synRelease",   "Synth Release", 5.f, 4000.f, 250.f, 400.f, 1.f, "ms");

    // ---- auto voice ----
    addB (l, "voxOn",     "Auto Voice On", false);
    addC (l, "voxMode",   "Auto Voice Mode", { "Vocal (singing)", "Podcast / Speech", "Instrument" }, 0);
    addF (l, "voxLevel",  "Voice Leveler", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addF (l, "voxTone",   "Voice Tone Match", 0.f, 100.f, 70.f, 0.f, 1.f, "%");
    addF (l, "voxDeEss",  "Voice De-Ess", 0.f, 100.f, 60.f, 0.f, 1.f, "%");
    addF (l, "voxComp",   "Voice Compression", 0.f, 100.f, 70.f, 0.f, 1.f, "%");
    addF (l, "voxClean",  "Voice Clean-Up", 0.f, 100.f, 40.f, 0.f, 1.f, "%");
    addF (l, "voxTarget", "Voice Target Level", -34.f, -10.f, -20.f, 0.f, 0.5f, "dB");

    // ---- auto master (the options the algorithm works from) ----
    addC (l, "mastStyle",   "Master Style", { "Balanced", "Warm", "Bright", "Punchy", "Loud" }, 0);
    addF (l, "mastTarget",  "Master Loudness Target", -24.f, -6.f, -14.f, 0.f, 0.1f, "LUFS");
    addF (l, "mastAmount",  "Master Tone Amount", 0.f, 100.f, 60.f, 0.f, 1.f, "%");
    addF (l, "mastGlue",    "Master Glue", 0.f, 100.f, 50.f, 0.f, 1.f, "%");
    addF (l, "mastWarmth",  "Master Warmth", 0.f, 100.f, 30.f, 0.f, 1.f, "%");
    addB (l, "mastMonoBass","Master Mono Bass", true);
    addB (l, "mastFollow",  "Live Loudness Follow", false);

    // ---- sing mode ----
    addB (l, "singOn",       "Sing Mode", false);
    addC (l, "singSource",   "Sing Melody Source", { "Compose melody", "Imported MIDI" }, 0);
    addC (l, "singLevel",    "Sing Difficulty", { "Easy", "Medium", "Hard" }, 1);
    addB (l, "singOct",      "Sing In Any Octave", true);
    addB (l, "singGuide",    "Guide Tone", true);
    addF (l, "singGuideLvl", "Guide Level", 0.f, 100.f, 50.f, 0.f, 1.f, "%");
    addB (l, "singAssist",   "Assist: Pull Voice To Melody", false);

    // ---- vocal note editor ----
    addB (l, "vedFollow",  "Vocal Edit Play With Project", true);
    addB (l, "vedLoop",    "Vocal Edit Loop", true);
    addB (l, "vedOrig",    "Vocal Edit Play Original", false);
    addF (l, "vedAnchor",  "Vocal Clip Start", 0.f, 2048.f, 0.f, 0.f, 0.01f, "beats");
    addF (l, "vedCorrect", "Vocal Auto Correct", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addF (l, "vedFlatten", "Vocal Auto Flatten Drift", 0.f, 100.f, 40.f, 0.f, 1.f, "%");
    addF (l, "vedVibrato", "Vocal Auto Vibrato", 0.f, 200.f, 100.f, 0.f, 1.f, "%");
    addF (l, "vedGlide",   "Vocal Auto Glide", 0.f, 150.f, 15.f, 0.f, 1.f, "ms");
    addF (l, "vedTiming",  "Vocal Auto Timing", 0.f, 100.f, 0.f, 0.f, 1.f, "%");
    addC (l, "vedGrid",    "Vocal Auto Timing Grid", { "1/4", "1/8", "1/16", "1/32" }, 1);
    addF (l, "vedLevel",   "Vocal Auto Level Notes", 0.f, 100.f, 0.f, 0.f, 1.f, "%");
    addC (l, "vedSnap",    "Vocal Edit Snap", { "Off", "Semitone", "Scale" }, 1);

    // ---- talk box ----
    addB (l, "tbOn",       "Talk Box On", false);
    addC (l, "tbSource",   "Talk Box Notes From", { "Voice pitch", "MIDI / keys", "Compose", "Fixed note" }, 1);
    addC (l, "tbWave",     "Talk Box Synth", { "Saw lead", "Pulse", "Super saw", "Square" }, 0);
    addI (l, "tbOctave",   "Talk Box Octave", -2, 2, 0);
    addF (l, "tbGlide",    "Talk Box Glide", 0.f, 500.f, 60.f, 0.f, 1.f, "ms");
    addF (l, "tbVibRate",  "Talk Box Vibrato Rate", 3.f, 9.f, 5.5f, 0.f, 0.1f, "Hz");
    addF (l, "tbVibDepth", "Talk Box Vibrato Depth", 0.f, 100.f, 12.f, 0.f, 1.f, "cents");
    addF (l, "tbSub",      "Talk Box Sub Octave", 0.f, 100.f, 30.f, 0.f, 1.f, "%");
    addF (l, "tbCarTone",  "Talk Box Synth Brightness", 500.f, 16000.f, 9000.f, 3000.f, 10.f, "Hz");
    addF (l, "tbSharp",    "Talk Box Vowel Sharpness", 0.f, 100.f, 60.f, 0.f, 1.f, "%");
    addF (l, "tbShift",    "Talk Box Mouth Size", -6.f, 6.f, 0.f, 0.f, 0.1f, "st");
    addF (l, "tbPresence", "Talk Box Presence", 0.f, 100.f, 60.f, 0.f, 1.f, "%");
    addF (l, "tbCons",     "Talk Box Consonants", 0.f, 100.f, 60.f, 0.f, 1.f, "%");
    addF (l, "tbDyn",      "Talk Box Follow Loudness", 0.f, 100.f, 50.f, 0.f, 1.f, "%");
    addF (l, "tbGate",     "Talk Box Gate", -80.f, -20.f, -52.f, 0.f, 0.5f, "dB");
    addF (l, "tbDrive",    "Talk Box Drive", 0.f, 100.f, 25.f, 0.f, 1.f, "%");
    addF (l, "tbTone",     "Talk Box Tone", 1000.f, 16000.f, 7000.f, 4000.f, 10.f, "Hz");
    addF (l, "tbMix",      "Talk Box Mix", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addF (l, "tbLevel",    "Talk Box Level", -24.f, 12.f, 0.f, 0.f, 0.1f, "dB");
    addF (l, "tbSnap",     "Talk Box Snap To Scale", 0.f, 100.f, 100.f, 0.f, 1.f, "%");
    addI (l, "tbFixed",    "Talk Box Fixed Note", 24, 96, 60);
    addB (l, "tbLatch",    "Talk Box Latch Keys", false);
    addC (l, "tbPart",     "Talk Box Compose Part", { "Melody", "Synth / arp", "Bass" }, 0);
    addB (l, "tbMuteSyn",  "Talk Box Mute Compose Synth", true);

    return l;
}
} // namespace aio
