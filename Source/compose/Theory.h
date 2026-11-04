#pragma once
#include <vector>
#include <string>
#include <array>
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace aio
{
// Small deterministic RNG (same sequence on every platform / compiler).
struct Rng
{
    uint64_t s;
    explicit Rng (uint64_t seed = 1) : s (seed * 0x9E3779B97F4A7C15ULL + 0x1234567ULL) { next(); next(); }
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    float uni()               { return (float) (next() >> 40) * (1.0f / 16777216.0f); }   // [0,1)
    int   range (int n)       { return n <= 1 ? 0 : (int) (next() % (uint64_t) n); }
    bool  chance (float p)    { return uni() < p; }
    float bipolar()           { return uni() * 2.f - 1.f; }

    // pick index by weights
    int weighted (const float* w, int n)
    {
        float tot = 0; for (int i = 0; i < n; ++i) tot += w[i];
        float r = uni() * tot;
        for (int i = 0; i < n; ++i) { r -= w[i]; if (r < 0) return i; }
        return n - 1;
    }
};

struct ScaleDef { const char* name; int n; int iv[7]; int parent; };

inline const std::array<ScaleDef, 12>& scaleDefs()
{
    static const std::array<ScaleDef, 12> s = {{
        { "Major",            7, { 0, 2, 4, 5, 7, 9, 11 }, 0 },
        { "Natural Minor",    7, { 0, 2, 3, 5, 7, 8, 10 }, 1 },
        { "Dorian",           7, { 0, 2, 3, 5, 7, 9, 10 }, 2 },
        { "Phrygian",         7, { 0, 1, 3, 5, 7, 8, 10 }, 3 },
        { "Lydian",           7, { 0, 2, 4, 6, 7, 9, 11 }, 4 },
        { "Mixolydian",       7, { 0, 2, 4, 5, 7, 9, 10 }, 5 },
        { "Locrian",          7, { 0, 1, 3, 5, 6, 8, 10 }, 6 },
        { "Harmonic Minor",   7, { 0, 2, 3, 5, 7, 8, 11 }, 7 },
        { "Melodic Minor",    7, { 0, 2, 3, 5, 7, 9, 11 }, 8 },
        { "Major Pentatonic", 5, { 0, 2, 4, 7, 9, 0, 0 },  0 },
        { "Minor Pentatonic", 5, { 0, 3, 5, 7, 10, 0, 0 }, 1 },
        { "Blues",            6, { 0, 3, 5, 6, 7, 10, 0 }, 1 },
    }};
    return s;
}

inline const char* pcName (int pc)
{
    static const char* n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return n[((pc % 12) + 12) % 12];
}

inline int floorDiv (int a, int b) { int q = a / b; if ((a % b != 0) && ((a < 0) != (b < 0))) --q; return q; }

// ---------------------------------------------------------------------------------- progressions
struct ProgSlot { int deg; int force; };   // force: -1 diatonic, 0 major, 1 minor
struct ProgDef  { const char* name; std::vector<ProgSlot> slots; };

inline const std::vector<ProgDef>& progDefs()
{
    static const std::vector<ProgDef> p = {
        { "I - V - vi - IV  (Pop)",            { {0,-1},{4,-1},{5,-1},{3,-1} } },
        { "vi - IV - I - V  (Emotional)",      { {5,-1},{3,-1},{0,-1},{4,-1} } },
        { "I - vi - IV - V  (50s)",            { {0,-1},{5,-1},{3,-1},{4,-1} } },
        { "ii - V - I - vi  (Jazz)",           { {1,-1},{4,-1},{0,-1},{5,-1} } },
        { "i - VI - III - VII  (Epic)",        { {0,-1},{5,-1},{2,-1},{6,-1} } },
        { "i - VII - VI - V  (Andalusian)",    { {0,-1},{6,-1},{5,-1},{4,0} } },
        { "i - iv - VII - III",                { {0,-1},{3,-1},{6,-1},{2,-1} } },
        { "I - IV - V - IV  (Rock)",           { {0,-1},{3,-1},{4,-1},{3,-1} } },
        { "I - iii - IV - V",                  { {0,-1},{2,-1},{3,-1},{4,-1} } },
        { "i - iv - v - i  (Dark)",            { {0,-1},{3,-1},{4,-1},{0,-1} } },
        { "vi - V - IV - V",                   { {5,-1},{4,-1},{3,-1},{4,-1} } },
        { "I - V - vi - iii - IV - I - IV - V  (Canon)", { {0,-1},{4,-1},{5,-1},{2,-1},{3,-1},{0,-1},{3,-1},{4,-1} } },
        { "i - i - iv - V  (Minor blues-ish)", { {0,-1},{0,-1},{3,-1},{4,0} } },
        { "IV - I - V - vi",                   { {3,-1},{0,-1},{4,-1},{5,-1} } },
    };
    return p;
}

inline int numProgressionChoices() { return (int) progDefs().size() + 2; }   // + Random Safe / Random Adventurous

// ---------------------------------------------------------------------------------- chords
enum ChordKind { Triad = 0, Seventh, Ninth, Sus2, Sus4, Add9, Mixed };

struct Chord
{
    int rootPc = 0;
    int degree = 0;
    std::vector<int> iv;      // ascending semitone intervals from the root
    std::string name;
};

inline std::string chordName (int rootPc, const std::vector<int>& iv)
{
    auto has = [&] (int x) { return std::find (iv.begin(), iv.end(), x) != iv.end(); };
    const bool m3 = has (3), M3 = has (4);
    const bool s2 = has (2) && ! m3 && ! M3, s4 = has (5) && ! m3 && ! M3;
    const bool b5 = has (6), s5 = has (8);
    const bool b7 = has (10), M7 = has (11), d7 = has (9);
    const bool n9 = has (14);

    std::string q;
    if (m3 && b5)
        q = d7 ? "dim7" : (b7 ? "m7b5" : "dim");
    else if (M3 && s5)
        q = (b7 || M7) ? "aug7" : "aug";
    else
    {
        if (m3) q += "m";
        if (M7) q += "maj";
        if ((b7 || M7) && n9) q += "9";
        else if (b7 || M7)    q += "7";
        else if (n9)          q += "add9";
        if (s2) q += "sus2";
        if (s4) q += "sus4";
    }
    return std::string (pcName (rootPc)) + q;
}

inline Chord buildChord (int key, int scaleIdx, int degree, int kind, int force)
{
    const auto& sd = scaleDefs()[(size_t) scaleIdx];
    const auto& S = scaleDefs()[(size_t) sd.parent].iv;   // 7-note parent scale for stacking thirds
    degree = ((degree % 7) + 7) % 7;

    auto at = [&] (int k) { return S[k % 7] + 12 * (k / 7) - S[degree]; };
    int third = at (degree + 2), fifth = at (degree + 4), seventh = at (degree + 6), ninth = at (degree + 8);
    if (force == 0) { third = 4; fifth = 7; seventh = 10; }
    if (force == 1) { third = 3; fifth = 7; seventh = 10; }

    Chord c;
    c.degree = degree;
    c.rootPc = (key + S[degree]) % 12;
    switch (kind)
    {
        case Triad:   c.iv = { 0, third, fifth }; break;
        case Seventh: c.iv = { 0, third, fifth, seventh }; break;
        case Ninth:   c.iv = { 0, third, fifth, seventh, ninth }; break;
        case Sus2:    c.iv = { 0, 2, 7 }; break;
        case Sus4:    c.iv = { 0, 5, 7 }; break;
        case Add9:    c.iv = { 0, third, fifth, ninth }; break;
        default:      c.iv = { 0, third, fifth }; break;
    }
    std::sort (c.iv.begin(), c.iv.end());
    c.name = chordName (c.rootPc, c.iv);
    return c;
}

// ---------------------------------------------------------------------------------- random progressions
inline std::vector<ProgSlot> randomProgression (int count, bool minorFlavour, bool adventurous, Rng& rng)
{
    // rows: from-degree, columns: to-degree (relative weights)
    static const float T[7][7] = {
        { 0.0f, 0.10f, 0.10f, 0.30f, 0.25f, 0.25f, 0.00f },
        { 0.10f, 0.0f, 0.00f, 0.20f, 0.60f, 0.10f, 0.00f },
        { 0.00f, 0.20f, 0.0f, 0.40f, 0.00f, 0.40f, 0.00f },
        { 0.25f, 0.15f, 0.00f, 0.0f, 0.40f, 0.20f, 0.00f },
        { 0.50f, 0.00f, 0.00f, 0.20f, 0.0f, 0.30f, 0.00f },
        { 0.00f, 0.25f, 0.15f, 0.35f, 0.25f, 0.0f, 0.00f },
        { 0.70f, 0.00f, 0.30f, 0.00f, 0.00f, 0.00f, 0.0f },
    };
    std::vector<ProgSlot> out;
    int cur = 0;
    for (int i = 0; i < count; ++i)
    {
        int force = -1;
        if (i > 0)
        {
            float w[7];
            for (int k = 0; k < 7; ++k)
                w[k] = adventurous ? (k == 6 ? 0.05f : 0.2f + T[cur][k]) : T[cur][k];
            cur = rng.weighted (w, 7);
        }
        if (cur == 4 && minorFlavour && rng.chance (0.6f)) force = 0;                 // harmonic V
        if (adventurous && cur != 0 && rng.chance (0.18f)) force = rng.chance (0.5f) ? 0 : 1;   // borrowed colour
        out.push_back ({ cur, force });
    }
    return out;
}

// ---------------------------------------------------------------------------------- voicing
inline std::vector<int> voiceChord (const Chord& c, const std::vector<int>& prev, int centerMidi, int style, bool smooth)
{
    const int n = (int) c.iv.size();
    const int rootBase = 12 * floorDiv (centerMidi, 12) + c.rootPc;

    std::vector<int> best;
    double bestCost = 1e18;
    for (int inv = 0; inv < n; ++inv)
        for (int shift = -12; shift <= 12; shift += 12)
        {
            std::vector<int> v;
            for (int i = 0; i < n; ++i)
            {
                int p = rootBase + c.iv[(size_t) i] + shift;
                if (i < inv) p += 12;
                v.push_back (p);
            }
            std::sort (v.begin(), v.end());
            double mean = 0; for (int p : v) mean += p; mean /= n;
            double cost = 0.35 * std::abs (mean - (centerMidi + 2));
            if (smooth && ! prev.empty())
                for (int p : v)
                {
                    int md = 99;
                    for (int q : prev) md = std::min (md, std::abs (p - q));
                    cost += md;
                }
            else
                cost += (inv == 0 ? 0.0 : 2.5);   // prefer root position without a previous chord
            if (cost < bestCost) { bestCost = cost; best = v; }
        }

    if (style == 1 && best.size() >= 3)            // open: drop the 2nd-highest note by an octave
    {
        best[best.size() - 2] -= 12;
        std::sort (best.begin(), best.end());
    }
    else if (style == 2 && ! best.empty())         // wide: root an octave lower + the rest as is
    {
        const int low = best.front();
        int r = low - (((low - c.rootPc) % 12) + 12) % 12;   // root at or just below the lowest note
        if (r == low) r -= 12;
        best.insert (best.begin(), r);
    }
    for (auto& p : best) p = std::clamp (p, 0, 127);
    return best;
}
} // namespace aio
