#pragma once
#include "Widgets.h"
#include "../compose/Styles.h"

// Compose tab building blocks: chord palette, chord timeline, per-part suggestion strip,
// editable piano roll (with harmonic layers) and drum grid.
namespace aio::ui
{
inline juce::Colour partColour (int part)
{
    switch (part)
    {
        case aio::PartChords: return col::chords;
        case aio::PartMelody: return col::melody;
        case aio::PartBass:   return col::bass;
        case aio::PartDrums:  return col::accent2;
        default:              return col::accent;
    }
}

inline const char* partName (int part)
{
    static const char* n[5] = { "Chords", "Melody", "Bass", "Drums", "Synth" };
    return n[std::clamp (part, 0, 4)];
}

inline float param (AIOProcessor& p, const char* id) { return p.apvts.getRawParameterValue (id)->load(); }

inline std::vector<int> voicedNotes (AIOProcessor& proc, const aio::Chord& c)
{
    const int center = 12 * ((int) param (proc, "cmpChordOct") + 1) + 6;
    return aio::voiceChord (c, {}, center, 0, false);
}

// =================================================================================================
// Chord palette (diatonic / borrowed / special) with "what usually comes next" highlighting
// =================================================================================================
class ChordPalette : public juce::Component, private juce::Timer
{
public:
    explicit ChordPalette (AIOProcessor& p) : proc (p) { rebuild(); startTimerHz (8); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (col::bg.brighter (0.03f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.f);

        static const char* names[3] = { "In key", "Borrowed", "Special" };
        static const juce::Colour gc[3] = { col::chords, col::accent2, col::violet };
        for (int r = 0; r < 3; ++r)
        {
            g.setColour (gc[r].withAlpha (0.85f));
            g.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            g.drawText (names[r], 10, rowTop (r), kLabelW - 12, rowH(), juce::Justification::centredLeft);
        }

        for (int i = 0; i < (int) entries.size(); ++i)
        {
            const auto& e = entries[(size_t) i];
            const auto rc = chipRect (i).toFloat();
            const juce::Colour c = gc[e.group];
            const int rel = fromFunc < 0 ? (e.group == 0 && e.func == 0 ? 2 : 0) : relatedness (fromFunc, e);

            g.setColour (c.withAlpha (rel == 2 ? 0.34f : rel == 1 ? 0.2f : 0.1f));
            g.fillRoundedRectangle (rc, 5.f);
            if (i == hover) { g.setColour (juce::Colours::white.withAlpha (0.12f)); g.fillRoundedRectangle (rc, 5.f); }
            g.setColour (rel == 2 ? c.brighter (0.4f) : c.withAlpha (rel == 1 ? 0.8f : 0.35f));
            g.drawRoundedRectangle (rc.reduced (0.5f), 5.f, rel == 2 ? 2.2f : 1.f);

            g.setColour (col::text);
            g.setFont (juce::Font (juce::FontOptions (13.f, juce::Font::bold)));
            g.drawText (e.chord.name, rc.withTrimmedBottom (rc.getHeight() * 0.42f).toNearestInt(), juce::Justification::centred);
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (e.roman, rc.withTrimmedTop (rc.getHeight() * 0.55f).toNearestInt(), juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int h = chipAt (e.getPosition());
        if (h != hover) { hover = h; repaint(); }
    }
    void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = chipAt (e.getPosition());
        if (i < 0) return;
        const auto& pc = entries[(size_t) i];
        for (int n : voicedNotes (proc, pc.chord)) proc.audition (aio::PartChords, n, 92, 650);
        if (! e.mods.isShiftDown()) proc.addCustomChord (pc.chord);      // shift-click only auditions
    }

private:
    static constexpr int kLabelW = 66;

    void rebuild()
    {
        key = (int) param (proc, "cmpKey"); scale = (int) param (proc, "cmpScale"); kind = (int) param (proc, "cmpChordKind");
        entries = aio::buildPalette (key, scale, kind);
        for (auto& r : rows) r.clear();
        for (int i = 0; i < (int) entries.size(); ++i) rows[(size_t) entries[(size_t) i].group].push_back (i);
    }

    void timerCallback() override
    {
        if (! isShowing()) return;
        const int k = (int) param (proc, "cmpKey"), sc = (int) param (proc, "cmpScale"), kd = (int) param (proc, "cmpChordKind");
        bool changed = false;
        if (k != key || sc != scale || kd != kind) { rebuild(); changed = true; }
        const auto& cc = proc.getCustomChords();
        const int fn = cc.empty() ? -1 : aio::functionOfChord (k, sc, cc.back().rootPc);
        if (fn != fromFunc) { fromFunc = fn; changed = true; }
        if (changed) repaint();
    }

    int rowH() const { return (getHeight() - 8) / 3; }
    int rowTop (int r) const { return 4 + r * rowH(); }

    juce::Rectangle<int> chipRect (int entry) const
    {
        const auto& e = entries[(size_t) entry];
        const auto& row = rows[(size_t) e.group];
        const int col = (int) (std::find (row.begin(), row.end(), entry) - row.begin());
        const int w = (getWidth() - kLabelW - 6) / 8;
        return { kLabelW + col * w, rowTop (e.group), w - 4, rowH() - 4 };
    }

    int chipAt (juce::Point<int> p) const
    {
        for (int i = 0; i < (int) entries.size(); ++i) if (chipRect (i).contains (p)) return i;
        return -1;
    }

    AIOProcessor& proc;
    std::vector<aio::PaletteChord> entries;
    std::array<std::vector<int>, 3> rows;
    int key = -1, scale = -1, kind = -1, fromFunc = -1, hover = -1;
};

// =================================================================================================
// "My chords" timeline: click = hear, drag = reorder, right-click = remove, wheel = length
// =================================================================================================
class ChordTimeline : public juce::Component, private juce::Timer
{
public:
    explicit ChordTimeline (AIOProcessor& p)
        : proc (p), useMine (p.apvts, "cmpUseCustom", "USE MY CHORDS", col::violet)
    {
        harmBtn.setButtonText ("Find chords for melody");
        harmBtn.setTooltip ("Looks at the melody and picks the chords that fit it best. The melody is frozen first.");
        harmBtn.onClick = [this]
        {
            const int n = proc.harmoniseMelody();
            status = n > 0 ? juce::String (n) + " chords fitted to the melody" : "Nothing to harmonise: the melody part is empty";
            repaint();
        };
        clearBtn.setButtonText ("Clear");
        clearBtn.onClick = [this] { proc.clearCustomChords(); status = {}; };
        addAndMakeVisible (useMine); addAndMakeVisible (harmBtn); addAndMakeVisible (clearBtn);
        startTimerHz (8);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8, 6);
        auto top = b.removeFromTop (24);
        useMine.setBounds (top.removeFromLeft (128));
        clearBtn.setBounds (top.removeFromRight (54)); top.removeFromRight (6);
        harmBtn.setBounds (top.removeFromRight (150));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (col::bg.brighter (0.03f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.f);
        const auto& cc = proc.getCustomChords();
        if (cc.empty())
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (12.f)));
            g.drawFittedText ("Click chords in the palette to build your own progression.\nClick = hear and add, shift-click = hear only.\nDrag a chip to reorder, right-click to remove, mouse wheel for length.",
                              getLocalBounds().reduced (12, 36), juce::Justification::centredLeft, 4);
            return;
        }
        for (int i = 0; i < (int) cc.size(); ++i)
        {
            const auto rc = chipRect (i).toFloat();
            const bool dropHere = dragging && i == dropIdx;
            g.setColour (col::violet.withAlpha (i == dragIdx && dragging ? 0.08f : 0.2f));
            g.fillRoundedRectangle (rc, 5.f);
            g.setColour (dropHere ? juce::Colours::white : col::violet.withAlpha (0.7f));
            g.drawRoundedRectangle (rc.reduced (0.5f), 5.f, dropHere ? 2.f : 1.f);
            g.setColour (col::text);
            g.setFont (juce::Font (juce::FontOptions (13.f, juce::Font::bold)));
            g.drawText (cc[(size_t) i].name, rc.withTrimmedBottom (rc.getHeight() * 0.4f).toNearestInt(), juce::Justification::centred);
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (juce::String (cc[(size_t) i].len, 0) + " beats", rc.withTrimmedTop (rc.getHeight() * 0.58f).toNearestInt(), juce::Justification::centred);
        }
        if (status.isNotEmpty())
        {
            g.setColour (col::accent);
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText (status, getLocalBounds().removeFromBottom (18).reduced (10, 0), juce::Justification::centredLeft);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragIdx = chipAt (e.getPosition()); dragging = false; dropIdx = dragIdx;
        if (dragIdx < 0) return;
        auto cc = proc.getCustomChords();
        if (e.mods.isRightButtonDown() || e.mods.isAltDown())
        {
            cc.erase (cc.begin() + dragIdx);
            proc.setCustomChords (std::move (cc));
            dragIdx = -1;
            return;
        }
        const auto& c = cc[(size_t) dragIdx];
        aio::Chord ch; ch.rootPc = c.rootPc; ch.iv = c.iv; ch.name = c.name;
        for (int n : voicedNotes (proc, ch)) proc.audition (aio::PartChords, n, 92, 650);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIdx < 0) return;
        if (e.getDistanceFromDragStart() > 6) dragging = true;
        if (dragging) { const int t = chipAt (e.getPosition()); if (t >= 0) dropIdx = t; repaint(); }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging && dragIdx >= 0 && dropIdx >= 0 && dropIdx != dragIdx)
        {
            auto cc = proc.getCustomChords();
            const auto moved = cc[(size_t) dragIdx];
            cc.erase (cc.begin() + dragIdx);
            cc.insert (cc.begin() + std::min (dropIdx, (int) cc.size()), moved);
            proc.setCustomChords (std::move (cc));
        }
        dragging = false; dragIdx = -1; repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const int i = chipAt (e.getPosition());
        if (i < 0 || w.deltaY == 0.f) return;
        static const double lens[4] = { 1.0, 2.0, 4.0, 8.0 };
        auto cc = proc.getCustomChords();
        int li = 0;
        for (int k = 0; k < 4; ++k) if (std::abs (lens[k] - cc[(size_t) i].len) < 0.01) li = k;
        li = std::clamp (li + (w.deltaY > 0 ? 1 : -1), 0, 3);
        cc[(size_t) i].len = lens[li];
        proc.setCustomChords (std::move (cc));
    }

