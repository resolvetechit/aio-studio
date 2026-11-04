#include "Composer.h"

namespace aio
{
namespace
{
struct Hit { double off, dur; int voice; };   // voice == -1 -> whole chord

std::vector<Hit> chordHits (int rhythm, double len, int nVoices)
{
    std::vector<Hit> h;
    auto arpIndex = [&] (int mode, int i, int n)
    {
        if (n <= 1) return 0;
        if (mode == 0) return i % n;
        if (mode == 1) return (n - 1) - (i % n);
        const int cyc = 2 * n - 2, k = i % cyc;
        return k < n ? k : cyc - k;
    };

    switch (rhythm)
    {
        case 0: h.push_back ({ 0.0, len * 0.98, -1 }); break;
        case 1: for (double t = 0; t < len - 1e-6; t += 2.0) h.push_back ({ t, std::min (2.0, len - t) * 0.95, -1 }); break;
        case 2: for (double t = 0; t < len - 1e-6; t += 1.0) h.push_back ({ t, 0.9, -1 }); break;
        case 3: for (double t = 0.5; t < len - 1e-6; t += 1.0) h.push_back ({ t, 0.45, -1 }); break;
        case 4:
        {
            static const double o[3] = { 0.0, 1.5, 3.0 }, d[3] = { 1.45, 1.45, 0.95 };
            for (double bar = 0; bar < len - 1e-6; bar += 4.0)
                for (int k = 0; k < 3; ++k)
                    if (bar + o[k] < len - 1e-6) h.push_back ({ bar + o[k], d[k], -1 });
            break;
        }
        case 5: case 6: case 7: case 8:
        {
            const double step = (rhythm == 8) ? 0.25 : 0.5;
            const int mode = (rhythm == 5) ? 0 : (rhythm == 6) ? 1 : 2;
            int i = 0;
            for (double t = 0; t < len - 1e-6; t += step, ++i)
                h.push_back ({ t, step * 0.95, arpIndex (mode, i, nVoices) });
            break;
        }
        default:   // strum
            for (int v = 0; v < nVoices; ++v)
                h.push_back ({ 0.045 * v, len * 0.97 - 0.045 * v, v });
            break;
    }
    return h;
}

void addBass (std::vector<NoteEvent>& out, const ChordInfo& c, int style, int vel)
{
    const int root = 36 + c.rootPc;              // C2..B2
    auto add = [&] (double off, double dur, int pitch, int v)
    {
        if (off < c.len - 1e-6) out.push_back ({ c.start + off, std::min (dur, c.len - off), pitch, v, PartBass });
    };
    switch (style)
    {
        case 0: add (0, c.len * 0.98, root, vel); break;
        case 1: for (double t = 0; t < c.len - 1e-6; t += 0.5) add (t, 0.45, root, vel - (std::fmod (t, 1.0) > 0.1 ? 14 : 0)); break;
        case 2: { int i = 0; for (double t = 0; t < c.len - 1e-6; t += 0.5, ++i) add (t, 0.45, root + (i % 2 ? 12 : 0), vel - (i % 2 ? 10 : 0)); break; }
        case 3:
            for (double bar = 0; bar < c.len - 1e-6; bar += 4.0)
            {
                add (bar + 0.0, 1.4, root, vel);
                add (bar + 1.5, 0.9, root, vel - 10);
                add (bar + 2.75, 1.1, root + 12, vel - 5);
            }
            break;
        default:
            for (double bar = 0; bar < c.len - 1e-6; bar += 4.0)
            {
                add (bar + 0.0, 1.9, root, vel);
                add (bar + 2.0, 1.9, root + 7, vel - 8);
            }
            break;
    }
}

// --------------------------------------------------------------------------------------- melody
struct RCell { double pos, len; bool rest; };

std::vector<RCell> makeBarRhythm (Rng& rng, float density, float restProb)
{
    std::vector<RCell> r;
    double pos = 0;
    const float d = std::clamp (density, 0.f, 1.f);
    while (pos < 4.0 - 1e-6)
    {
        const double durs[6] = { 0.25, 0.5, 0.75, 1.0, 1.5, 2.0 };
        float w[6] = { d * d * 0.9f, 0.45f + d * 0.6f, 0.12f, 0.95f - d * 0.55f, 0.40f * (1.f - d), 0.22f * (1.f - d) };
        for (int k = 0; k < 6; ++k)
        {
            if (pos + durs[k] > 4.0 + 1e-6) w[k] = 0.f;
            // keep odd lengths on the grid: 0.25 and 0.75 only when we are on an 8th/16th boundary
            if (k == 0 && std::fmod (pos, 0.5) > 0.01 && std::fmod (pos, 0.5) < 0.24) w[k] = 0.f;
            if (k == 2 && std::fmod (pos, 0.5) > 0.01) w[k] = 0.f;
            if ((k == 4 || k == 5) && std::fmod (pos, 1.0) > 0.01) w[k] = 0.f;
        }
        float tot = 0; for (float x : w) tot += x;
        if (tot <= 0) { r.push_back ({ pos, 4.0 - pos, false }); break; }
        const int k = rng.weighted (w, 6);
        const bool rest = (pos > 0.01 || rng.chance (0.1f)) && rng.chance (restProb * (durs[k] >= 1.0 ? 0.6f : 1.0f));
        r.push_back ({ pos, durs[k], rest });
        pos += durs[k];
    }
    return r;
}

struct MelodyCtx
{
    const ScaleDef* sc;
    int keyPitch0;                 // midi pitch of scale index 0
    int lo, hi;                    // allowed scale-index range
    const std::vector<ChordInfo>* chords;

