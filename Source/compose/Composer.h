#pragma once
#include "Theory.h"

namespace aio
{
enum Part { PartChords = 0, PartMelody = 1, PartBass = 2 };

struct NoteEvent
{
    double start = 0, len = 1;     // in beats (quarter notes)
    int pitch = 60, vel = 90, part = 0;
};

struct ChordInfo
{
    double start = 0, len = 4;
    std::string name;
    int rootPc = 0;
    std::vector<int> pcs;
};

struct Pattern
{
    double lengthBeats = 16.0;
    std::vector<NoteEvent> notes;      // sorted by start
    std::vector<ChordInfo> chords;
};

struct ComposeSettings
{
    int key = 0, scale = 0, progression = 0;
    int bars = 1;            // index into {2,4,8,16}
    int chordLen = 1;        // index into {1/2 bar, 1 bar, 2 bars}
    int chordKind = 0, voicing = 0, chordRhythm = 0, chordOct = 3, chordVel = 85;
    bool smoothVoicing = true;
    float humanize = 0.2f, swing = 0.f;

    bool  melodyOn = true;
    float melodyDensity = 0.5f, melodyRests = 0.25f;
    int   melodyStyle = 0, melodyRange = 2, melodyOct = 4, melodyVel = 100;

    bool bassOn = true;
    int  bassStyle = 0;

    uint32_t chordSeed = 1, melodySeed = 1;
};

inline int barsFromIndex (int i)        { static const int t[4] = { 2, 4, 8, 16 }; return t[std::clamp (i, 0, 3)]; }
inline double chordLenFromIndex (int i) { static const double t[3] = { 2.0, 4.0, 8.0 }; return t[std::clamp (i, 0, 2)]; }

Pattern generatePattern (const ComposeSettings&);

// Option lists used by both the parameter layout and the tests.
inline const char* const* chordRhythmNames (int& count)
{
    static const char* n[] = { "Sustain", "Half notes", "Quarter pulse", "Offbeat stabs", "Tresillo",
                               "Arp Up", "Arp Down", "Arp Up-Down", "Arp 16ths", "Strum" };
    count = 10; return n;
}
} // namespace aio
