#pragma once
#include "Widgets.h"
#include "../vocal/VocalEngine.h"

namespace aio::ui
{
inline juce::String noteLabel (double midi, bool withCents = false)
{
    const int n = (int) std::lround (midi);
    juce::String s = juce::String (aio::pcName (((n % 12) + 12) % 12)) + juce::String (n / 12 - 1);
    if (withCents)
    {
        const int c = (int) std::lround ((midi - n) * 100.0);
        if (c != 0) s << (c > 0 ? " +" : " ") << c << "c";
    }
    return s;
}

/*
   Melodyne-style note editor: every note of the vocal is a blob on a pitch / time grid.
     drag a blob up / down        : change its pitch (snaps to semitones or to the scale)
     drag it left / right         : move it in time (it can't pass its neighbours)
     drag its left / right edge   : stretch the note
     click empty space + drag     : box select      shift-click : add to selection
     double click                 : snap the note(s) to the grid
     right click                  : split / join / mute / snap / reset
     wheel                        : scroll time   ctrl + wheel: zoom time   shift + wheel: scroll pitch   alt + wheel: zoom pitch
     click the ruler              : move the playhead
*/
class VocalRoll : public juce::Component, private juce::Timer
{
public:
    explicit VocalRoll (AIOProcessor& p) : proc (p)
    {
        setWantsKeyboardFocus (true);
        startTimerHz (30);
    }
    ~VocalRoll() override { stopTimer(); }

    std::function<void()> onSelectionChanged, onEdited;

    static constexpr int keyW = 48, rulerH = 22;

    // ------------------------------------------------------------------ view
    void zoomToFit()
    {
        auto& d = vocal().doc();
        const double len = std::max (1.0, aio::editedLength (d.notes, d.clipSeconds));
        const int w = std::max (200, getWidth() - keyW);
        viewT0 = -0.05;
        pxPerSec = std::clamp ((double) w / std::min (len + 0.1, 14.0), 20.0, 600.0);
        float lo = 1000.f, hi = -1000.f;
        for (const auto& n : d.notes) { lo = std::min ({ lo, n.target, n.centre }); hi = std::max ({ hi, n.target, n.centre }); }
        if (hi < lo) { lo = 55.f; hi = 72.f; }
        const float span = std::max (20.f, hi - lo + 7.f);
        const int h = std::max (100, getHeight() - rulerH);
        pxPerSemi = std::clamp ((double) h / span, 7.0, 34.0);
        topMidi = (double) hi + 3.5 + std::max (0.0, ((double) h / pxPerSemi - span) * 0.5);
        repaint();
    }

    juce::Point<float> pointFor (double t, double midi) const { return { (float) (keyW + (t - viewT0) * pxPerSec), (float) (rulerH + (topMidi - midi) * pxPerSemi) }; }
    double timeAt (float x) const { return viewT0 + ((double) x - keyW) / pxPerSec; }
    double midiAt (float y) const { return topMidi - ((double) y - rulerH) / pxPerSemi; }

    // ------------------------------------------------------------------ selection
    const std::vector<char>& selection() const { return sel; }
    int selectedCount() const { int c = 0; for (auto v : sel) c += v ? 1 : 0; return c; }
    int firstSelected() const { for (size_t i = 0; i < sel.size(); ++i) if (sel[i]) return (int) i; return -1; }
    void selectAll()  { syncSel(); std::fill (sel.begin(), sel.end(), 1); selChanged(); }
    void selectNone() { syncSel(); std::fill (sel.begin(), sel.end(), 0); selChanged(); }
    void selectOnly (int i) { syncSel(); std::fill (sel.begin(), sel.end(), 0); if (i >= 0 && (size_t) i < sel.size()) sel[(size_t) i] = 1; selChanged(); }

