#include "Styles.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace aio
{
// =================================================================================================
// Style table
// =================================================================================================
const std::vector<StyleDef>& styleDefs()
{
    // name, bpmLo, bpmHi, scale, prog[3], chordRhythm, chordKind, voicing, melStyle, dens, rests, swing, drum, bass, synth
    static const std::vector<StyleDef> v = {
        { "Pop",                  100, 128, 0, { 0, 1, 2 },    2, 0, 0, 2, 0.50f, 0.25f, 0.00f, DrumPopRock,   BassPulse8,  SynArp },
        { "Rock",                 110, 150, 5, { 7, 8, 0 },    2, 0, 0, 1, 0.50f, 0.25f, 0.00f, DrumPopRock,   BassPulse8,  SynPad },
        { "Indie / Alt",          100, 135, 0, { 10, 13, 0 },  4, 5, 0, 0, 0.40f, 0.30f, 0.00f, DrumPopRock,   BassPulse8,  SynBells },
        { "Ballad / Wedding",      60,  85, 0, { 11, 0, 13 },  7, 0, 1, 0, 0.35f, 0.30f, 0.00f, DrumBallad,    BassSustain, SynPad },
        { "Lo-fi Hip-Hop",         70,  90, 2, { 3, 12, 9 },   3, 2, 1, 0, 0.30f, 0.40f, 0.35f, DrumBoomBap,   BassSub808,  SynBells },
        { "Boom Bap Hip-Hop",      80, 100, 1, { 9, 6, 4 },    3, 1, 1, 2, 0.40f, 0.35f, 0.20f, DrumBoomBap,   BassSub808,  SynBells },
        { "Trap",                 130, 170, 1, { 4, 6, 9 },    0, 1, 1, 2, 0.60f, 0.25f, 0.00f, DrumTrap,      BassSub808,  SynBells },
        { "R&B / Slow Jam",        60,  95, 2, { 3, 12, 1 },   4, 2, 1, 0, 0.45f, 0.30f, 0.15f, DrumBoomBap,   BassSub808,  SynPad },
        { "House",                118, 128, 1, { 1, 10, 6 },   3, 1, 0, 3, 0.55f, 0.25f, 0.05f, DrumFourFloor, BassOffbeat, SynStabs },
        { "Deep House",           118, 124, 2, { 3, 12, 10 },  3, 2, 1, 0, 0.35f, 0.40f, 0.12f, DrumFourFloor, BassOffbeat, SynPad },
        { "Techno",               125, 140, 3, { 9, 6, 12 },   0, 0, 0, 3, 0.50f, 0.30f, 0.00f, DrumFourFloor, BassRolling, SynSequence },
        { "Trance",               130, 145, 1, { 4, 1, 6 },    8, 0, 0, 2, 0.60f, 0.20f, 0.00f, DrumFourFloor, BassRolling, SynGated },
        { "Drum & Bass",          168, 180, 1, { 6, 9, 4 },    1, 1, 0, 0, 0.45f, 0.35f, 0.00f, DrumDnB,       BassDnB,     SynPad },
        { "Afrobeats",             95, 115, 2, { 0, 10, 13 },  4, 1, 0, 2, 0.55f, 0.25f, 0.10f, DrumAfro,      BassAfro,    SynBells },
        { "Reggaeton",             85, 100, 1, { 5, 6, 9 },    4, 0, 0, 2, 0.55f, 0.25f, 0.00f, DrumDembow,    BassDembow,  SynBells },
        { "Reggae",                70,  90, 0, { 7, 0, 13 },   3, 0, 0, 0, 0.40f, 0.35f, 0.10f, DrumReggae,    BassReggae,  SynStabs },
        { "Funk / Disco",         100, 125, 5, { 7, 12, 3 },   3, 1, 0, 1, 0.55f, 0.25f, 0.10f, DrumFunk,      BassFunk,    SynStabs },
        { "Synthwave",             95, 120, 1, { 4, 6, 1 },    0, 0, 0, 2, 0.50f, 0.25f, 0.00f, DrumPopRock,   BassPulse8,  SynArp },
        { "Ambient / Cinematic",   60,  90, 4, { 13, 1, 11 },  0, 2, 2, 0, 0.20f, 0.50f, 0.00f, DrumAmbient,   BassSustain, SynPad },
    };
    return v;
}

const char* drumFamilyName (int f)
{
    static const char* n[kNumDrumFamilies] = { "Pop / Rock", "Four on the floor", "Boom bap", "Trap (half-time)", "Reggae one-drop",
                                               "Dembow", "Afro", "Drum & Bass", "Funk", "Ballad", "Ambient" };
    return n[std::clamp (f, 0, (int) kNumDrumFamilies - 1)];
}

const char* synthFamilyName (int f)
{
    static const char* n[kNumSynthFamilies] = { "Arpeggio", "Pad", "Stabs", "Sequence", "Bells", "Gated pad" };
    return n[std::clamp (f, 0, (int) kNumSynthFamilies - 1)];
}

TempoFeel tempoFeelFor (int style, double bpm)
{
    const auto& d = styleDefs()[(size_t) std::clamp (style, 0, numStyles() - 1)];
    TempoFeel f;
    f.slow = bpm < d.bpmLo * 0.8;
    f.fast = bpm > d.bpmHi * 1.25;
    f.high = bpm >= 150.0;
    f.low  = bpm <= 85.0;
    return f;
}

std::string tempoHint (int style, double bpm)
{
    const auto& d = styleDefs()[(size_t) std::clamp (style, 0, numStyles() - 1)];
    const auto f = tempoFeelFor (style, bpm);
    char buf[200];
    const char* verdict = f.slow ? "slower than usual: busier hats and arps"
                        : f.fast ? "faster than usual: half-time drums, calmer arps"
                        : (bpm >= d.bpmLo && bpm <= d.bpmHi) ? "in the pocket"
                                                             : "close to the range";
    std::snprintf (buf, sizeof (buf), "%s usually sits at %d-%d BPM.  Host %.0f BPM: %s", d.name, d.bpmLo, d.bpmHi, bpm, verdict);
    return buf;
}

// =================================================================================================
// Drums
// =================================================================================================
namespace
{
enum Lane { Kick = 0, Snare, Clap, CHat, OHat, Rim, Tom, Crash };

struct DFam { const char *k, *s, *c, *ch, *oh, *rim; int hatBase; bool ghost; bool halfOK; bool gentle; const char *k2, *k3; };

// 16 steps per bar, 'x' hit, 'o' accent, 'g' ghost, '.' empty
const DFam kFams[kNumDrumFamilies] = {
    /* PopRock   */ { "x.......x.x.....", "....x.......x...", "",                 "x.x.x.x.x.x.x.x.", "",                 "",                 64, true,  true,  false , "x.x.....x.x.....", "x..x....x..x...." },
    /* FourFloor */ { "x...x...x...x...", "",                "....x.......x...", "xxxxxxxxxxxxxxxx", "..x...x...x...x.", "",                 52, false, false, false , "x...x...x...x...", "x...x...x...x..." },
    /* BoomBap   */ { "x.....x...x.....", "....x.......x...", "",                 "x.x.x.x.x.x.x.x.", "",                 "",                 62, true,  true,  false , "x.......x.x.x...", "x..x....x.....x." },
    /* Trap      */ { "x.......x.....x.", "........x.......", "........x.......", "x.x.x.x.x.x.x.x.", "......x.........", "",                 66, false, false, false , "x..x....x.x.....", "x.......x..x..x." },
    /* Reggae    */ { "........x.......", "",                "",                 "x.x.x.x.x.x.x.x.", "",                 "........x.......", 56, false, false, true  , "........x.......", "........x......." },
    /* Dembow    */ { "x...x...x...x...", "...x..x....x..x.", "",                 "x.x.x.x.x.x.x.x.", "",                 "",                 62, false, false, false , "x...x...x...x...", "x...x...x...x..." },
    /* Afro      */ { "x..x..x...x.....", "",                "....x.......x...", "x.x.x.x.x.x.x.x.", "",                 "..x..x..x..x..x.", 58, false, false, false , "x..x..x...x..x..", "x.....x..x.x...." },
    /* DnB       */ { "x.........x.....", "....x.......x...", "",                 "x.x.x.x.x.x.x.x.", "..............x.", "",                 60, true,  false, false , "x..x......x.....", "x.........x..x.." },
    /* Funk      */ { "x..x..x...x..x..", "....x.......x...", "",                 "xxxxxxxxxxxxxxxx", "......x.......x.", "",                 56, true,  true,  false , "x..x....x..x..x.", "x.x...x...x..x.." },
    /* Ballad    */ { "x.......x.......", "....x.......x...", "",                 "x.x.x.x.x.x.x.x.", "",                 "",                 50, false, true,  true  , "x.......x.x.....", "x..x....x......." },
    /* Ambient   */ { "x...............", "",                "",                 "x.x.x.x.x.x.x.x.", "",                 "........x.......", 40, false, false, true  , "x.......x.......", "x..............." },
};

constexpr int kLaneNotes[kDrumLanes] = { 36, 38, 39, 42, 46, 37, 47, 49 };
} // namespace

const char* drumLaneName (int lane)
{
    static const char* n[kDrumLanes] = { "Kick", "Snare", "Clap", "Hat", "Open hat", "Rim / perc", "Tom", "Crash" };
    return n[std::clamp (lane, 0, kDrumLanes - 1)];
}

int drumLaneNote (int lane) { return kLaneNotes[std::clamp (lane, 0, kDrumLanes - 1)]; }

int drumLaneForNote (int note)
{
    switch (note)
    {
        case 35: case 36: return Kick;
        case 38: case 40: return Snare;
        case 39: return Clap;
        case 42: case 44: return CHat;
        case 46: return OHat;
        case 41: case 43: case 45: case 47: case 48: case 50: return Tom;
        case 49: case 51: case 52: case 55: case 57: return Crash;
        default: return Rim;
    }
}

void genDrums (std::vector<NoteEvent>& out, const ComposeSettings& s, double lengthBeats, const TempoFeel& feel, int family)
{
    family = std::clamp (family, 0, (int) kNumDrumFamilies - 1);
    const DFam& f = kFams[family];
    Rng rng (s.drumSeed * 2246822519u + 3u);
    const int nBars = std::max (1, (int) std::lround (lengthBeats / 4.0));
    const float busy = std::clamp (s.drumBusy, 0.f, 1.f);
    const bool half = feel.fast && f.halfOK;
    const bool eighthHats = feel.high && family != DrumDnB && family != DrumTrap;
    const double velScale = s.drumVel / 100.0;
    bool prevFill = false;
    const int kickSet = rng.range (3);                 // which kick pattern this idea is built on

    for (int bar = 0; bar < nBars; ++bar)
    {
        const char* kickStr = f.k;
        {
            const int ks = (bar > 0 && rng.chance (0.15f)) ? rng.range (3) : kickSet;
            if (ks == 1) kickStr = f.k2; else if (ks == 2) kickStr = f.k3;
        }
        int g[kDrumLanes][16] = {};
        int tomPitch[16];
        for (int& t : tomPitch) t = 47;

        auto setLane = [&] (int lane, const char* str)
        {
            if (*str == 0) return;                      // empty lane
            for (int i = 0; i < 16; ++i)
            {
                const char c = str[i];
                g[lane][i] = c == 'x' ? 100 : c == 'o' ? 118 : c == 'g' ? 45 : 0;
            }
        };
        setLane (Kick, kickStr); setLane (Snare, f.s); setLane (Clap, f.c); setLane (OHat, f.oh); setLane (Rim, f.rim);
        for (int i = 0; i < 16; ++i)
            if (f.ch[i] != '.')
            {
                int v = f.hatBase + (i % 4 == 0 ? 20 : (i % 2 == 0 ? 9 : 0)) + rng.range (9) - 4;
                if (i % 2 == 1) v -= 10;
                g[CHat][i] = std::max (20, v);
            }

        // open hats replace the closed hat on the same step
        for (int i = 0; i < 16; ++i) if (g[OHat][i] > 0) g[CHat][i] = 0;

        // ---- tempo feel
        if (half)
        {
            const bool hadSnare = f.s[0] != 0, hadClap = f.c[0] != 0;
            for (int i = 0; i < 16; ++i) { g[Snare][i] = 0; g[Clap][i] = 0; }
            if (hadSnare) g[Snare][8] = 100;
            if (hadClap)  g[Clap][8] = 100;
            g[Kick][8] = 0; g[Kick][0] = 100;
        }
        if (eighthHats)
            for (int i = 1; i < 16; i += 2) g[CHat][i] = 0;
        if (feel.slow && (family == DrumPopRock || family == DrumBoomBap || family == DrumBallad))
            for (int i = 1; i < 16; i += 2) if (g[CHat][i] == 0 && g[OHat][i] == 0) g[CHat][i] = std::max (24, f.hatBase - 26 + rng.range (9));

        // ---- "busy" variations
        const bool kickVar = family == DrumPopRock || family == DrumBoomBap || family == DrumTrap || family == DrumFunk
                          || family == DrumDnB || family == DrumAfro;
        if (kickVar)
        {
            static const int cand[7] = { 3, 6, 7, 10, 11, 14, 15 };
            int extra = std::min (half ? 1 : 2, (int) (busy * 2.2f + rng.uni()));
            for (int tries = 0; extra > 0 && tries < 10; ++tries)
            {
                const int i = cand[rng.range (7)];
                if (g[Kick][i] || g[Snare][i] || g[Clap][i]) continue;
                if ((i > 0 && g[Kick][i - 1]) || (i < 15 && g[Kick][i + 1])) continue;
                g[Kick][i] = 90; --extra;
            }
        }
        if (f.ghost && ! half && busy > 0.f)
        {
            static const int gs[5] = { 3, 7, 9, 11, 15 };
            for (int i : gs)
                if (! g[Snare][i] && ! g[Kick][i] && rng.chance (busy * 0.45f)) g[Snare][i] = 36 + rng.range (14);
        }
        if (family == DrumFourFloor || family == DrumAfro || family == DrumAmbient)
        {
            static const int ps[6] = { 2, 5, 7, 10, 13, 14 };
            for (int i : ps)
                if (! g[Rim][i] && ! g[Kick][i] && rng.chance (busy * 0.25f)) g[Rim][i] = 50 + rng.range (22);
        }
        if ((family == DrumPopRock || family == DrumBoomBap || family == DrumTrap || family == DrumFunk) && rng.chance (busy * 0.2f))
        { g[OHat][14] = 78; g[CHat][14] = 0; g[CHat][15] = 0; }
        if (family == DrumTrap && busy > 0.3f)                                   // hat rolls
            for (int beat : { 1, 3 })
                if (rng.chance (busy * 0.7f))
                    for (int k = 0; k < 4; ++k) g[CHat][beat * 4 + k] = 48 + k * 11;

        // ---- fills on phrase ends
        const bool lastBar = bar == nBars - 1;
        float fillP = (lastBar && nBars > 1) ? 0.9f : ((bar % 4) == 3 ? 0.5f : 0.f);
        fillP *= (0.4f + busy * 0.6f) * (f.gentle ? 0.5f : 1.f);
        const bool doFill = rng.chance (fillP);
        if (doFill)
        {
            if (f.gentle)
            {
                g[Tom][14] = 80; g[Tom][15] = 70; tomPitch[14] = 45; tomPitch[15] = 43;
            }
            else
            {
                const int type = rng.range (3);
                const int from = (family == DrumFourFloor && rng.chance (0.5f)) ? 8 : 12;
                auto clear = [&] (int i)
                {
                    for (int l : { (int) Snare, (int) Clap, (int) CHat, (int) OHat, (int) Rim, (int) Tom }) g[l][i] = 0;
                };
                if (type == 0)                                                    // snare roll
                    for (int i = from; i < 16; ++i)
                    {
                        clear (i); if (i >= 12) g[Kick][i] = 0;
                        g[Snare][i] = 56 + (i - from) * (60 / std::max (1, 15 - from));
                    }
                else if (type == 1)                                               // tom run
                {
                    static const int tp[4] = { 50, 47, 45, 43 };
                    for (int k = 0; k < 4; ++k)
                    {
                        const int i = 12 + k;
                        clear (i); g[Kick][i] = 0;
                        g[Tom][i] = 92 + k * 5; tomPitch[i] = tp[k];
                    }
                }
                else                                                              // stop-time pickup
                {
                    for (int i = 13; i < 16; ++i) { clear (i); g[Kick][i] = 0; }
                    g[Snare][15] = 105;
                }
            }
        }

        // ---- crashes
        if ((bar == 0 && rng.chance (0.7f)) || (bar > 0 && bar % 8 == 0 && rng.chance (0.6f)) || (prevFill && rng.chance (0.5f)))
            g[Crash][0] = 105;
        prevFill = doFill;

        // ---- emit
        for (int lane = 0; lane < kDrumLanes; ++lane)
            for (int i = 0; i < 16; ++i)
            {
                if (g[lane][i] <= 0) continue;
                NoteEvent n;
                n.start = bar * 4.0 + i * 0.25;
                n.len = lane == OHat ? 0.4 : (lane == Crash ? 1.0 : 0.12);
                n.pitch = lane == Tom ? tomPitch[i] : kLaneNotes[lane];
                n.vel = std::clamp ((int) std::lround (g[lane][i] * velScale), 1, 127);
                n.part = PartDrums;
                out.push_back (n);
            }
    }
}

// =================================================================================================
// Bass
// =================================================================================================
namespace
{
struct BStep { float off, dur; int tone, vel; };   // tone: 0 root, 1 octave, 2 fifth, 3 third, 4 flat seventh, 5 approach note
struct BTemplate { const BStep* a; int na; const BStep* b; int nb; const BStep* c; int nc; };

const BStep kSusA[] = { { 0, 3.9f, 0, 100 } };
const BStep kSusB[] = { { 0, 3.4f, 0, 100 }, { 3.5f, 0.45f, 2, 80 } };
const BStep kP8A[]  = { { 0, .45f, 0, 105 }, { .5f, .45f, 0, 86 }, { 1, .45f, 0, 95 }, { 1.5f, .45f, 0, 86 },
                        { 2, .45f, 0, 98 }, { 2.5f, .45f, 0, 86 }, { 3, .45f, 0, 95 }, { 3.5f, .45f, 0, 86 } };
const BStep kP8B[]  = { { 0, .45f, 0, 105 }, { .5f, .45f, 0, 86 }, { 1, .45f, 0, 95 }, { 1.5f, .45f, 0, 86 },
                        { 2, .45f, 0, 98 }, { 2.5f, .45f, 0, 86 }, { 3, .45f, 0, 95 }, { 3.5f, .45f, 5, 90 } };
const BStep kOffA[] = { { .5f, .45f, 0, 100 }, { 1.5f, .45f, 0, 100 }, { 2.5f, .45f, 0, 100 }, { 3.5f, .45f, 0, 100 } };
const BStep kOffB[] = { { .5f, .45f, 0, 100 }, { 1.5f, .45f, 0, 100 }, { 2.5f, .45f, 0, 100 }, { 3.25f, .2f, 0, 80 }, { 3.5f, .45f, 1, 100 } };
const BStep kSubA[] = { { 0, 1.5f, 0, 112 }, { 1.5f, .5f, 0, 92 }, { 2.5f, 1.f, 0, 104 }, { 3.5f, .5f, 1, 88 } };
const BStep kSubB[] = { { 0, 1.f, 0, 112 }, { 1, .5f, 0, 84 }, { 2, .75f, 0, 104 }, { 2.75f, .25f, 0, 90 }, { 3, 1.f, 2, 98 } };
const BStep kFunkA[] = { { 0, .5f, 0, 105 }, { .75f, .25f, 0, 82 }, { 1.5f, .5f, 1, 96 }, { 2, .5f, 0, 100 }, { 2.75f, .25f, 4, 82 },
                         { 3, .5f, 2, 92 }, { 3.5f, .5f, 3, 86 } };
const BStep kFunkB[] = { { 0, .5f, 0, 105 }, { .75f, .25f, 0, 80 }, { 1.5f, .5f, 1, 96 }, { 2, .5f, 0, 100 }, { 3, .25f, 2, 90 },
                         { 3.25f, .25f, 3, 88 }, { 3.5f, .25f, 4, 88 }, { 3.75f, .25f, 5, 92 } };
const BStep kRegA[] = { { 1.5f, .5f, 0, 92 }, { 2, 1.f, 0, 108 }, { 3.5f, .5f, 2, 84 } };
const BStep kRegB[] = { { .5f, .5f, 0, 88 }, { 2, 1.f, 0, 108 }, { 3, .5f, 4, 84 }, { 3.5f, .5f, 2, 88 } };
const BStep kRollA[] = { { .25f, .2f, 0, 96 }, { .5f, .2f, 0, 90 }, { .75f, .2f, 0, 92 }, { 1.25f, .2f, 0, 96 }, { 1.5f, .2f, 0, 90 }, { 1.75f, .2f, 0, 92 },
                         { 2.25f, .2f, 0, 96 }, { 2.5f, .2f, 0, 90 }, { 2.75f, .2f, 0, 92 }, { 3.25f, .2f, 0, 96 }, { 3.5f, .2f, 0, 90 }, { 3.75f, .2f, 0, 92 } };
const BStep kRollB[] = { { .25f, .2f, 0, 96 }, { .5f, .2f, 0, 90 }, { .75f, .2f, 0, 92 }, { 1.25f, .2f, 0, 96 }, { 1.5f, .2f, 0, 90 }, { 1.75f, .2f, 0, 92 },
                         { 2.25f, .2f, 0, 96 }, { 2.5f, .2f, 0, 90 }, { 2.75f, .2f, 0, 92 }, { 3.25f, .2f, 0, 96 }, { 3.5f, .2f, 1, 90 }, { 3.75f, .2f, 1, 92 } };
const BStep kDnbA[] = { { 0, .75f, 0, 108 }, { .75f, .25f, 0, 84 }, { 2.5f, .5f, 0, 104 }, { 3.25f, .25f, 1, 88 }, { 3.5f, .5f, 2, 88 } };
const BStep kDnbB[] = { { 0, .75f, 0, 108 }, { 1.5f, .5f, 0, 92 }, { 2.5f, .5f, 0, 104 }, { 3, .25f, 0, 84 }, { 3.5f, .5f, 4, 88 } };
const BStep kAfrA[] = { { 0, 1.f, 0, 102 }, { 1.5f, 1.f, 0, 92 }, { 3, .75f, 2, 94 } };
const BStep kAfrB[] = { { 0, 1.f, 0, 102 }, { 1.5f, .5f, 4, 86 }, { 2, 1.f, 0, 96 }, { 3, .5f, 2, 92 }, { 3.5f, .5f, 0, 84 } };
const BStep kDemA[] = { { 0, .9f, 0, 106 }, { 1.5f, .4f, 0, 86 }, { 2, .9f, 2, 100 }, { 3.5f, .4f, 0, 88 } };
const BStep kDemB[] = { { 0, .9f, 0, 106 }, { 1.5f, .4f, 2, 86 }, { 2, .9f, 0, 100 }, { 3, .4f, 0, 82 }, { 3.5f, .4f, 4, 88 } };

// third, alternative base pattern per family (a different "idea" for the same groove)
const BStep kSusC[]  = { { 0, 1.9f, 0, 100 }, { 2, 1.9f, 2, 86 } };
const BStep kP8C[]   = { { 0, .9f, 0, 104 }, { 1, .9f, 0, 92 }, { 2, .9f, 0, 98 }, { 3, .4f, 0, 92 }, { 3.5f, .4f, 5, 86 } };
const BStep kOffC[]  = { { .5f, .3f, 0, 100 }, { .75f, .2f, 0, 80 }, { 1.5f, .3f, 0, 100 }, { 2.5f, .3f, 0, 100 }, { 2.75f, .2f, 0, 80 }, { 3.5f, .3f, 1, 100 } };
const BStep kSubC[]  = { { 0, 2.f, 0, 112 }, { 2, 1.f, 0, 100 }, { 3, .5f, 2, 90 }, { 3.5f, .5f, 0, 84 } };
const BStep kFunkC[] = { { 0, .4f, 0, 105 }, { .5f, .25f, 0, 80 }, { 1, .4f, 2, 95 }, { 1.75f, .25f, 0, 84 }, { 2.5f, .5f, 4, 92 }, { 3, .4f, 0, 100 }, { 3.75f, .25f, 5, 90 } };
const BStep kRegC[]  = { { 0, .5f, 0, 84 }, { 1.5f, .5f, 0, 92 }, { 2, 1.5f, 0, 108 }, { 3.5f, .5f, 2, 86 } };

#define AIO_T(a, b, c) { a, (int) (sizeof (a) / sizeof (BStep)), b, (int) (sizeof (b) / sizeof (BStep)), c, (int) (sizeof (c) / sizeof (BStep)) }
const BTemplate kBass[kNumBassFamilies] = {
    AIO_T (kSusA, kSusB, kSusC), AIO_T (kP8A, kP8B, kP8C), AIO_T (kOffA, kOffB, kOffC), AIO_T (kSubA, kSubB, kSubC), AIO_T (kFunkA, kFunkB, kFunkC),
    AIO_T (kRegA, kRegB, kRegC), AIO_T (kRollA, kRollB, kRollA), AIO_T (kDnbA, kDnbB, kDnbA), AIO_T (kAfrA, kAfrB, kAfrA), AIO_T (kDemA, kDemB, kDemA)
};
#undef AIO_T

int bassRootOf (int pc) { int r = 36 + pc; if (pc > 7) r -= 12; return r; }
} // namespace

void genStyleBass (std::vector<NoteEvent>& out, const std::vector<ChordInfo>& chords, int family, const TempoFeel& feel, uint32_t seed, int vel)
{
    family = std::clamp (family, 0, (int) kNumBassFamilies - 1);
    if (family == BassRolling && feel.high) family = BassOffbeat;
    const BTemplate& T = kBass[family];
    Rng rng (seed * 2654435761u + 41u);
    const int baseSel = rng.range (3);                 // 0 / 1: the A pattern, 2: the alternative C pattern
    const float mutate = baseSel == 1 ? 0.28f : 0.14f;

    for (size_t ci = 0; ci < chords.size(); ++ci)
    {
        const auto& c = chords[ci];
        const int root = bassRootOf (c.rootPc);
        const int third = c.pcs.size() > 1 ? ((c.pcs[1] - c.rootPc + 12) % 12) : 4;
        const int nextPc = chords[(ci + 1) % chords.size()].rootPc;
        const int k0 = (int) std::floor (c.start / 4.0 + 1e-9);
        const int k1 = (int) std::ceil ((c.start + c.len) / 4.0 - 1e-9);
        for (int k = k0; k < k1; ++k)
        {
            const bool useB = (k % 4 == 3) || rng.chance (0.12f);
            const BStep* src = useB ? T.b : (baseSel == 2 ? T.c : T.a);
            const int n = useB ? T.nb : (baseSel == 2 ? T.nc : T.na);

            // per-bar variations: a few notes change (fifth / octave / flat seventh), off-beat notes drop out, a ghost note may appear
            std::vector<BStep> bar (src, src + n);
            for (size_t i = 1; i < bar.size(); ++i)
            {
                auto& b = bar[i];
                const bool onBeat = std::fmod (b.off, 1.0f) < 0.01f;
                if (b.tone != 5 && rng.chance (mutate)) { static const int tones[3] = { 2, 1, 4 }; b.tone = tones[rng.range (3)]; }
                if (! onBeat && rng.chance (mutate * 0.85f)) b.vel = 0;
            }
            if (rng.chance (0.3f))
            {
                static const float cand[4] = { 0.75f, 1.75f, 2.75f, 3.75f };
                const float off = cand[rng.range (4)];
                bool free = true;
                for (const auto& b : bar) if (std::abs (b.off - off) < 0.2f || (b.off < off && b.off + b.dur > off + 0.01f)) free = false;
                if (free) bar.push_back ({ off, 0.2f, 0, 74 });
            }
            const BStep* st = bar.data();
            for (int i = 0; i < (int) bar.size(); ++i)
            {
                if (st[i].vel <= 0) continue;
                const double abs = k * 4.0 + st[i].off;
                if (abs < c.start - 1e-6 || abs >= c.start + c.len - 1e-6) continue;
                int pitch = root;
                switch (st[i].tone)
                {
                    case 1: pitch = root + 12; break;
                    case 2: pitch = root + 7; break;
                    case 3: pitch = root + third; break;
                    case 4: pitch = root + 10; break;
                    case 5:
                    {
                        const int nr = bassRootOf (nextPc);
                        pitch = (nr == root) ? root + 7 : nr + (rng.chance (0.6f) ? -1 : 1);
                        break;
                    }
                    default: break;
                }
                NoteEvent ne;
                ne.start = abs;
                ne.len = std::min ((double) st[i].dur, c.start + c.len - abs);
                ne.pitch = std::clamp (pitch, 24, 60);
                ne.vel = std::clamp ((int) std::lround (st[i].vel * vel / 100.0), 1, 127);
                ne.part = PartBass;
                out.push_back (ne);
            }
        }
    }
}

// =================================================================================================
// Synth / arp part
// =================================================================================================
namespace
{
// every chord pitch class folded into [lowBase, lowBase + 12), ascending
std::vector<int> toneSet (const ChordInfo& c, int lowBase)
{
    std::vector<int> t;
    for (int pc : c.pcs) t.push_back (lowBase + (((pc - lowBase) % 12) + 12) % 12);
    std::sort (t.begin(), t.end());
    t.erase (std::unique (t.begin(), t.end()), t.end());
    return t;
}
} // namespace

void genSynthPart (std::vector<NoteEvent>& out, const std::vector<ChordInfo>& chords, int family, const ComposeSettings& s, const TempoFeel& feel)
{
    family = std::clamp (family, 0, (int) kNumSynthFamilies - 1);
    Rng rng (s.synthSeed * 2654435761u + 11u);
    const int lowBase = 12 * (s.synthOct + 1) - 5;
    const int vel = s.synthVel;
    const int variant = rng.range (4);
    const int flavour = rng.range (3);                 // second independent choice: octave span / inversion / gate
    char gmask[17];
    for (int i = 0; i < 16; ++i) gmask[i] = (i == 0 || rng.chance (0.62f)) ? 'x' : '.';
    gmask[16] = 0;

    auto add = [&] (double start, double len, int pitch, int v)
    {
        NoteEvent n; n.start = start; n.len = len; n.pitch = std::clamp (pitch, 0, 127); n.vel = std::clamp (v, 1, 127); n.part = PartSynth;
        out.push_back (n);
    };

    for (size_t c = 0; c < chords.size(); ++c)
    {
        const auto& ci = chords[c];
        auto tones = toneSet (ci, lowBase);
        if (tones.empty()) continue;
        if (family != SynArp && family != SynSequence && family != SynBells)
            for (int r = 0; r < flavour && tones.size() > 1; ++r) { tones.front() += 12; std::sort (tones.begin(), tones.end()); }   // inversion
        const double end = ci.start + ci.len;
        const int k0 = (int) std::floor (ci.start / 4.0 + 1e-9), k1 = (int) std::ceil (end / 4.0 - 1e-9);

        switch (family)
        {
            case SynArp:
            {
                const double step = (feel.low && ! feel.high) ? 0.25 : 0.5;
                std::vector<int> pool = tones;
                if (flavour > 0) for (int t : tones) pool.push_back (t + 12);          // one or two octaves
                const int m = (int) pool.size();
                const double gate = flavour == 2 ? 0.5 : 0.85;
                int i = 0;
                for (double t = 0; t < ci.len - 1e-6; t += step, ++i)
                {
                    int idx = 0;
                    switch (variant)
                    {
                        case 0: idx = i % m; break;
                        case 1: idx = (m - 1) - (i % m); break;
                        case 2: { const int cyc = std::max (1, 2 * m - 2), q = i % cyc; idx = q < m ? q : cyc - q; break; }
                        default: idx = i == 0 ? 0 : rng.range (m); break;
                    }
                    add (ci.start + t, std::min (step * gate, ci.len - t), pool[(size_t) idx], vel + (i % 4 == 0 ? 10 : 0) - (i % 2 ? 6 : 0));
                }
                break;
            }
            case SynPad:
            {
                // variant 0: one long chord, 1: re-struck halfway, 2: re-struck every 2 beats
                const double hit = variant % 3 == 0 ? ci.len : variant % 3 == 1 ? std::max (2.0, ci.len * 0.5) : 2.0;
                for (double t = 0; t < ci.len - 1e-6; t += hit)
                    for (size_t j = 0; j < tones.size(); ++j)
                        add (ci.start + t + 0.02 * (double) j, std::min (hit, ci.len - t) * 0.97, tones[j], vel);
                break;
            }
            case SynStabs:
            {
                static const std::vector<double> pat[4] = { { 0, 1.5, 3.0 }, { .5, 1.5, 2.5, 3.5 }, { 0, .75, 1.5, 2.25, 3.0 }, { 1.0, 2.5, 3.5 } };
                for (int k = k0; k < k1; ++k)
                {
                    const auto& p = pat[(variant + (k % 4 == 3 ? 1 : 0)) % 4];
                    for (double off : p)
                    {
                        const double abs = k * 4.0 + off;
                        if (abs < ci.start - 1e-6 || abs >= end - 1e-6) continue;
                        const int cnt = std::min<int> (4, (int) tones.size());
                        for (int j = 0; j < cnt; ++j) add (abs, std::min (flavour == 1 ? 0.2 : 0.3, end - abs), tones[(size_t) j], vel + (off < 1e-6 ? 8 : 0));
                    }
                }
                break;
            }
            case SynSequence:
            {
                const double step = feel.high ? 0.5 : 0.25;
                const int per = (int) std::lround (4.0 / step);
                Rng rs (s.synthSeed * 977u + (uint32_t) (c % 2) * 131u + 5u);
                std::vector<bool> mask ((size_t) per);
                std::vector<int> pit ((size_t) per);
                const int rootLow = (lowBase - 12) + ((((ci.rootPc - (lowBase - 12)) % 12) + 12) % 12);
                const int fifth = rootLow + 7, alt = rootLow + (ci.pcs.size() > 1 ? ((ci.pcs[1] - ci.rootPc + 12) % 12) : 3);
                const int pool[5] = { rootLow, rootLow, rootLow + 12, fifth, alt };
                for (int i = 0; i < per; ++i) { mask[(size_t) i] = (i == 0) || rs.chance (0.62f); pit[(size_t) i] = pool[rs.range (5)]; }
                for (int k = k0; k < k1; ++k)
                    for (int i = 0; i < per; ++i)
                    {
                        const double abs = k * 4.0 + i * step;
                        if (! mask[(size_t) i] || abs < ci.start - 1e-6 || abs >= end - 1e-6) continue;
                        add (abs, step * 0.8, pit[(size_t) i], vel + (i % 4 == 0 ? 14 : 0));
                    }
                break;
            }
            case SynBells:
            {
                Rng rb (s.synthSeed * 7u + (uint32_t) (c % 2) * 977u + 3u);
                const int slots = std::max (1, (int) std::lround (std::min (ci.len, 8.0) / 0.5));
                const int hits = std::min (slots, 3 + rb.range (3));
                std::vector<bool> used ((size_t) slots);
                for (int h = 0; h < hits; ++h)
                {
                    int sl = rb.range (slots);
                    for (int tries = 0; used[(size_t) sl] && tries < 8; ++tries) sl = rb.range (slots);
                    used[(size_t) sl] = true;
                    const double st = ci.start + sl * 0.5;
                    const int pitch = tones[(size_t) rb.range ((int) tones.size())] + 12;
                    add (st, std::min (rb.chance (0.5f) ? 1.0 : 1.5, end - st), pitch, vel - 6 + rb.range (12));
                }
                break;
            }
            default: // SynGated
            {
                const char* mk = gmask;
                const double step = feel.high ? 0.5 : 0.25;
                const int per = (int) std::lround (4.0 / step);
                for (int k = k0; k < k1; ++k)
                    for (int i = 0; i < per; ++i)
                    {
                        const int mi = feel.high ? i * 2 : i;
                        const double abs = k * 4.0 + i * step;
                        if (mk[mi] != 'x' || abs < ci.start - 1e-6 || abs >= end - 1e-6) continue;
                        const int cnt = std::min<int> (4, (int) tones.size());
                        for (int j = 0; j < cnt; ++j) add (abs, step * 0.7, tones[(size_t) j], vel + (i % 4 == 0 ? 10 : 0));
                    }
                break;
            }
        }
    }
}

// =================================================================================================
// Palette
// =================================================================================================
namespace
{
std::string romanFor (int key, int rootPc, const std::vector<int>& iv)
{
    static const char* r[12] = { "I", "bII", "II", "bIII", "III", "IV", "#IV", "V", "bVI", "VI", "bVII", "VII" };
    std::string s = r[((rootPc - key) % 12 + 12) % 12];
    const bool minor = std::find (iv.begin(), iv.end(), 3) != iv.end() && std::find (iv.begin(), iv.end(), 4) == iv.end();
    if (minor) for (auto& ch : s) ch = (char) std::tolower ((unsigned char) ch);
    if (std::find (iv.begin(), iv.end(), 6) != iv.end() && minor) s += "o";
    return s;
}

int funcOfOffset (int off)
{
    static const int m[12] = { 0, 4, 1, 2, 2, 3, 4, 4, 3, 5, 4, 6 };
    return m[((off % 12) + 12) % 12];
}
} // namespace

int functionOfChord (int key, int scaleIdx, int rootPc)
{
    const auto& S = scaleDefs()[(size_t) scaleDefs()[(size_t) std::clamp (scaleIdx, 0, 11)].parent].iv;
    const int rel = ((rootPc - key) % 12 + 12) % 12;
    int best = 0, bd = 99;
    for (int d = 0; d < 7; ++d)
    {
        int diff = std::abs (S[d] - rel); diff = std::min (diff, 12 - diff);
        if (diff < bd) { bd = diff; best = d; }
    }
    return best;
}

CustomChord toCustom (const Chord& c, double len)
{
    CustomChord cc; cc.rootPc = c.rootPc; cc.iv = c.iv; cc.name = c.name; cc.len = len;
    return cc;
}

std::vector<PaletteChord> buildPalette (int key, int scaleIdx, int kind)
{
    scaleIdx = std::clamp (scaleIdx, 0, 11);
    if (kind == Mixed || kind == Sus2 || kind == Sus4) kind = Triad;
    std::vector<PaletteChord> out;

    // ---- diatonic
    for (int d = 0; d < 7; ++d)
    {
        PaletteChord p;
        p.chord = buildChord (key, scaleIdx, d, kind, -1);
        p.roman = romanFor (key, p.chord.rootPc, p.chord.iv);
        p.group = 0; p.func = d;
        out.push_back (p);
    }

    auto sameTriad = [] (const Chord& a, const Chord& b)
    {
        if (a.rootPc != b.rootPc) return false;
        auto third = [] (const Chord& c) { return c.iv.size() > 1 ? c.iv[1] : 0; };
        auto fifth = [] (const Chord& c) { return c.iv.size() > 2 ? c.iv[2] : 0; };
        return third (a) == third (b) && fifth (a) == fifth (b);
    };

    // ---- borrowed from other modes of the same tonic
    const int parent = scaleDefs()[(size_t) scaleIdx].parent;
    const bool minorFamily = scaleDefs()[(size_t) parent].iv[2] == 3;
    const int modes[4] = { minorFamily ? 0 : 1, minorFamily ? 5 : 3, minorFamily ? 4 : 2, 7 };
    std::vector<PaletteChord> borrowed;
    for (int mi : modes)
        for (int d = 0; d < 7; ++d)
        {
            PaletteChord p;
            p.chord = buildChord (key, mi, d, kind, -1);
            bool dup = false;
            for (const auto& o : out) if (sameTriad (o.chord, p.chord)) dup = true;
            for (const auto& o : borrowed) if (sameTriad (o.chord, p.chord)) dup = true;
            if (dup) continue;
            p.roman = romanFor (key, p.chord.rootPc, p.chord.iv);
            p.group = 1;
            p.func = funcOfOffset (p.chord.rootPc - key);
            borrowed.push_back (p);
        }
    if (borrowed.size() > 8) borrowed.resize (8);          // most common modes come first, so the rare ones are cut
    std::stable_sort (borrowed.begin(), borrowed.end(), [&] (const PaletteChord& a, const PaletteChord& b)
                      { return ((a.chord.rootPc - key + 12) % 12) < ((b.chord.rootPc - key + 12) % 12); });
    out.insert (out.end(), borrowed.begin(), borrowed.end());

    // ---- special: secondary dominants, tritone sub, leading-tone diminished
    const auto& S = scaleDefs()[(size_t) parent].iv;
    static const char* tgtName[5] = { "ii", "iii", "IV", "V", "vi" };
    for (int i = 0; i < 5; ++i)
    {
        const int tgt = i + 1;
        PaletteChord p;
        p.chord.rootPc = (key + S[tgt] + 7) % 12;
        p.chord.iv = { 0, 4, 7, 10 };
        p.chord.name = chordName (p.chord.rootPc, p.chord.iv);
        p.roman = std::string ("V7/") + tgtName[i];
        p.group = 2; p.func = 4; p.target = tgt;
        out.push_back (p);
    }
    {
        PaletteChord p;
        p.chord.rootPc = (key + 1) % 12; p.chord.iv = { 0, 4, 7, 10 };
        p.chord.name = chordName (p.chord.rootPc, p.chord.iv);
        p.roman = "bII7"; p.group = 2; p.func = 4; p.target = 0;
        out.push_back (p);
        PaletteChord q;
        q.chord.rootPc = (key + 11) % 12; q.chord.iv = { 0, 3, 6, 9 };
        q.chord.name = chordName (q.chord.rootPc, q.chord.iv);
        q.roman = "vii\xC2\xB0" "7"; q.group = 2; q.func = 6; q.target = 0;
        out.push_back (q);
    }
    return out;
}

int relatedness (int fromFunc, const PaletteChord& to)
{
    static const int strong[7][3] = { { 3, 4, 5 }, { 4, -1, -1 }, { 5, 3, -1 }, { 4, 0, -1 }, { 0, 5, -1 }, { 3, 1, -1 }, { 0, -1, -1 } };
    static const int maybe[7][3]  = { { 1, 2, -1 }, { 0, 3, -1 }, { 1, 4, -1 }, { 5, 1, -1 }, { 3, 1, -1 }, { 4, 0, -1 }, { 2, -1, -1 } };
    fromFunc = std::clamp (fromFunc, 0, 6);
    const int d = to.group == 2 ? to.target : to.func;
    int r = 0;
    for (int x : strong[fromFunc]) if (x == d) r = 2;
    if (r == 0) for (int x : maybe[fromFunc]) if (x == d) r = 1;
    if (to.group == 1) r = std::min (r, 1);
    return r;
}

// =================================================================================================
// Harmonise a melody
// =================================================================================================
std::vector<CustomChord> harmoniseMelody (const std::vector<NoteEvent>& melody, double lengthBeats, double segLen,
                                          int key, int scaleIdx, int kind)
{
    std::vector<CustomChord> out;
    if (melody.empty() || segLen <= 0) return out;
    scaleIdx = std::clamp (scaleIdx, 0, 11);

    std::vector<PaletteChord> cands;
    for (auto& p : buildPalette (key, scaleIdx, kind)) if (p.group <= 1) cands.push_back (p);

    const auto& sd = scaleDefs()[(size_t) scaleIdx];
    bool inScale[12] = {};
    for (int i = 0; i < sd.n; ++i) inScale[(key + sd.iv[i]) % 12] = true;

    static const int strongN[7][3] = { { 3, 4, 5 }, { 4, -1, -1 }, { 5, 3, -1 }, { 4, 0, -1 }, { 0, 5, -1 }, { 3, 1, -1 }, { 0, -1, -1 } };
    const int nSeg = std::max (1, (int) std::lround (lengthBeats / segLen));
    int prevFunc = -1;
    CustomChord prev;
    bool havePrev = false;

    for (int sidx = 0; sidx < nSeg; ++sidx)
    {
        const double a = sidx * segLen, b = a + segLen;
        double w[12] = {}; double tot = 0;
        for (const auto& n : melody)
        {
            if (n.part != PartMelody) continue;
            const double o = std::min (b, n.start + n.len) - std::max (a, n.start);
            if (o <= 0) continue;
            const bool onBeat = n.start >= a - 1e-6 && std::fmod (n.start, 1.0) < 1e-3;
            const double wt = o * (onBeat ? 1.3 : 1.0) * (0.6 + 0.4 * n.vel / 100.0);
            w[((n.pitch % 12) + 12) % 12] += wt; tot += wt;
        }

        if (tot <= 0)
        {
            if (havePrev) { auto c = prev; c.len = segLen; out.push_back (c); }
            else          { out.push_back (toCustom (cands[0].chord, segLen)); prev = out.back(); havePrev = true; prevFunc = 0; }
            continue;
        }

        double best = -1e9; int bi = 0;
        for (size_t ci = 0; ci < cands.size(); ++ci)
        {
            const auto& c = cands[ci].chord;
            double sc = 0;
            for (int pc = 0; pc < 12; ++pc)
            {
                if (w[pc] <= 0) continue;
                const int rel = ((pc - c.rootPc) % 12 + 12) % 12;
                double v;
                if (rel == 0) v = 1.0;
                else if (std::find (c.iv.begin(), c.iv.end(), rel) != c.iv.end())
                    v = (c.iv.size() > 1 && rel == c.iv[1]) ? 0.9 : (c.iv.size() > 2 && rel == c.iv[2]) ? 0.85 : 0.7;
                else v = inScale[pc] ? -0.15 : -0.6;
                sc += w[pc] * v;
            }
            sc /= tot;
            if (cands[ci].group == 1) sc -= 0.1;
            if (prevFunc >= 0) for (int x : strongN[prevFunc]) if (x == cands[ci].func) sc += 0.12;
            if (sidx == 0 && cands[ci].func == 0 && cands[ci].group == 0) sc += 0.1;
            if (sidx == nSeg - 1 && cands[ci].group == 0 && (cands[ci].func == 0 || cands[ci].func == 4)) sc += 0.1;
            if (sc > best) { best = sc; bi = (int) ci; }
        }
        out.push_back (toCustom (cands[(size_t) bi].chord, segLen));
        prev = out.back(); havePrev = true; prevFunc = cands[(size_t) bi].func;
    }
    return out;
}

// =================================================================================================
// Serialisation
// =================================================================================================
std::string serializeNotes (const std::vector<NoteEvent>& v)
{
    std::string s;
    char buf[96];
    for (const auto& n : v)
    {
        std::snprintf (buf, sizeof (buf), "%.4f,%.4f,%d,%d;", n.start, n.len, n.pitch, n.vel);
        s += buf;
    }
    return s;
}

std::vector<NoteEvent> deserializeNotes (const std::string& s, int part)
{
    std::vector<NoteEvent> v;
    std::stringstream ss (s);
    std::string item;
    while (std::getline (ss, item, ';'))
    {
        double st = 0, len = 0; int p = 0, ve = 0;
        if (std::sscanf (item.c_str(), "%lf,%lf,%d,%d", &st, &len, &p, &ve) == 4 && len > 0 && p >= 0 && p < 128)
        {
            NoteEvent n; n.start = std::max (0.0, st); n.len = len; n.pitch = p; n.vel = std::clamp (ve, 1, 127); n.part = part;
            v.push_back (n);
        }
    }
    return v;
}

std::string serializeChords (const std::vector<CustomChord>& v)
{
    std::string s;
    char buf[64];
    for (const auto& c : v)
    {
        std::snprintf (buf, sizeof (buf), "%d:", c.rootPc);
        s += buf;
        for (size_t i = 0; i < c.iv.size(); ++i) { s += std::to_string (c.iv[i]); if (i + 1 < c.iv.size()) s += ","; }
        std::snprintf (buf, sizeof (buf), ":%.3f;", c.len);
        s += buf;
    }
    return s;
}

std::vector<CustomChord> deserializeChords (const std::string& s)
{
    std::vector<CustomChord> v;
    std::stringstream ss (s);
    std::string item;
    while (std::getline (ss, item, ';'))
    {
        const auto c1 = item.find (':'), c2 = item.rfind (':');
        if (c1 == std::string::npos || c2 == c1) continue;
        CustomChord c;
        c.rootPc = ((std::atoi (item.substr (0, c1).c_str()) % 12) + 12) % 12;
        std::stringstream is (item.substr (c1 + 1, c2 - c1 - 1));
        std::string t;
        while (std::getline (is, t, ',')) c.iv.push_back (std::clamp (std::atoi (t.c_str()), 0, 24));
        c.len = std::clamp (std::atof (item.substr (c2 + 1).c_str()), 0.5, 16.0);
        if (c.iv.size() < 2) continue;
        std::sort (c.iv.begin(), c.iv.end());
        c.name = chordName (c.rootPc, c.iv);
        v.push_back (c);
    }
    return v;
}
} // namespace aio
