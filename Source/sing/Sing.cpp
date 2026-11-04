#include "Sing.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace aio
{
// make a sorted list monophonic: overlapping notes are trimmed, the higher note wins ("skyline")
static void makeMonophonic (std::vector<SingNote>& v)
{
    std::stable_sort (v.begin(), v.end(), [] (const SingNote& a, const SingNote& b) { return a.start < b.start; });
    std::vector<SingNote> out;
    for (auto n : v)
    {
        if (n.len < 0.03) continue;
        if (! out.empty())
        {
            auto& p = out.back();
            const double pEnd = p.start + p.len;
            if (n.start < pEnd - 1.0e-6)
            {
                if (n.pitch > p.pitch)
                {
                    p.len = n.start - p.start;                 // the new, higher note cuts the old one short
                    if (p.len < 0.03) out.pop_back();
                }
                else
                {
                    const double nEnd = n.start + n.len;
                    if (nEnd <= pEnd + 1.0e-6) continue;       // swallowed by the higher note
                    n.len = nEnd - pEnd; n.start = pEnd;       // keep the part that sticks out
                    if (n.len < 0.03) continue;
                }
            }
        }
        out.push_back (n);
    }
    v = std::move (out);
}

SingTrack trackFromPattern (const Pattern& p)
{
    SingTrack t;
    t.lengthBeats = p.lengthBeats > 0 ? p.lengthBeats : 16.0;
    for (auto& n : p.notes)
        if (n.part == PartMelody) t.notes.push_back ({ n.start, n.len, n.pitch });
    makeMonophonic (t.notes);
    return t;
}

SingTrack trackFromMidiFile (const juce::File& f, juce::String& error)
{
    SingTrack out;
    juce::FileInputStream fis (f);
    if (! fis.openedOk()) { error = "Could not open " + f.getFileName(); return out; }
    juce::MidiFile mf;
    if (! mf.readFrom (fis)) { error = "Not a valid MIDI file"; return out; }
    const int tf = mf.getTimeFormat();
    if (tf <= 0) { error = "SMPTE-timed MIDI files are not supported"; return out; }

    struct Cand { std::vector<SingNote> notes; double score = 0; };
    Cand best;
    for (int t = 0; t < mf.getNumTracks(); ++t)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (t));
        seq.updateMatchedPairs();
        Cand c;
        double pitchSum = 0; int overlaps = 0;
        double lastEnd = -1;
        std::vector<SingNote> tmp;
        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            auto* e = seq.getEventPointer (i);
            if (! e->message.isNoteOn() || e->message.getChannel() == 10 || e->noteOffObject == nullptr) continue;
            SingNote n;
            n.start = e->message.getTimeStamp() / (double) tf;
            n.len = (e->noteOffObject->message.getTimeStamp() - e->message.getTimeStamp()) / (double) tf;
            n.pitch = e->message.getNoteNumber();
            if (n.len <= 0) continue;
            if (n.start < lastEnd - 1.0e-6) ++overlaps;
            lastEnd = std::max (lastEnd, n.start + n.len);
            pitchSum += n.pitch;
            tmp.push_back (n);
        }
        if (tmp.size() < 4) continue;
        const double mean = pitchSum / (double) tmp.size();
        c.score = (double) tmp.size() * (0.6 + 0.4 * mean / 100.0) * (1.0 - 0.5 * (double) overlaps / (double) tmp.size());
        c.notes = std::move (tmp);
        if (c.score > best.score) best = std::move (c);
    }
    if (best.notes.empty()) { error = "No melody notes found in that file"; return out; }

    if (best.notes.size() > 6000) best.notes.resize (6000);
    makeMonophonic (best.notes);
    double end = 0;
    for (auto& n : best.notes) end = std::max (end, n.start + n.len);
    out.notes = std::move (best.notes);
    out.lengthBeats = std::max (4.0, std::ceil (end / 4.0 - 1.0e-9) * 4.0);
    return out;
}

std::string serializeTrack (const SingTrack& t)
{
    std::ostringstream os;
    char buf[96];
    std::snprintf (buf, sizeof buf, "%.4f", t.lengthBeats);
    os << buf << "|";
    for (auto& n : t.notes)
    {
        std::snprintf (buf, sizeof buf, "%.4f,%.4f,%d;", n.start, n.len, n.pitch);
        os << buf;
    }
    return os.str();
}

SingTrack deserializeTrack (const std::string& s)
{
    SingTrack t;
    const auto bar = s.find ('|');
    if (bar == std::string::npos) return t;
    t.lengthBeats = std::max (1.0, std::atof (s.substr (0, bar).c_str()));
    size_t pos = bar + 1;
    while (pos < s.size() && t.notes.size() < 20000)
    {
        const auto end = s.find (';', pos);
        if (end == std::string::npos) break;
        double a = 0, b = 0; int p = 0;
        if (std::sscanf (s.substr (pos, end - pos).c_str(), "%lf,%lf,%d", &a, &b, &p) == 3 && b > 0 && p >= 0 && p < 128)
            t.notes.push_back ({ a, b, p });
        pos = end + 1;
    }
    makeMonophonic (t.notes);
    return t;
}
} // namespace aio