private:
    juce::Rectangle<int> chipRect (int i) const
    {
        const int w = 70, h = 44, gap = 6;
        const int perRow = std::max (1, (getWidth() - 16) / (w + gap));
        return { 8 + (i % perRow) * (w + gap), 38 + (i / perRow) * (h + gap), w, h };
    }
    int chipAt (juce::Point<int> p) const
    {
        const int n = (int) proc.getCustomChords().size();
        for (int i = 0; i < n; ++i) if (chipRect (i).contains (p)) return i;
        return -1;
    }
    void timerCallback() override
    {
        if (! isShowing()) return;
        const int n = (int) proc.getCustomChords().size();
        if (n != lastCount) { lastCount = n; repaint(); }
    }

    AIOProcessor& proc;
    Toggle useMine;
    juce::TextButton harmBtn, clearBtn;
    juce::String status;
    int dragIdx = -1, dropIdx = -1, lastCount = -1;
    bool dragging = false;
};

// =================================================================================================
// One row of "suggestion" controls: on/off, previous idea, next idea, lock (freeze) for every part
// =================================================================================================
class PartStrip : public juce::Component, private juce::Timer
{
public:
    explicit PartStrip (AIOProcessor& p) : proc (p)
    {
        static const int order[5] = { aio::PartChords, aio::PartDrums, aio::PartBass, aio::PartMelody, aio::PartSynth };
        static const char* ids[5] = { "cmpChordsOn", "cmpMelOn", "cmpBassOn", "cmpDrumsOn", "cmpSynOn" };
        for (int i = 0; i < 5; ++i)
        {
            auto& g = groups[(size_t) i];
            const int part = order[i];
            g.part = part;
            g.on = std::make_unique<Toggle> (p.apvts, ids[part], juce::String (partName (part)).toUpperCase(), partColour (part));
            g.prev.setButtonText ("<");  g.prev.setTooltip ("Previous idea");
            g.next.setButtonText ("New idea >"); g.next.setTooltip ("Suggest another " + juce::String (partName (part)).toLowerCase() + " pattern");
            g.lock.setButtonText ("Lock");
            g.lock.setTooltip ("Lock keeps this pattern while you change other settings. Unlock brings back suggestions.");
            g.prev.onClick = [this, part] { proc.suggestPart (part, -1); };
            g.next.onClick = [this, part] { proc.suggestPart (part, +1); };
            g.lock.onClick = [this, part]
            {
                if (proc.isPartLocked (part)) proc.unlockPart (part);
                else proc.setPartNotes (part, proc.getPartNotes (part));
            };
            addAndMakeVisible (*g.on); addAndMakeVisible (g.prev); addAndMakeVisible (g.next); addAndMakeVisible (g.lock);
        }
        startTimerHz (8);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        const int w = b.getWidth() / 5;
        for (auto& g : groups)
        {
            auto r = b.removeFromLeft (w).reduced (3, 2);
            g.on->setBounds (r.removeFromLeft (92));
            g.prev.setBounds (r.removeFromLeft (26)); r.removeFromLeft (2);
            g.lock.setBounds (r.removeFromRight (48)); r.removeFromRight (2);
            g.next.setBounds (r);
        }
    }