    // ------------------------------------------------------------------ operations (each one is a single undo step)
    void beginEdit() { vocal().doc().checkpoint(); }
    void applyToSelected (const std::function<void (aio::VocalNote&)>& fn)
    {
        syncSel();
        auto& notes = vocal().doc().notes;
        for (size_t i = 0; i < notes.size(); ++i) if (sel[i]) fn (notes[i]);
    }
    void commit() { vocal().notesChanged(); repaint(); if (onEdited) onEdited(); }

    void editSelected (const std::function<void (aio::VocalNote&)>& fn)
    {
        if (selectedCount() == 0) return;
        beginEdit(); applyToSelected (fn); commit();
    }
    void snapSelected (int mode)           // 1 semitone, 2 scale
    {
        const auto mask = proc.getEffectiveTunerMask();
        editSelected ([mode, mask] (aio::VocalNote& n)
                      { n.target = (float) aio::nearestAllowedMidi ((double) n.target, mode == 2 ? mask : (uint16_t) 0x0FFF); });
    }
    void toggleMute()
    {
        bool anyOn = false;
        { syncSel(); auto& notes = vocal().doc().notes; for (size_t i = 0; i < notes.size(); ++i) if (sel[i] && ! notes[i].muted) anyOn = true; }
        editSelected ([anyOn] (aio::VocalNote& n) { n.muted = anyOn; });
    }
    void resetSelected()
    {
        if (selectedCount() == 0) return;
        beginEdit();
        auto& d = vocal().doc();
        for (size_t i = 0; i < d.notes.size(); ++i) if (sel[i]) aio::resetNote (d.notes, (int) i, maxTime());
        commit();
    }
    bool splitAt (double tNew)
    {
        auto& d = vocal().doc();
        const int i = aio::noteAtNewTime (d.notes, tNew);
        if (i < 0) return false;
        syncSel();
        d.checkpoint();
        if (! aio::splitNote (d.notes, i, tNew, d.sr, d.hop)) { d.undoStack.pop_back(); return false; }
        sel.insert (sel.begin() + i + 1, sel[(size_t) i]);
        commit(); selChanged();
        return true;
    }
    bool splitAtPlayhead() { return splitAt (vocal().playheadSeconds()); }
    bool joinSelected()
    {
        auto& d = vocal().doc();
        syncSel();
        int target = -1;
        for (size_t i = 0; i + 1 < d.notes.size(); ++i) if (sel[i] && sel[i + 1]) { target = (int) i; break; }
        if (target < 0) target = firstSelected();
        if (target < 0) return false;
        d.checkpoint();
        if (! aio::mergeWithNext (d.notes, target)) { d.undoStack.pop_back(); return false; }
        sel.erase (sel.begin() + target + 1);
        sel[(size_t) target] = 1;
        commit(); selChanged();
        return true;
    }
    bool undo() { return stepHistory (true); }
    bool redo() { return stepHistory (false); }

    int hitNote (juce::Point<float> p) const
    {
        const auto& notes = vocal().doc().notes;
        for (int i = (int) notes.size() - 1; i >= 0; --i)
            if (hitRect (notes[(size_t) i]).contains (p)) return i;
        return -1;
    }
    juce::Rectangle<float> blobRect (const aio::VocalNote& n) const
    {
        const auto a = pointFor (n.nt0, n.target), b = pointFor (n.nt1, n.target);
        const float h = std::max (9.f, (float) pxPerSemi * 0.9f);
        return { a.x, a.y - h * 0.5f, std::max (3.f, b.x - a.x), h };
    }

    // ------------------------------------------------------------------ painting
    void paint (juce::Graphics& g) override
    {
        auto& v = vocal();
        const auto& d = v.doc();
        const int W = getWidth(), H = getHeight();
        g.fillAll (col::bg.darker (0.2f));
        const juce::Rectangle<int> grid (keyW, rulerH, W - keyW, H - rulerH);

        if (! v.hasClip())
        {
            g.setColour (col::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 6.f);
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (15.f)));
            juce::String msg = v.busy() ? v.status() + "  " + juce::String (juce::roundToInt (v.progress() * 100.f)) + "%"
                                        : "Load a vocal (WAV / MP3 / FLAC) or press RECORD and sing or speak.\nEvery note shows up here as a blob you can drag.";
            if (v.recMode() == aio::VocalSession::RecRecording) msg = "Recording...  " + juce::String (v.recSeconds(), 1) + " s";
            if (v.recMode() == aio::VocalSession::RecArmed) msg = "Armed - press play in your DAW to start recording";
            g.drawFittedText (msg, getLocalBounds().reduced (30), juce::Justification::centred, 4);
            return;
        }

