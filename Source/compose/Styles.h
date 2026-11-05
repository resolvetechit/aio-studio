#pragma once
#include "Composer.h"

namespace aio
{
// ---------------------------------------------------------------------------------------------
// Genre / style presets. Every preset is a *starting point* (tempo range, scale, chord rhythm,
// drum / bass / synth pattern family) written from common conventions of the genre. Nothing is
// copied from existing songs.
// ---------------------------------------------------------------------------------------------
enum DrumFamily  { DrumPopRock = 0, DrumFourFloor, DrumBoomBap, DrumTrap, DrumReggae, DrumDembow, DrumAfro,
                   DrumDnB, DrumFunk, DrumBallad, DrumAmbient, kNumDrumFamilies };
enum BassFamily  { BassSustain = 0, BassPulse8, BassOffbeat, BassSub808, BassFunk, BassReggae, BassRolling,
                   BassDnB, BassAfro, BassDembow, kNumBassFamilies };
enum SynthFamily { SynArp = 0, SynPad, SynStabs, SynSequence, SynBells, SynGated, kNumSynthFamilies };

struct StyleDef
{
    const char* name;
    int bpmLo, bpmHi;
    int scale;                 // index into scaleDefs()
    int prog[3];               // preferred progressions (index into progDefs())
    int chordRhythm, chordKind, voicing;
    int melodyStyle;
    float melodyDensity, melodyRests, swing;
    int drum, bass, synth;     // pattern families
};

const std::vector<StyleDef>& styleDefs();
inline int numStyles() { return (int) styleDefs().size(); }
const char* drumFamilyName (int fam);
const char* synthFamilyName (int fam);

// Tempo "feel": the only way the host BPM reaches the generators. A handful of flags, so nudging the
// tempo does not reshuffle a pattern the user is happy with.
struct TempoFeel
{
    bool slow = false;     // well below the style's range  -> busier subdivisions
    bool fast = false;     // well above the style's range  -> half-time drums, calmer arps
    bool high = false;     // >= 150 BPM                    -> 8th-note hats / arps
    bool low  = false;     // <= 85 BPM                     -> room for 16ths, denser melody
    int key() const { return (slow ? 1 : 0) | (fast ? 2 : 0) | (high ? 4 : 0) | (low ? 8 : 0); }
};
TempoFeel tempoFeelFor (int style, double bpm);
std::string tempoHint (int style, double bpm);           // one line for the UI

// ---- part generators (called by generatePattern)
void genDrums (std::vector<NoteEvent>& out, const ComposeSettings& s, double lengthBeats, const TempoFeel& feel, int family);
void genStyleBass (std::vector<NoteEvent>& out, const std::vector<ChordInfo>& chords, int family, const TempoFeel& feel, uint32_t seed, int vel);
void genSynthPart (std::vector<NoteEvent>& out, const std::vector<ChordInfo>& chords, int family, const ComposeSettings& s, const TempoFeel& feel);

// GM drum lanes used by the grid editor and the generator (index -> MIDI note)
constexpr int kDrumLanes = 8;
const char* drumLaneName (int lane);
int  drumLaneNote (int lane);
int  drumLaneForNote (int note);         // nearest lane for any GM drum note

// ---- Klimper-style chord palette ---------------------------------------------------------------
struct PaletteChord
{
    Chord chord;
    std::string roman;      // "IV", "bVII", "V7/vi" ...
    int group = 0;          // 0 diatonic, 1 borrowed from another mode, 2 special (secondary dominants, dim7 ...)
    int func = 0;           // functional degree 0..6 used for the "related chords" highlight
    int target = -1;        // group 2: degree it resolves to
};
std::vector<PaletteChord> buildPalette (int key, int scaleIdx, int kind);
int functionOfChord (int key, int scaleIdx, int rootPc);             // 0..6, nearest scale degree
int relatedness (int fromFunc, const PaletteChord& to);              // 0 none, 1 possible, 2 strong follow-up
CustomChord toCustom (const Chord&, double len);

// Find chords that fit a melody (one chord per segment); empty melody -> empty result.
std::vector<CustomChord> harmoniseMelody (const std::vector<NoteEvent>& melody, double lengthBeats, double segLen,
                                          int key, int scaleIdx, int kind);

// ---- (de)serialisation for plug-in state
std::string serializeNotes (const std::vector<NoteEvent>&);
std::vector<NoteEvent> deserializeNotes (const std::string&, int part);
std::string serializeChords (const std::vector<CustomChord>&);
std::vector<CustomChord> deserializeChords (const std::string&);
} // namespace aio
