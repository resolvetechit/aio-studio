#pragma once
#include <vector>
#include <memory>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <atomic>

/*
   Vocal note editor engine (Melodyne-style, monophonic).

   analysePitch()   : offline pitch tracker (YIN candidates + Viterbi so octave jumps are rare)
   segmentNotes()   : cuts the pitch track into notes ("blobs"), each with its own pitch curve
   applyAuto()      : the one-click corrections (snap to scale, flatten drift, keep / reduce vibrato,
                      quantise timing, even out note levels)
   edit helpers     : move / stretch / split / merge / reset, always keeping notes in order
   renderVocal()    : pitch-synchronous overlap-add re-synthesis of the whole clip from the edited notes
                      (pitch, drift, vibrato, formant, level, timing). Voiceless parts (breaths, S, T, K) are
                      carried through with plain OLA so they stay natural.
*/
namespace aio
{
struct PitchTrack
{
    double sr = 44100.0;
    int hop = 256;                      // samples between frames; frame i is centred on sample i * hop
    std::vector<float> midi;            // < 0 : not voiced
    std::vector<float> levelDb;         // short-time RMS in dB
    int numFrames() const { return (int) midi.size(); }
};

struct AnalysisOptions
{
    float minHz = 70.f, maxHz = 900.f;
    float gateDb = -62.f;               // frames quieter than this are never voiced
};

PitchTrack analysePitch (const float* x, size_t n, double sr, const AnalysisOptions& = {},
                         const std::atomic<bool>* cancel = nullptr, std::atomic<float>* progress = nullptr);

// Per-note pitch detail, shared (immutable) between undo snapshots.
struct NoteCurve
{
    int f0 = 0;                         // first track frame
    std::vector<float> midi;            // filled-in pitch of every frame of the note
    std::vector<float> slow;            // deviation from the note centre that changes slowly ("drift")
    std::vector<float> fast;            // the rest ("vibrato" and wobble)
};

struct VocalNote
{
    double t0 = 0, t1 = 0;              // original span, seconds
    double nt0 = 0, nt1 = 0;            // edited span, seconds
    float centre = 60.f;                // original pitch centre (midi, fractional)
    float level = -40.f;                // mean level, dB
    std::shared_ptr<const NoteCurve> curve;

    float target = 60.f;                // edited pitch centre (midi, fractional)
    float drift = 1.f;                  // 1 = natural, 0 = flat
    float vib = 1.f;                    // 1 = natural, 0 = none, 2 = double
    float formant = 0.f;                // semitones
    float gainDb = 0.f;
    bool  muted = false;

    double origLen() const { return t1 - t0; }
    double newLen() const  { return nt1 - nt0; }
    bool pitchEdited() const { return std::abs (target - centre) > 0.005f || std::abs (drift - 1.f) > 0.005f || std::abs (vib - 1.f) > 0.005f; }
    bool timeEdited() const  { return std::abs (nt0 - t0) > 0.0005 || std::abs (nt1 - t1) > 0.0005; }
    bool edited() const { return pitchEdited() || timeEdited() || std::abs (formant) > 0.005f || std::abs (gainDb) > 0.05f || muted; }
};

struct SegmentOptions
{
    int   minFrames = 6;                // shortest note
    int   shortNote = 11;               // pieces shorter than this (scoops, glide fragments) are merged into their neighbour
    int   maxGap = 4;                   // unvoiced frames bridged inside a note
    float splitSemis = 0.85f;           // pitch change that starts a new note
    float dipDb = 8.f;                  // level dip that splits two notes sung on the same pitch
};

std::vector<VocalNote> segmentNotes (const PitchTrack&, const SegmentOptions& = {});

struct AutoSettings
{
    uint16_t mask = 0x0FFF;             // allowed pitch classes (bit 0 = C)
    float correct = 1.f;                // 0..1 how far each note moves to the nearest allowed pitch
    float flatten = 0.5f;               // 0..1 removes pitch drift inside the note
    float vibrato = 1.f;                // 0..2 vibrato depth (1 = leave alone)
    float glideMs = 15.f;               // transition time between notes (render option, kept here for the UI)
    float timing = 0.f;                 // 0..1 quantise strength
    int   gridPerBeat = 4;              // 1 = quarter notes, 2 = eighths, 4 = sixteenths, 8 = 32nds
    double bpm = 120.0;
    double gridOffsetSec = 0.0;         // time of a beat line inside the clip
    float level = 0.f;                  // 0..1 evens out note loudness
};

// resets the notes selected by `scope` (empty = all) to the analysis, then applies the settings
void applyAuto (std::vector<VocalNote>& notes, const AutoSettings&, const std::vector<char>* scope = nullptr);
int  nearestAllowedMidi (double midi, uint16_t mask);

// ---- editing helpers (all keep the notes ordered and non-overlapping) -----------------------------------------
double newToOrig (const VocalNote&, double tNew);
double origToNew (const VocalNote&, double tOrig);
void   allowedRange (const std::vector<VocalNote>&, int i, double& lo, double& hi, double maxTime);
void   moveNote (std::vector<VocalNote>&, int i, double newStart, double maxTime);
void   setNoteStart (std::vector<VocalNote>&, int i, double t, double maxTime);
void   setNoteEnd (std::vector<VocalNote>&, int i, double t, double maxTime);
bool   splitNote (std::vector<VocalNote>&, int i, double tNew, double sr, int hop);
bool   mergeWithNext (std::vector<VocalNote>&, int i);
void   resetNote (std::vector<VocalNote>&, int i, double maxTime);
double editedLength (const std::vector<VocalNote>&, double clipSeconds);
int    noteAtNewTime (const std::vector<VocalNote>&, double t);

struct RenderOptions
{
    float glideMs = 15.f;
    float globalFormant = 0.f;
};

// Re-synthesises the clip. Returns an empty vector when cancelled.
std::vector<float> renderVocal (const std::vector<float>& src, double sr, int hop, const std::vector<VocalNote>& notes,
                                const RenderOptions& = {}, const std::atomic<bool>* cancel = nullptr);

std::vector<float> resampleSinc (const std::vector<float>& in, double srIn, double srOut);

// ---- persistence ---------------------------------------------------------------------------------------------
std::vector<uint8_t> serialiseNotes (const std::vector<VocalNote>&, double sr, int hop);
bool deserialiseNotes (const uint8_t* data, size_t size, std::vector<VocalNote>& out, double& sr, int& hop);

// ---- document with undo ------------------------------------------------------------------------------------------
struct VocalDoc
{
    double sr = 44100.0;
    int hop = 256;
    double clipSeconds = 0.0;
    std::vector<VocalNote> notes;
    std::vector<std::vector<VocalNote>> undoStack, redoStack;

    void checkpoint()
    {
        undoStack.push_back (notes);
        if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
        redoStack.clear();
    }
    bool undo()  { if (undoStack.empty()) return false; redoStack.push_back (notes); notes = undoStack.back(); undoStack.pop_back(); return true; }
    bool redo()  { if (redoStack.empty()) return false; undoStack.push_back (notes); notes = redoStack.back(); redoStack.pop_back(); return true; }
    void clearHistory() { undoStack.clear(); redoStack.clear(); }
};
} // namespace aio