        // ---- pitch rows
        const auto mask = proc.getEffectiveTunerMask();
        const int mTop = (int) std::ceil (topMidi) + 1, mBot = (int) std::floor (midiAt ((float) H)) - 1;
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (juce::Rectangle<int> (0, rulerH, W, H - rulerH));
            for (int m = mBot; m <= mTop; ++m)
            {
                const int pc = ((m % 12) + 12) % 12;
                const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
                const bool inScale = (mask & (1u << pc)) != 0 && mask != 0x0FFF;
                const float y0 = pointFor (0, m + 0.5).y, y1 = pointFor (0, m - 0.5).y;
                g.setColour (black ? col::bg.darker (0.35f) : col::panel.withAlpha (0.55f));
                g.fillRect (juce::Rectangle<float> ((float) keyW, y0, (float) (W - keyW), y1 - y0));
                if (inScale) { g.setColour (col::accent.withAlpha (0.07f)); g.fillRect (juce::Rectangle<float> ((float) keyW, y0, (float) (W - keyW), y1 - y0)); }
                g.setColour (col::edge.withAlpha (pc == 0 ? 0.9f : 0.35f));
                g.drawHorizontalLine ((int) y1, (float) keyW, (float) W);
                // key column
                g.setColour (black ? juce::Colour (0xff15171c) : juce::Colour (0xffd9dde6));
                g.fillRect (juce::Rectangle<float> (0.f, y0 + 0.5f, black ? (float) keyW * 0.6f : (float) keyW - 1.f, y1 - y0 - 1.f));
                if (pc == 0 || pxPerSemi > 20.0)
                {
                    g.setColour (pc == 0 ? juce::Colour (0xff15171c) : juce::Colour (0xff6c7385));
                    g.setFont (juce::Font (juce::FontOptions (10.5f)));
                    g.drawText (noteLabel (m), 2, (int) y0, keyW - 6, (int) (y1 - y0), juce::Justification::centredRight);
                }
            }
        }

        // ---- time ruler + grid
        {
            double step = 1.0;
            for (double s : { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0 }) { step = s; if (s * pxPerSec >= 70.0) break; }
            g.setColour (col::panel);
            g.fillRect (0, 0, W, rulerH);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            for (double t = std::floor (viewT0 / step) * step; t < timeAt ((float) W); t += step)
            {
                const float x = pointFor (t, 0).x;
                if (x < keyW) continue;
                g.setColour (col::edge.withAlpha (0.6f));
                g.drawVerticalLine ((int) x, (float) rulerH, (float) H);
                g.setColour (col::dim);
                g.drawText (step < 1.0 ? juce::String (t, 2) + "s" : mmssText (t), (int) x + 3, 0, 60, rulerH, juce::Justification::centredLeft);
            }
        }

        // ---- notes
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (grid);
            syncSel();
            for (size_t i = 0; i < d.notes.size(); ++i) paintNote (g, d.notes[i], sel[i] != 0);
            if (drag == Band)
            {
                g.setColour (col::accent.withAlpha (0.12f)); g.fillRect (band);
                g.setColour (col::accent.withAlpha (0.8f)); g.drawRect (band, 1.f);
            }
        }

        // ---- playhead
        {
            const double ph = v.playheadSeconds();
            const float x = pointFor (ph, 0).x;
            if (x >= keyW && x <= W && (v.isPlaying() || ph > 0.0))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawVerticalLine ((int) x, 0.f, (float) H);
                juce::Path tri; tri.addTriangle (x - 5.f, 0.f, x + 5.f, 0.f, x, 9.f);
                g.fillPath (tri);
            }
        }
        if (v.busy())
        {
            g.setColour (juce::Colours::black.withAlpha (0.5f)); g.fillRect (grid);
            g.setColour (col::text); g.setFont (juce::Font (juce::FontOptions (15.f)));
            g.drawText (v.status() + "  " + juce::String (juce::roundToInt (v.progress() * 100.f)) + "%", grid, juce::Justification::centred);
        }
        else if (v.rendering())
        {
            g.setColour (col::accent2); g.setFont (juce::Font (juce::FontOptions (11.f)));
            g.drawText ("updating...", W - 90, rulerH + 4, 84, 14, juce::Justification::centredRight);
        }
    }

    // ------------------------------------------------------------------ mouse
    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = hitNote (e.position);
        if (i < 0) { setMouseCursor (juce::MouseCursor::NormalCursor); return; }
        const auto r = blobRect (vocal().doc().notes[(size_t) i]);
        setMouseCursor ((e.position.x - r.getX() < 6.f || r.getRight() - e.position.x < 6.f) ? juce::MouseCursor::LeftRightResizeCursor
                                                                                           : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        auto& v = vocal();
        if (! v.hasClip()) return;
        drag = None; dragMoved = false; dragCheckpointed = false;
        syncSel();

        if (e.position.y < rulerH && e.position.x >= keyW)
        {
            drag = Seek;
            v.seekSeconds (std::max (0.0, timeAt (e.position.x)));
            repaint();
            return;
        }
        const int hit = hitNote (e.position);
        if (e.mods.isPopupMenu())
        {
            if (hit >= 0 && ! sel[(size_t) hit]) selectOnly (hit);
            showMenu (e.position);
            return;
        }
        if (hit >= 0)
        {
            if (e.mods.isShiftDown()) { sel[(size_t) hit] = sel[(size_t) hit] ? 0 : 1; selChanged(); if (! sel[(size_t) hit]) return; }
            else if (! sel[(size_t) hit]) selectOnly (hit);

            const auto& n = v.doc().notes[(size_t) hit];
            const auto r = blobRect (n);
            drag = (e.position.x - r.getX() < 6.f && selectedCount() == 1) ? ResizeL
                 : (r.getRight() - e.position.x < 6.f && selectedCount() == 1) ? ResizeR : Move;
            dragNote = hit;
            dragOrigin = e.position;
            dragStart = v.doc().notes;
            if (e.getNumberOfClicks() < 2) v.auditionRange (n.nt0, std::min (n.nt1, n.nt0 + 1.5));
            repaint();
            return;
        }
        if (! e.mods.isShiftDown()) selectNone();
        drag = Band; dragOrigin = e.position; band = { e.position, e.position };
        selBefore = sel;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& v = vocal();
        if (! v.hasClip()) return;
        if (drag == Seek) { v.seekSeconds (std::max (0.0, timeAt (e.position.x))); repaint(); return; }
        if (drag == Band)
        {
            band = juce::Rectangle<float> (dragOrigin, e.position);
            syncSel();
            sel = e.mods.isShiftDown() ? selBefore : std::vector<char> (sel.size(), 0);
            sel.resize (v.doc().notes.size(), 0);
            for (size_t i = 0; i < v.doc().notes.size(); ++i)
                if (band.intersects (blobRect (v.doc().notes[i]))) sel[i] = 1;
            selChanged();
            repaint();
            return;
        }
        if (drag == None) return;

        const float dx = e.position.x - dragOrigin.x, dy = e.position.y - dragOrigin.y;
        if (! dragMoved && std::abs (dx) < 3.f && std::abs (dy) < 3.f) return;
        if (! dragCheckpointed) { v.doc().notes = dragStart; v.doc().checkpoint(); dragCheckpointed = true; }
        dragMoved = true;

        auto& notes = v.doc().notes;
        for (size_t i = 0; i < notes.size() && i < dragStart.size(); ++i) if (sel[i]) notes[i] = dragStart[i];

        if (drag == Move)
        {
            const double dt = std::abs (dx) < 6.f ? 0.0 : (double) dx / pxPerSec;
            double dm = std::abs (dy) < 4.f ? 0.0 : (double) (-dy) / pxPerSemi;
            if (dm != 0.0)
            {
                const double want = snapMidi ((double) dragStart[(size_t) dragNote].target + dm, e.mods.isAltDown());
                dm = want - (double) dragStart[(size_t) dragNote].target;
            }
            std::vector<int> idx;
            for (size_t i = 0; i < notes.size(); ++i) if (sel[i]) idx.push_back ((int) i);
            if (dt > 0) std::reverse (idx.begin(), idx.end());
            for (int i : idx)
            {
                notes[(size_t) i].target = dragStart[(size_t) i].target + (float) dm;
                if (dt != 0.0) aio::moveNote (notes, i, dragStart[(size_t) i].nt0 + dt, maxTime());
            }
        }
        else if (drag == ResizeL) aio::setNoteStart (notes, dragNote, dragStart[(size_t) dragNote].nt0 + (double) dx / pxPerSec, maxTime());
        else if (drag == ResizeR) aio::setNoteEnd (notes, dragNote, dragStart[(size_t) dragNote].nt1 + (double) dx / pxPerSec, maxTime());

        const auto now = juce::Time::getMillisecondCounter();
        if (now - lastRender > 180) { v.notesChanged(); lastRender = now; }
        if (onEdited) onEdited();
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag == Move || drag == ResizeL || drag == ResizeR)
        {
            if (dragMoved) commit();
        }
        drag = None;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int hit = hitNote (e.position);
        if (hit < 0) return;
        if (! sel[(size_t) hit]) selectOnly (hit);
        snapSelected ((int) proc.apvts.getRawParameterValue ("vedSnap")->load() == 2 ? 2 : 1);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const double dy = w.deltaY != 0.f ? (double) w.deltaY : (double) w.deltaX;
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            const double tMouse = timeAt (e.position.x);
            pxPerSec = std::clamp (pxPerSec * std::exp (dy * 0.9), 6.0, 1500.0);
            viewT0 = tMouse - ((double) e.position.x - keyW) / pxPerSec;
        }
        else if (e.mods.isAltDown())
        {
            const double mMouse = midiAt (e.position.y);
            pxPerSemi = std::clamp (pxPerSemi * std::exp (dy * 0.7), 6.0, 60.0);
            topMidi = mMouse + ((double) e.position.y - rulerH) / pxPerSemi;
        }
        else if (e.mods.isShiftDown()) topMidi += dy * 6.0;
        else viewT0 -= dy * 0.25 * (double) (getWidth() - keyW) / pxPerSec;
        viewT0 = std::max (viewT0, -2.0);
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto m = k.getModifiers();
        const int c = k.getKeyCode();
        if (c == juce::KeyPress::deleteKey || c == juce::KeyPress::backspaceKey) { toggleMute(); return true; }
        if ((m.isCommandDown() || m.isCtrlDown()) && (c == 'z' || c == 'Z')) { if (m.isShiftDown()) redo(); else undo(); return true; }
        if ((m.isCommandDown() || m.isCtrlDown()) && (c == 'y' || c == 'Y')) { redo(); return true; }
        if ((m.isCommandDown() || m.isCtrlDown()) && (c == 'a' || c == 'A')) { selectAll(); return true; }
        if (c == 's' || c == 'S') { splitAtPlayhead(); return true; }
        return false;
    }

    void resized() override { if (! fitted && getWidth() > 100 && vocal().hasClip()) { fitted = true; zoomToFit(); } }
    void clipChanged() { fitted = false; sel.clear(); resized(); if (! fitted) zoomToFit(); selChanged(); }