    int pitchOf (int idx) const
    {
        const int oct = floorDiv (idx, sc->n), deg = idx - oct * sc->n;
        return keyPitch0 + sc->iv[deg] + 12 * oct;
    }
    const ChordInfo& chordAt (double beat) const
    {
        for (const auto& c : *chords) if (beat >= c.start - 1e-6 && beat < c.start + c.len - 1e-6) return c;
        return chords->back();
    }
    bool isChordTone (int idx, const ChordInfo& c) const
    {
        const int pc = ((pitchOf (idx) % 12) + 12) % 12;
        return std::find (c.pcs.begin(), c.pcs.end(), pc) != c.pcs.end();
    }
    int snap (int idx, const ChordInfo& c, int prefer) const
    {
        int best = idx; int bd = 99;
        for (int d = -5; d <= 5; ++d)
        {
            const int j = idx + d;
            if (j < lo - 2 || j > hi + 2 || ! isChordTone (j, c)) continue;
            const int dist = std::abs (d) * 2 + (d != 0 && ((d > 0) != (prefer > 0)) ? 1 : 0);
            if (dist < bd) { bd = dist; best = j; }
        }
        return best;
    }
};

int pickStep (Rng& rng, int style, int lastStep)
{
    static const float stepwise[7] = { 0.04f, 0.14f, 0.32f, 0.08f, 0.32f, 0.14f, 0.04f };   // -3..+3
    static const float hooky[7]    = { 0.02f, 0.10f, 0.25f, 0.26f, 0.25f, 0.10f, 0.02f };
    float w[11];
    int off, cnt;
    if (style == 1)   // leaps
    {
        static const float leaps[11] = { 0.04f, 0.08f, 0.12f, 0.14f, 0.12f, 0.04f, 0.12f, 0.14f, 0.12f, 0.08f, 0.04f };
        for (int i = 0; i < 11; ++i) w[i] = leaps[i];
        off = -5; cnt = 11;
    }
    else
    {
        const float* src = style == 2 ? hooky : stepwise;
        for (int i = 0; i < 7; ++i) w[i] = src[i];
        off = -3; cnt = 7;
    }
    // momentum: keep going in the same direction a bit more often
    for (int i = 0; i < cnt; ++i)
        if (lastStep != 0 && ((i + off) > 0) == (lastStep > 0) && (i + off) != 0) w[i] *= 1.25f;
    return off + rng.weighted (w, cnt);
}

void genMelody (const ComposeSettings& s, const std::vector<ChordInfo>& chords, Pattern& out)
{
    if (chords.empty()) return;
    Rng rng (s.melodySeed * 7919u + 13u);
    const auto& sd = scaleDefs()[(size_t) std::clamp (s.scale, 0, 11)];

    MelodyCtx ctx;
    ctx.sc = &sd;
    ctx.chords = &chords;
    const int base = 12 * (s.melodyOct + 1);
    ctx.keyPitch0 = base + ((s.key + 6) % 12) - 6;
    static const float spans[4] = { 1.0f, 1.5f, 2.0f, 3.0f };
    const int span = std::max (sd.n, (int) std::lround (sd.n * spans[std::clamp (s.melodyRange, 0, 3)]));
    ctx.lo = -(sd.n / 2);
    ctx.hi = ctx.lo + span;

    const int bars = (int) std::lround (out.lengthBeats / 4.0);
    const bool arp = (s.melodyStyle == 3);

    // two rhythm cells: A (motif) and B (answer / cadence)
    auto rA = makeBarRhythm (rng, s.melodyDensity, s.melodyRests);
    auto rB = makeBarRhythm (rng, s.melodyDensity, s.melodyRests * 0.8f);

    std::vector<int> motifSteps;   // steps recorded while generating A
    std::vector<int> prevBarSteps;
    int cur = ctx.lo + span / 2;
    cur = ctx.snap (cur, ctx.chordAt (0.0), 0);
    int lastStep = 0, arpDir = 1;

    for (int bar = 0; bar < bars; ++bar)
    {
        // phrase plan: A  A'  A  B   (repeated); 1 and 2 bar loops use A / A B
        const int slot = bar % 4;
        const bool isB = (bars >= 2 && ((bars == 2 && bar == 1) || (bars >= 4 && slot == 3)));
        const bool isAprime = (bars >= 4 && slot == 1);
        const auto& cells = isB ? rB : rA;

        std::vector<int> steps;
        const bool reuse = ! isB && ! motifSteps.empty();
        int noteIdx = 0;

        for (const auto& cell : cells)
        {
            if (cell.rest) { ++noteIdx; continue; }
            const double t = bar * 4.0 + cell.pos;
            const auto& ch = ctx.chordAt (t);

            int step;
            if (arp)
            {
                // next chord tone in the current direction
                step = 0; int j = cur;
                for (int tries = 0; tries < 12; ++tries)
                {
                    j += arpDir;
                    if (j > ctx.hi || j < ctx.lo) { arpDir = -arpDir; j = cur; continue; }
                    if (ctx.isChordTone (j, ch)) { step = j - cur; break; }
                }
                if (rng.chance (0.12f)) arpDir = -arpDir;
            }
            else if (reuse && noteIdx < (int) motifSteps.size() && ! (isAprime && cell.pos >= 2.0 && rng.chance (0.8f)))
                step = motifSteps[(size_t) noteIdx];
            else
                step = pickStep (rng, s.melodyStyle, lastStep);

            int idx = cur + step;
            if (idx > ctx.hi) idx = ctx.hi - std::abs (step) % 3;
            if (idx < ctx.lo) idx = ctx.lo + std::abs (step) % 3;

            const bool strong = std::fmod (cell.pos, 2.0) < 1e-6 || cell.len >= 1.0;
            if (strong && ! arp) idx = ctx.snap (idx, ch, step);

            // final note of the whole phrase resolves to the root / a chord tone of the last chord
            const bool lastNote = (bar == bars - 1) && (&cell == &cells.back() || cell.pos + cell.len >= 3.99);
            if (lastNote)
            {
                int bestI = idx, bd = 99;
                for (int j = ctx.lo; j <= ctx.hi; ++j)
                {
                    const int pc = ((ctx.pitchOf (j) % 12) + 12) % 12;
                    if (pc != ch.rootPc) continue;
                    if (std::abs (j - idx) < bd) { bd = std::abs (j - idx); bestI = j; }
                }
                idx = bestI;
            }

            steps.push_back (idx - cur);
            lastStep = idx - cur;
            cur = idx;
            ++noteIdx;

            NoteEvent n;
            n.start = t;
            n.len = cell.len * 0.94;
            n.pitch = std::clamp (ctx.pitchOf (cur), 0, 127);
            n.vel = std::clamp (s.melodyVel + (cell.pos < 1e-6 ? 8 : (std::fmod (cell.pos, 1.0) > 0.1 ? -8 : 0)), 1, 127);
            n.part = PartMelody;
            out.notes.push_back (n);
        }

        if (! isB && motifSteps.empty()) motifSteps = steps;
        prevBarSteps = steps;
    }
}

void applyFeel (std::vector<NoteEvent>& notes, double length, float swing, float humanize, Rng& rng)
{
    for (auto& n : notes)
    {
        const double frac = std::fmod (n.start, 1.0);
        if (swing > 0.f && std::abs (frac - 0.5) < 0.02) n.start += swing * (1.0 / 6.0);
        if (humanize > 0.f)
        {
            n.vel = std::clamp (n.vel + (int) std::lround (rng.bipolar() * 12.f * humanize), 1, 127);
            if (n.start > 0.01) n.start = std::max (0.0, n.start + rng.bipolar() * 0.02 * humanize);
        }
        n.start = std::clamp (n.start, 0.0, length - 0.05);
        n.len = std::max (0.05, std::min (n.len, length - n.start));
    }
}
} // namespace

Pattern generatePattern (const ComposeSettings& s)
{
    Pattern p;
    p.lengthBeats = barsFromIndex (s.bars) * 4.0;
    const double clen = chordLenFromIndex (s.chordLen);
    const int nChords = std::max (1, (int) std::lround (p.lengthBeats / clen));
    const int scaleIdx = std::clamp (s.scale, 0, 11);
    const auto& sd = scaleDefs()[(size_t) scaleIdx];
    const bool minorFlavour = sd.iv[2] == 3;

    Rng rc (s.chordSeed * 2654435761u + 77u);

    // ---- progression ----
    std::vector<ProgSlot> slots;
    const auto& defs = progDefs();
    if (s.progression < (int) defs.size())
    {
        const auto& d = defs[(size_t) s.progression].slots;
        for (int i = 0; i < nChords; ++i) slots.push_back (d[(size_t) i % d.size()]);
    }
    else
        slots = randomProgression (nChords, minorFlavour, s.progression > (int) defs.size(), rc);

    // ---- chords + voicing ----
    std::vector<int> prev;
    const int center = 12 * (s.chordOct + 1) + 6;
    std::vector<NoteEvent> chordNotes, bassNotes;
    for (int i = 0; i < nChords; ++i)
    {
        int kind = s.chordKind;
        if (kind == Mixed)
        {
            const float w[4] = { 0.35f, 0.35f, 0.15f, 0.15f };
            static const int kinds[4] = { Triad, Seventh, Add9, Sus4 };
            kind = kinds[rc.weighted (w, 4)];
        }
        const Chord c = buildChord (s.key, scaleIdx, slots[(size_t) i].deg, kind, slots[(size_t) i].force);
        ChordInfo ci;
        ci.start = i * clen; ci.len = clen; ci.name = c.name; ci.rootPc = c.rootPc;
        for (int iv : c.iv) ci.pcs.push_back ((c.rootPc + iv) % 12);
        p.chords.push_back (ci);

        auto v = voiceChord (c, prev, center, s.voicing, s.smoothVoicing);
        prev = v;

        std::vector<int> arpVoices = v;
        if (s.chordRhythm >= 5 && s.chordRhythm <= 8) arpVoices.push_back (v.front() + 12);
        const bool isArp = (s.chordRhythm >= 5 && s.chordRhythm <= 8);
        const auto hits = chordHits (s.chordRhythm, clen, (int) (isArp ? arpVoices.size() : v.size()));
        for (const auto& h : hits)
        {
            const int vel = s.chordVel + (h.off < 1e-6 ? 8 : 0);
            if (h.voice < 0)
                for (int pch : v) chordNotes.push_back ({ ci.start + h.off, h.dur, pch, vel, PartChords });
            else
                chordNotes.push_back ({ ci.start + h.off, h.dur, (isArp ? arpVoices : v)[(size_t) h.voice], vel, PartChords });
        }
        if (s.bassOn) addBass (bassNotes, ci, s.bassStyle, 100);
    }

    Rng rf (s.chordSeed * 40503u + 5u);
    applyFeel (chordNotes, p.lengthBeats, s.swing, s.humanize, rf);
    applyFeel (bassNotes,  p.lengthBeats, s.swing, s.humanize * 0.5f, rf);
    p.notes = chordNotes;
    p.notes.insert (p.notes.end(), bassNotes.begin(), bassNotes.end());

    if (s.melodyOn)
    {
        Pattern mel; mel.lengthBeats = p.lengthBeats;
        genMelody (s, p.chords, mel);
        Rng rm (s.melodySeed * 31u + 9u);
        applyFeel (mel.notes, p.lengthBeats, s.swing, s.humanize * 0.6f, rm);
        p.notes.insert (p.notes.end(), mel.notes.begin(), mel.notes.end());
    }

    std::stable_sort (p.notes.begin(), p.notes.end(), [] (const NoteEvent& a, const NoteEvent& b) { return a.start < b.start; });
    return p;
}
} // namespace aio