    void paint (juce::Graphics& g) override
    {
        const int w = getWidth() / 5;
        for (int i = 0; i < 5; ++i)
        {
            const auto r = juce::Rectangle<float> ((float) (i * w) + 1.f, 0.f, (float) w - 2.f, (float) getHeight());
            g.setColour (partColour (groups[(size_t) i].part).withAlpha (0.1f));
            g.fillRoundedRectangle (r, 6.f);
            g.setColour (partColour (groups[(size_t) i].part).withAlpha (0.5f));
            g.drawRoundedRectangle (r.reduced (0.5f), 6.f, 1.f);
        }
    }

private:
    void timerCallback() override
    {
        if (! isShowing()) return;
        for (auto& g : groups)
        {
            const bool lk = proc.isPartLocked (g.part);
            if (lk != g.lock.getToggleState())
            {
                g.lock.setToggleState (lk, juce::dontSendNotification);
                g.lock.setButtonText (lk ? "Locked" : "Lock");
            }
        }
    }

    struct Group
    {
        int part = 0;
        std::unique_ptr<Toggle> on;
        juce::TextButton prev, next, lock;
    };
    AIOProcessor& proc;
    std::array<Group, 5> groups;
};

// =================================================================================================
// Editable piano roll with harmonic layers (root / chord tone / in scale), one part edited at a time
// =================================================================================================
class EditableRoll : public juce::Component, private juce::Timer
{
public:
    explicit EditableRoll (AIOProcessor& p) : proc (p)
    {
        for (int i = 0; i < 4; ++i)
        {
            auto* b = partBtns.add (new juce::TextButton (partName (kEditParts[i])));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (7001);
            b->setColour (juce::TextButton::buttonOnColourId, partColour (kEditParts[i]).withAlpha (0.75f));
            b->onClick = [this, i] { editPart = kEditParts[i]; repaint(); };
            addAndMakeVisible (b);
        }
        partBtns[1]->setToggleState (true, juce::dontSendNotification);

        snapBox.addItem ("Snap 1/4", 1); snapBox.addItem ("Snap 1/8", 2); snapBox.addItem ("Snap 1/16", 3);
        snapBox.setSelectedId (2, juce::dontSendNotification);
        scaleSnap.setButtonText ("Stay in scale");
        scaleSnap.setToggleState (true, juce::dontSendNotification);
        scaleSnap.setColour (juce::ToggleButton::tickColourId, col::violet);
        clearBtn.setButtonText ("Clear");
        lockBtn.setButtonText ("Freeze");
        lockBtn.setTooltip ("Editing a part freezes it automatically. Unlock goes back to generated suggestions.");
        clearBtn.onClick = [this] { proc.setPartNotes (editPart, {}); };
        lockBtn.onClick = [this]
        {
            if (proc.isPartLocked (editPart)) proc.unlockPart (editPart);
            else proc.setPartNotes (editPart, proc.getPartNotes (editPart));
        };
        for (auto* c : std::initializer_list<juce::Component*> { &snapBox, &scaleSnap, &clearBtn, &lockBtn }) addAndMakeVisible (c);
        startTimerHz (30);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (6, 4);
        auto top = b.removeFromTop (24);
        for (auto* pb : partBtns) { pb->setBounds (top.removeFromLeft (66)); top.removeFromLeft (3); }
        top.removeFromLeft (8);
        snapBox.setBounds (top.removeFromLeft (92)); top.removeFromLeft (6);
        scaleSnap.setBounds (top.removeFromLeft (110));
        lockBtn.setBounds (top.removeFromRight (70)); top.removeFromRight (4);
        clearBtn.setBounds (top.removeFromRight (54));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (col::bg.brighter (0.03f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.f);

        auto pat = proc.getPatternForUI();
        if (! pat) return;
        L = pat->lengthBeats;
        layoutArea();
        if (! dragging) computeRange (*pat);

        const int key = (int) param (proc, "cmpKey");
        const auto& sd = aio::scaleDefs()[(size_t) std::clamp ((int) param (proc, "cmpScale"), 0, 11)];
        bool inScale[12] = {};
        for (int i = 0; i < sd.n; ++i) inScale[(key + sd.iv[i]) % 12] = true;

        // ---- rows + harmonic layers
        for (int p = lo; p <= hi; ++p)
        {
            const int pc = p % 12;
            const float y = yOf (p);
            const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
            g.setColour (black ? col::panel.withAlpha (0.6f) : col::panel.withAlpha (0.25f));
            g.fillRect (area.getX(), y, area.getWidth(), rowH);
            if (inScale[pc]) { g.setColour (juce::Colours::white.withAlpha (0.04f)); g.fillRect (area.getX(), y, area.getWidth(), rowH); }
            for (const auto& c : pat->chords)
            {
                const bool isRoot = pc == c.rootPc;
                const bool isTone = std::find (c.pcs.begin(), c.pcs.end(), pc) != c.pcs.end();
                if (! isTone) continue;
                g.setColour (isRoot ? col::violet.withAlpha (0.32f) : col::chords.withAlpha (0.16f));
                g.fillRect (xOf (c.start), y, xOf (c.start + c.len) - xOf (c.start), rowH);
            }
            if (pc == 0)
            {
                g.setColour (col::dim);
                g.setFont (juce::Font (juce::FontOptions (10.f)));
                g.drawText ("C" + juce::String (p / 12 - 1), (int) (area.getX() - 34), (int) y - 2, 30, (int) rowH + 4, juce::Justification::centredRight);
            }
        }

        // ---- beat / bar lines
        for (double b = 0; b <= L + 1e-6; b += 1.0)
        {
            const float x = xOf (b);
            const bool bar = std::fmod (b, 4.0) < 1e-6;
            g.setColour (bar ? col::edge.brighter (0.5f) : col::edge.withAlpha (0.45f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }

        // ---- chord names
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        for (const auto& c : pat->chords)
        {
            g.setColour (col::violet);
            g.drawText (c.name, (int) xOf (c.start) + 4, (int) (area.getY() - 17), (int) (xOf (c.start + c.len) - xOf (c.start)) - 4, 16, juce::Justification::centredLeft);
        }

        // ---- notes (other parts faded, the edited part solid)
        for (const auto& n : pat->notes)
        {
            if (n.part == aio::PartDrums || n.part == editPart) continue;
            g.setColour (partColour (n.part).withAlpha (0.22f));
            g.fillRoundedRectangle (xOf (n.start), yOf (n.pitch) + 0.5f, std::max (2.f, xOf (n.start + n.len) - xOf (n.start) - 1.f), std::max (2.5f, rowH - 1.f), 2.f);
        }
        const auto& edited = dragging ? work : liveNotes (*pat);
        for (size_t i = 0; i < edited.size(); ++i)
        {
            const auto& n = edited[i];
            const auto rc = noteRect (n);
            g.setColour (partColour (editPart).withAlpha (0.4f + 0.6f * (float) n.vel / 127.f));
            g.fillRoundedRectangle (rc, 2.f);
            if (dragging && (int) i == dragIdx) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (rc, 2.f, 1.2f); }
        }

        if (proc.composeRunning.load())
        {
            g.setColour (col::text);
            g.drawVerticalLine ((int) xOf (std::fmod (proc.uiBeat.load(), L)), area.getY(), area.getBottom());
        }

        g.setColour (proc.isPartLocked (editPart) ? col::accent2 : col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        g.drawText (proc.isPartLocked (editPart) ? "edited / locked" : "click to draw, drag to move, right-click to delete",
                    getLocalBounds().removeFromBottom (16).reduced (10, 0), juce::Justification::centredRight);
    }

    // centre of a note cell in component coordinates (valid after the first paint; used by the tests)
    juce::Point<float> pointFor (double beat, int pitch) const { return { xOf (beat), yOf (pitch) + rowH * 0.5f }; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto pat = proc.getPatternForUI();
        if (! pat || ! area.contains (e.position)) return;
        L = pat->lengthBeats;
        work = proc.getPartNotes (editPart);
        dragIdx = hit (e.position);
        if (e.mods.isRightButtonDown() || e.mods.isAltDown())
        {
            if (dragIdx >= 0) { work.erase (work.begin() + dragIdx); proc.setPartNotes (editPart, work); }
            dragIdx = -1; return;
        }
        dragging = true;
        const double beat = beatAt (e.position.x);
        if (dragIdx >= 0)
        {
            const auto& n = work[(size_t) dragIdx];
            if (std::abs (e.position.x - xOf (n.start + n.len)) < 7.f && xOf (n.start + n.len) - xOf (n.start) > 16.f) mode = Resize;
            else { mode = Move; grab = beat - n.start; }
            held = n.pitch;
        }
        else
        {
            aio::NoteEvent n;
            n.start = std::clamp (std::floor (beat / snap() + 1e-9) * snap(), 0.0, L - snap());
            n.len = snap();
            n.pitch = conform (pitchAt (e.position.y));
            n.vel = editPart == aio::PartBass ? 100 : editPart == aio::PartChords ? 85 : 95;
            n.part = editPart;
            work.push_back (n);
            dragIdx = (int) work.size() - 1;
            mode = Create; held = n.pitch;
        }
        proc.audition (editPart, held, 90, 0);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging || dragIdx < 0) return;
        auto& n = work[(size_t) dragIdx];
        const double beat = beatAt (e.position.x);
        if (mode == Move)
        {
            n.start = std::clamp (std::round ((beat - grab) / snap()) * snap(), 0.0, std::max (0.0, L - n.len));
            const int np = conform (pitchAt (e.position.y));
            if (np != n.pitch)
            {
                proc.audition (editPart, held, 0, -1);
                n.pitch = np; held = np;
                proc.audition (editPart, held, 90, 0);
            }
        }
        else
        {
            const double end = std::round (beat / snap()) * snap();
            n.len = std::clamp (end - n.start, snap(), std::max (snap(), L - n.start));
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! dragging) return;
        proc.audition (editPart, held, 0, -1);
        dragging = false;
        if (dragIdx >= 0) proc.setPartNotes (editPart, work);
        dragIdx = -1;
    }

private:
    static constexpr int kEditParts[4] = { aio::PartChords, aio::PartMelody, aio::PartBass, aio::PartSynth };
    enum Mode { Move, Resize, Create };

    double snap() const { const int id = snapBox.getSelectedId(); return id == 1 ? 1.0 : id == 3 ? 0.25 : 0.5; }

    void layoutArea()
    {
        auto b = getLocalBounds().toFloat().reduced (6.f, 4.f);
        b.removeFromTop (28.f);
        b.removeFromBottom (16.f);
        b.removeFromTop (18.f);       // chord names
        b.removeFromLeft (36.f);      // note names
        area = b;
        pxPerBeat = L > 0 ? area.getWidth() / (float) L : 1.f;
        rowH = area.getHeight() / (float) std::max (1, hi - lo + 1);
    }

    void computeRange (const aio::Pattern& pat)
    {
        int mn = 127, mx = 0;
        for (const auto& n : pat.notes) if (n.part != aio::PartDrums) { mn = std::min (mn, n.pitch); mx = std::max (mx, n.pitch); }
        if (mn > mx) { mn = 60; mx = 72; }
        lo = mn - 3; hi = mx + 3;
        if (hi - lo < 29) { const int mid = (lo + hi) / 2; lo = mid - 14; hi = lo + 29; }
        lo = std::max (0, lo); hi = std::min (127, hi);
        rowH = area.getHeight() / (float) std::max (1, hi - lo + 1);
    }

    const std::vector<aio::NoteEvent>& liveNotes (const aio::Pattern& pat)
    {
        cache.clear();
        for (const auto& n : pat.notes) if (n.part == editPart) cache.push_back (n);
        return cache;
    }

    float xOf (double beat) const { return area.getX() + (float) beat * pxPerBeat; }
    float yOf (int pitch) const { return area.getBottom() - (float) (pitch - lo + 1) * rowH; }
    double beatAt (float x) const { return std::clamp ((double) ((x - area.getX()) / pxPerBeat), 0.0, L); }
    int pitchAt (float y) const { return std::clamp (lo + (int) std::floor ((area.getBottom() - y) / rowH), 0, 127); }
    juce::Rectangle<float> noteRect (const aio::NoteEvent& n) const
    {
        return { xOf (n.start), yOf (n.pitch) + 0.5f, std::max (3.f, xOf (n.start + n.len) - xOf (n.start) - 1.f), std::max (3.f, rowH - 1.f) };
    }
    int hit (juce::Point<float> p) const
    {
        for (int i = (int) work.size() - 1; i >= 0; --i)
        {
            auto r = noteRect (work[(size_t) i]);
            r = r.withTrimmedTop (-1.f).withTrimmedBottom (-1.f).withTrimmedRight (-4.f);
            if (r.contains (p)) return i;
        }
        return -1;
    }

    int conform (int pitch) const
    {
        if (! scaleSnap.getToggleState()) return pitch;
        const int key = (int) param (proc, "cmpKey");
        const auto& sd = aio::scaleDefs()[(size_t) std::clamp ((int) param (proc, "cmpScale"), 0, 11)];
        auto ok = [&] (int p)
        {
            const int rel = ((p - key) % 12 + 12) % 12;
            for (int i = 0; i < sd.n; ++i) if (sd.iv[i] == rel) return true;
            return false;
        };
        for (int d = 0; d <= 6; ++d)
        {
            if (ok (pitch - d)) return std::max (0, pitch - d);
            if (ok (pitch + d)) return std::min (127, pitch + d);
        }
        return pitch;
    }

    void timerCallback() override
    {
        if (! isShowing()) return;
        const bool lk = proc.isPartLocked (editPart);
        const char* want = lk ? "Unlock" : "Freeze";
        if (lockBtn.getButtonText() != want) lockBtn.setButtonText (want);
        repaint();
    }

    AIOProcessor& proc;
    juce::OwnedArray<juce::TextButton> partBtns;
    juce::ComboBox snapBox;
    juce::ToggleButton scaleSnap;
    juce::TextButton clearBtn, lockBtn;

    int editPart = aio::PartMelody;
    double L = 16.0;
    juce::Rectangle<float> area;
    float pxPerBeat = 1.f, rowH = 6.f;
    int lo = 48, hi = 77;

    std::vector<aio::NoteEvent> work, cache;
    bool dragging = false;
    int dragIdx = -1, held = 60;
    Mode mode = Move;
    double grab = 0.0;
};

// =================================================================================================
// Drum grid: 8 lanes x 16 steps, one bar at a time
// =================================================================================================
class DrumGrid : public juce::Component, private juce::Timer
{
public:
    explicit DrumGrid (AIOProcessor& p) : proc (p)
    {
        prevBtn.setButtonText ("<"); nextBtn.setButtonText (">");
        clearBtn.setButtonText ("Clear bar");
        prevBtn.onClick = [this] { bar = std::max (0, bar - 1); repaint(); };
        nextBtn.onClick = [this] { bar = std::min (bars() - 1, bar + 1); repaint(); };
        clearBtn.onClick = [this]
        {
            auto notes = proc.getPartNotes (aio::PartDrums);
            notes.erase (std::remove_if (notes.begin(), notes.end(), [this] (const aio::NoteEvent& n) { return barOf (n) == bar; }), notes.end());
            proc.setPartNotes (aio::PartDrums, notes);
        };
        for (auto* c : std::initializer_list<juce::Component*> { &prevBtn, &nextBtn, &clearBtn }) addAndMakeVisible (c);
        startTimerHz (20);
    }

    void resized() override
    {
        auto b = getLocalBounds().reduced (8, 6);
        auto top = b.removeFromTop (24);
        clearBtn.setBounds (top.removeFromRight (74));
        nextBtn.setBounds (top.removeFromRight (28)); top.removeFromRight (4);
        prevBtn.setBounds (top.removeFromRight (28));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (col::bg.brighter (0.03f));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 8.f);
        const int nb = bars();
        bar = std::clamp (bar, 0, nb - 1);

        g.setColour (col::accent2);
        g.setFont (juce::Font (juce::FontOptions (13.f, juce::Font::bold)));
        g.drawText ("DRUMS   bar " + juce::String (bar + 1) + " / " + juce::String (nb), 12, 8, 200, 20, juce::Justification::centredLeft);

        const auto cells = gridArea();
        const auto notes = proc.getPartNotes (aio::PartDrums);
        int vel[aio::kDrumLanes][16] = {};
        for (const auto& n : notes)
        {
            if (barOf (n) != bar) continue;
            const int step = stepOf (n);
            if (step < 0 || step > 15) continue;
            const int lane = aio::drumLaneForNote (n.pitch);
            vel[lane][step] = std::max (vel[lane][step], n.vel);
        }

        const int playStep = proc.composeRunning.load() && (int) std::floor (proc.uiBeat.load() / 4.0) == bar
                               ? (int) std::floor (std::fmod (proc.uiBeat.load(), 4.0) * 4.0) : -1;
        const float cw = cells.getWidth() / 16.f, ch = cells.getHeight() / (float) aio::kDrumLanes;
        for (int lane = 0; lane < aio::kDrumLanes; ++lane)
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (11.5f)));
            g.drawText (aio::drumLaneName (lane), (int) (cells.getX() - 72), (int) (cells.getY() + lane * ch), 68, (int) ch, juce::Justification::centredRight);
            for (int step = 0; step < 16; ++step)
            {
                const auto rc = juce::Rectangle<float> (cells.getX() + step * cw, cells.getY() + lane * ch, cw, ch).reduced (1.5f);
                const bool beatGroup = (step / 4) % 2 == 0;
                g.setColour (beatGroup ? col::panel.brighter (0.15f) : col::panel);
                g.fillRoundedRectangle (rc, 3.f);
                if (step == playStep) { g.setColour (juce::Colours::white.withAlpha (0.12f)); g.fillRoundedRectangle (rc, 3.f); }
                if (vel[lane][step] > 0)
                {
                    g.setColour (col::accent2.withAlpha (0.3f + 0.7f * (float) vel[lane][step] / 127.f));
                    g.fillRoundedRectangle (rc, 3.f);
                    if (vel[lane][step] >= 115) { g.setColour (juce::Colours::white.withAlpha (0.6f)); g.drawRoundedRectangle (rc, 3.f, 1.2f); }
                }
            }
        }
        g.setColour (proc.isPartLocked (aio::PartDrums) ? col::accent2 : col::dim);
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText (proc.isPartLocked (aio::PartDrums) ? "edited / locked" : "click a cell. shift = accent, alt or right-click = ghost",
                    getLocalBounds().removeFromBottom (16).reduced (10, 0), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto cells = gridArea();
        if (! cells.contains (e.position)) return;
        const int step = (int) ((e.position.x - cells.getX()) / (cells.getWidth() / 16.f));
        const int lane = (int) ((e.position.y - cells.getY()) / (cells.getHeight() / (float) aio::kDrumLanes));
        if (step < 0 || step > 15 || lane < 0 || lane >= aio::kDrumLanes) return;

        auto notes = proc.getPartNotes (aio::PartDrums);
        std::vector<size_t> here;
        for (size_t i = 0; i < notes.size(); ++i)
            if (barOf (notes[i]) == bar && stepOf (notes[i]) == step && aio::drumLaneForNote (notes[i].pitch) == lane) here.push_back (i);

        const bool accent = e.mods.isShiftDown(), ghost = e.mods.isAltDown() || e.mods.isRightButtonDown();
        const int wantVel = accent ? 122 : ghost ? 45 : 100;
        int played = wantVel;
        if (! here.empty() && ! accent && ! ghost)
        {
            for (auto it = here.rbegin(); it != here.rend(); ++it) notes.erase (notes.begin() + (long) *it);
            played = 0;
        }
        else if (! here.empty())
            for (size_t i : here) notes[i].vel = wantVel;
        else
        {
            aio::NoteEvent n;
            n.start = bar * 4.0 + step * 0.25;
            n.len = lane == 4 ? 0.4 : lane == 7 ? 1.0 : 0.12;
            n.pitch = aio::drumLaneNote (lane);
            n.vel = wantVel; n.part = aio::PartDrums;
            notes.push_back (n);
        }
        if (played > 0) proc.audition (aio::PartDrums, aio::drumLaneNote (lane), played, 1);
        proc.setPartNotes (aio::PartDrums, notes);
    }

private:
    int bars() const
    {
        auto pat = proc.getPatternForUI();
        return pat ? std::max (1, (int) std::lround (pat->lengthBeats / 4.0)) : 1;
    }
    static int barOf (const aio::NoteEvent& n) { return (int) std::floor ((n.start + 0.125) / 4.0); }
    int stepOf (const aio::NoteEvent& n) const { return (int) std::lround ((n.start - barOf (n) * 4.0) / 0.25); }

    juce::Rectangle<float> gridArea() const
    {
        auto b = getLocalBounds().toFloat().reduced (8.f, 6.f);
        b.removeFromTop (32.f);
        b.removeFromBottom (16.f);
        b.removeFromLeft (74.f);
        return b;
    }

    void timerCallback() override { if (isShowing()) repaint(); }

    AIOProcessor& proc;
    juce::TextButton prevBtn, nextBtn, clearBtn;
    int bar = 0;
};
} // namespace aio::ui
