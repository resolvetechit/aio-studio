#pragma once
#include <vector>
#include <memory>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../compose/Composer.h"

/*
   Sing mode (karaoke-style pitch game).

   SingTrack   the melody to sing: monophonic notes on a looping beat grid (from the Compose melody or an imported MIDI file)
   ScoreKeeper turns a stream of (beat, detected pitch) frames into per-note hit fractions, ratings and a 0..10000 score
*/
namespace aio
{
struct SingNote { double start = 0, len = 1; int pitch = 60; };

struct SingTrack
{
    double lengthBeats = 16.0;
    std::vector<SingNote> notes;     // sorted by start, non-overlapping

    bool empty() const { return notes.empty(); }

    // index of the note sounding at `beat` (wrapped into the loop) or -1
    int noteAt (double beat) const
    {
        if (notes.empty() || lengthBeats <= 0) return -1;
        double b = std::fmod (beat, lengthBeats);
        if (b < 0) b += lengthBeats;
        int lo = 0, hi = (int) notes.size() - 1, found = -1;
        while (lo <= hi)
        {
            const int mid = (lo + hi) / 2;
            if (notes[(size_t) mid].start <= b) { found = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (found >= 0 && b < notes[(size_t) found].start + notes[(size_t) found].len) return found;
        return -1;
    }

    int lowestPitch() const  { int v = 127; for (auto& n : notes) v = std::min (v, n.pitch); return notes.empty() ? 48 : v; }
    int highestPitch() const { int v = 0;   for (auto& n : notes) v = std::max (v, n.pitch); return notes.empty() ? 72 : v; }
};

SingTrack trackFromPattern (const Pattern& p);
SingTrack trackFromMidiFile (const juce::File& f, juce::String& error);
std::string serializeTrack (const SingTrack& t);
SingTrack   deserializeTrack (const std::string& s);

// ---------------------------------------------------------------------------------------------------------------
struct SingConfig
{
    float  toleranceSemis = 1.0f;     // Easy 1.5, Medium 1.0, Hard 0.5
    bool   octaveAgnostic = true;     // sing in any octave
    double graceBeats = 0.12;         // start of each note does not count against the singer
};

enum SingRating { RatingMiss = 0, RatingGood, RatingGreat, RatingPerfect };

struct SingEvent { int noteIndex; int rating; double beat; };

class ScoreKeeper
{
public:
    void setTrack (std::shared_ptr<const SingTrack> t) { track = std::move (t); resetPass(); }
    void resetPass()
    {
        const size_t n = track ? track->notes.size() : 0;
        hit.assign (n, 0.0); cells.assign (n, 0);
        nextToFinish = 0; lastBeat = -1.0; scoreNow = 0.0; hittingNow = false; events.clear();
    }

    void addFrame (double beat, float midi, const SingConfig& cfg)
    {
        if (! track || track->empty()) return;
        const double L = track->lengthBeats;
        double b = std::fmod (beat, L); if (b < 0) b += L;

        if (lastBeat >= 0.0)
        {
            if (b < lastBeat - 0.5 * L)              // looped around: close the pass
            {
                finishAll();
                lastPassScore = scoreNow; ++passes; passJustFinished = true;
                resetPassKeepStats();
            }
            else if (b < lastBeat - 0.05)            // seek backwards inside the loop: start over
                resetPassKeepStats();
        }
        const double dBeat = (lastBeat >= 0.0 && b >= lastBeat) ? std::min (0.25, b - lastBeat) : 0.0;
        lastBeat = b;

        // finish notes that are over
        while (nextToFinish < track->notes.size() && b >= track->notes[nextToFinish].start + track->notes[nextToFinish].len)
            finishNote (nextToFinish++);

        const int i = track->noteAt (b);
        hittingNow = false;
        if (i < 0) { recompute(); return; }
        const auto& note = track->notes[(size_t) i];
        const bool voiced = midi >= 0.f;
        bool ok = false;
        if (voiced)
        {
            double d = (double) midi - (double) note.pitch;
            if (cfg.octaveAgnostic) { d = std::fmod (d, 12.0); if (d > 6.0) d -= 12.0; if (d < -6.0) d += 12.0; }
            ok = std::abs (d) <= (double) cfg.toleranceSemis;
        }
        hittingNow = ok;
        const double into = b - note.start;
        const double grace = std::min (cfg.graceBeats, 0.4 * note.len);
        if (ok)
        {
            const int cell = std::clamp ((int) (into / note.len * 16.0), 0, 15);
            cells[(size_t) i] = (uint16_t) (cells[(size_t) i] | (1u << cell));
        }
        if (into >= grace) { if (ok) hit[(size_t) i] += dBeat; }
        else if (ok)       hit[(size_t) i] += dBeat;             // lucky early hits count, misses in the grace window do not hurt
        gracePerNote = grace;
        recompute();
    }

    double fraction (size_t i) const
    {
        if (! track || i >= hit.size()) return 0.0;
        const auto& n = track->notes[i];
        const double denom = std::max (n.len - cfgGrace (n), 0.5 * n.len);
        return std::min (1.0, hit[i] / std::max (denom, 1.0e-6));
    }
    uint16_t hitCells (size_t i) const { return i < cells.size() ? cells[i] : (uint16_t) 0; }

    double score() const { return scoreNow; }                        // 0..10000, current pass
    double lastScore() const { return lastPassScore; }
    int    passCount() const { return passes; }
    bool   hitting() const { return hittingNow; }
    bool   takePassFinished() { const bool v = passJustFinished; passJustFinished = false; return v; }
    std::vector<SingEvent> takeEvents() { auto e = std::move (events); events.clear(); return e; }

    static const char* rankName (double score)
    {
        if (score < 1000) return "Warming up";
        if (score < 3000) return "Bathroom singer";
        if (score < 5000) return "Open mic";
        if (score < 7000) return "Headliner";
        if (score < 8500) return "Chart topper";
        return "Superstar";
    }
    static int stars (double score) { return std::clamp ((int) (score / 2000.0), 0, 5); }

private:
    double cfgGrace (const SingNote& n) const { return std::min (gracePerNote, 0.4 * n.len); }

    void recompute()
    {
        double num = 0, den = 0;
        for (size_t i = 0; i < track->notes.size(); ++i)
        {
            const double len = track->notes[i].len;
            num += fraction (i) * len; den += len;
        }
        scoreNow = den > 0 ? 10000.0 * num / den : 0.0;
    }

    void finishNote (size_t i)
    {
        const double f = fraction (i);
        const int r = f >= 0.85 ? RatingPerfect : f >= 0.65 ? RatingGreat : f >= 0.40 ? RatingGood : RatingMiss;
        events.push_back ({ (int) i, r, track->notes[i].start + track->notes[i].len });
    }
    void finishAll() { while (nextToFinish < track->notes.size()) finishNote (nextToFinish++); }
    void resetPassKeepStats()
    {
        const size_t n = track->notes.size();
        hit.assign (n, 0.0); cells.assign (n, 0);
        nextToFinish = 0; scoreNow = 0.0; hittingNow = false;
    }

    std::shared_ptr<const SingTrack> track;
    std::vector<double> hit;
    std::vector<uint16_t> cells;
    std::vector<SingEvent> events;
    size_t nextToFinish = 0;
    double lastBeat = -1.0, scoreNow = 0.0, lastPassScore = 0.0, gracePerNote = 0.12;
    int passes = 0;
    bool hittingNow = false, passJustFinished = false;
};
} // namespace aio