private:
    enum DragKind { None, Move, ResizeL, ResizeR, Band, Seek };

    aio::VocalSession& vocal() const { return proc.getVocal(); }
    double maxTime() const { return vocal().doc().clipSeconds * 2.0 + 30.0; }
    void syncSel() { const size_t n = vocal().doc().notes.size(); if (sel.size() != n) sel.assign (n, 0); }
    void selChanged() { repaint(); if (onSelectionChanged) onSelectionChanged(); }

    static juce::String mmssText (double t)
    {
        const int s = (int) std::floor (std::max (0.0, t));
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    juce::Rectangle<float> hitRect (const aio::VocalNote& n) const
    {
        auto r = blobRect (n);
        if (r.getHeight() < 16.f) r = r.withSizeKeepingCentre (r.getWidth(), 16.f);
        return r;
    }

    double snapMidi (double m, bool alt) const
    {
        if (alt) return m;
        switch ((int) proc.apvts.getRawParameterValue ("vedSnap")->load())
        {
            case 1:  return std::round (m);
            case 2:  return (double) aio::nearestAllowedMidi (m, proc.getEffectiveTunerMask());
            default: return m;
        }
    }

    bool stepHistory (bool back)
    {
        auto& d = vocal().doc();
        const bool ok = back ? d.undo() : d.redo();
        if (! ok) return false;
        sel.assign (d.notes.size(), 0);
        vocal().notesChanged();
        selChanged();
        if (onEdited) onEdited();
        return true;
    }

    void paintNote (juce::Graphics& g, const aio::VocalNote& n, bool selected) const
    {
        const auto r = blobRect (n);
        if (r.getRight() < keyW || r.getX() > getWidth()) return;

        // ghost of where it came from
        if (n.pitchEdited() || n.timeEdited())
        {
            const auto a = pointFor (n.t0, n.centre), b = pointFor (n.t1, n.centre);
            const float h = r.getHeight();
            g.setColour (col::dim.withAlpha (0.35f));
            g.drawRoundedRectangle (a.x, a.y - h * 0.5f, std::max (3.f, b.x - a.x), h, 3.f, 1.f);
        }

        const bool pitchEd = n.pitchEdited();
        const float lvl = juce::jlimit (0.f, 1.f, (n.level + 50.f) / 40.f);
        juce::Colour base = n.muted ? col::dim.withAlpha (0.25f) : (pitchEd ? col::accent2 : col::accent).withAlpha (0.35f + 0.5f * lvl);
        g.setColour (base);
        g.fillRoundedRectangle (r, 4.f);
        g.setColour (n.muted ? col::dim : (pitchEd ? col::accent2 : col::accent).brighter (0.2f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.f, selected ? 2.2f : 1.f);
        if (selected) { g.setColour (juce::Colours::white.withAlpha (0.9f)); g.drawRoundedRectangle (r.expanded (1.5f), 5.f, 1.2f); }

        // pitch curve inside the blob
        if (n.curve && ! n.curve->midi.empty() && r.getWidth() > 10.f)
        {
            const auto& cv = *n.curve;
            const int L = (int) cv.midi.size();
            const int stride = std::max (1, L / std::max (8, (int) (r.getWidth() / 2.f)));
            const double hop = vocal().doc().hop, sr = vocal().doc().sr;
            juce::Path pOut, pIn;
            for (int k = 0; k < L; k += stride)
            {
                const double tOrig = ((double) (cv.f0 + k) * hop) / sr;
                const double tNew = aio::origToNew (n, tOrig);
                const float x = pointFor (tNew, 0).x;
                const float yOut = pointFor (0, (double) n.target + (double) (n.drift * cv.slow[(size_t) k] + n.vib * cv.fast[(size_t) k])).y;
                const float yIn = pointFor (0, (double) cv.midi[(size_t) k]).y;
                if (k == 0) { pOut.startNewSubPath (x, yOut); pIn.startNewSubPath (x, yIn); }
                else        { pOut.lineTo (x, yOut); pIn.lineTo (x, yIn); }
            }
            if (pitchEd) { g.setColour (juce::Colours::white.withAlpha (0.25f)); g.strokePath (pIn, juce::PathStrokeType (1.f)); }
            g.setColour (n.muted ? col::dim : juce::Colours::white.withAlpha (0.9f));
            g.strokePath (pOut, juce::PathStrokeType (1.4f));
        }

        if (r.getWidth() > 44.f && r.getHeight() >= 9.f)
        {
            g.setColour (juce::Colour (0xff0e1014).withAlpha (0.9f));
            g.setFont (juce::Font (juce::FontOptions (juce::jmin (11.f, r.getHeight() - 1.f), juce::Font::bold)));
            g.drawText (noteLabel (n.target, true) + (n.muted ? "  (muted)" : ""), r.reduced (4.f, 0.f), juce::Justification::topLeft);
        }
    }

    void showMenu (juce::Point<float> at)
    {
        juce::PopupMenu m;
        const double tAt = timeAt (at.x);
        m.addItem (1, "Split here");
        m.addItem (2, "Join with next", selectedCount() > 0);
        m.addItem (3, "Mute / unmute", selectedCount() > 0);
        m.addSeparator();
        m.addItem (4, "Snap pitch to scale", selectedCount() > 0);
        m.addItem (5, "Snap pitch to semitone", selectedCount() > 0);
        m.addItem (6, "Remove pitch drift", selectedCount() > 0);
        m.addItem (7, "Remove vibrato", selectedCount() > 0);
        m.addItem (8, "Reset note(s)", selectedCount() > 0);
        m.addSeparator();
        m.addItem (9, "Select all");
        juce::Component::SafePointer<VocalRoll> self (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (juce::Rectangle<int> ((int) at.x, (int) at.y, 1, 1).translated (getScreenX(), getScreenY())),
                         [self, tAt] (int r)
                         {
                             if (self == nullptr) return;
                             switch (r)
                             {
                                 case 1: self->splitAt (tAt); break;
                                 case 2: self->joinSelected(); break;
                                 case 3: self->toggleMute(); break;
                                 case 4: self->snapSelected (2); break;
                                 case 5: self->snapSelected (1); break;
                                 case 6: self->editSelected ([] (aio::VocalNote& n) { n.drift = 0.f; }); break;
                                 case 7: self->editSelected ([] (aio::VocalNote& n) { n.vib = 0.f; }); break;
                                 case 8: self->resetSelected(); break;
                                 case 9: self->selectAll(); break;
                                 default: break;
                             }
                         });
    }

    void timerCallback() override
    {
        auto& v = vocal();
        if (! v.hasClip()) { if (v.busy() || v.recMode() != aio::VocalSession::RecIdle) repaint(); return; }
        const double ph = v.playheadSeconds();
        const bool moving = v.isPlaying() || std::abs (ph - lastPlayhead) > 1.0e-6;
        if (moving)
        {
            lastPlayhead = ph;
            const double tRight = timeAt ((float) getWidth());
            if (v.isPlaying() && drag == None && (ph > tRight - 0.04 * (tRight - viewT0) || ph < viewT0))
                viewT0 = std::max (-0.05, ph - 0.1 * (tRight - viewT0));
            repaint();
        }
        else if (v.busy() || v.rendering()) repaint();
    }

    AIOProcessor& proc;
    double viewT0 = -0.05, pxPerSec = 80.0, topMidi = 76.0, pxPerSemi = 14.0;
    std::vector<char> sel, selBefore;
    DragKind drag = None;
    std::vector<aio::VocalNote> dragStart;
    int dragNote = -1;
    juce::Point<float> dragOrigin;
    juce::Rectangle<float> band;
    bool dragMoved = false, dragCheckpointed = false, fitted = false;
    juce::uint32 lastRender = 0;
    double lastPlayhead = -1.0;
};
} // namespace aio::ui
