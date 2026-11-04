#pragma once
#include <cstring>
#include "Widgets.h"

namespace aio::ui
{
// =================================================================================================
// EQ curve + live spectrum, draggable band nodes
// =================================================================================================
class EQDisplay : public juce::Component, private juce::Timer
{
public:
    explicit EQDisplay (AIOProcessor& p) : proc (p), fft (kOrder), window ((size_t) kN, juce::dsp::WindowingFunction<float>::hann)
    {
        history.fill (0.f);
        smooth.fill (-100.f);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::panel); g.fillRoundedRectangle (r, 8.f);
        const auto area = plotArea();

        // grid
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        static const float fl[] = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
        for (float f : fl)
        {
            const float x = fToX (f);
            g.setColour (col::edge.withAlpha (0.7f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.setColour (col::dim);
            g.drawText (f >= 1000 ? juce::String (f / 1000.f, 0) + "k" : juce::String ((int) f), (int) x - 16, (int) area.getBottom() + 2, 32, 12, juce::Justification::centred);
        }
        for (int db = -18; db <= 18; db += 6)
        {
            const float y = dbToY ((float) db);
            g.setColour (db == 0 ? col::edge.brighter (0.4f) : col::edge.withAlpha (0.7f));
            g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
            g.setColour (col::dim);
            g.drawText (juce::String (db), (int) area.getX() - 30, (int) y - 6, 26, 12, juce::Justification::centredRight);
        }

        // spectrum
        {
            juce::Path sp;
            bool started = false;
            for (float x = area.getX(); x <= area.getRight(); x += 2.f)
            {
                const float f = xToF (x);
                const float bin = f / (float) (proc.getSampleRateForUI() * 0.5) * (float) (kN / 2);
                const int b0 = juce::jlimit (0, kN / 2 - 2, (int) bin);
                const float frac = bin - (float) b0;
                const float db = smooth[(size_t) b0] * (1.f - frac) + smooth[(size_t) b0 + 1] * frac;
                const float y = area.getBottom() - juce::jlimit (0.f, 1.f, (db + 90.f) / 80.f) * area.getHeight();
                if (! started) { sp.startNewSubPath (x, y); started = true; } else sp.lineTo (x, y);
            }
            sp.lineTo (area.getRight(), area.getBottom());
            sp.lineTo (area.getX(), area.getBottom());
            sp.closeSubPath();
            g.setColour (col::accent.withAlpha (0.16f));
            g.fillPath (sp);
        }

        const auto bands = readBands();
        const double sr = proc.getSampleRateForUI();

        // combined response
        {
            juce::Path curve;
            bool started = false;
            for (float x = area.getX(); x <= area.getRight(); x += 2.f)
            {
                const float db = (float) aio::ParametricEQ::responseDb (bands, sr, xToF (x));
                const float y = dbToY (juce::jlimit (-18.f, 18.f, db));
                if (! started) { curve.startNewSubPath (x, y); started = true; } else curve.lineTo (x, y);
            }
            g.setColour (col::accent);
            g.strokePath (curve, juce::PathStrokeType (2.2f));
        }

        // nodes
        for (int i = 0; i < 6; ++i)
        {
            const auto p = nodePos (i, bands);
            const auto c = bandColour (i);
            const bool on = bands[(size_t) i].on;
            g.setColour (on ? c : c.withAlpha (0.25f));
            g.fillEllipse (p.x - 7.f, p.y - 7.f, 14.f, 14.f);
            g.setColour (col::bg);
            g.setFont (juce::Font (juce::FontOptions (10.f, juce::Font::bold)));
            g.drawText (juce::String (i + 1), (int) p.x - 7, (int) p.y - 7, 14, 14, juce::Justification::centred);
            if (i == dragBand)
            {
                g.setColour (c);
                g.drawEllipse (p.x - 10.f, p.y - 10.f, 20.f, 20.f, 1.5f);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto bands = readBands();
        dragBand = -1;
        float best = 14.f;
        for (int i = 0; i < 6; ++i)
        {
            const float d = nodePos (i, bands).getDistanceFrom (e.position);
            if (d < best) { best = d; dragBand = i; }
        }
        if (dragBand >= 0)
        {
            if (e.mods.isPopupMenu() || e.getNumberOfClicks() > 1)
            {
                toggleBand (dragBand);
                return;
            }
            for (auto* id : { "Freq", "Gain" })
                if (auto* p = proc.apvts.getParameter (eqId (dragBand, id))) p->beginChangeGesture();
            dragging = true;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging || dragBand < 0) return;
        const auto area = plotArea();
        const float f = juce::jlimit (20.f, 20000.f, xToF (juce::jlimit (area.getX(), area.getRight(), e.position.x)));
        setParam (eqId (dragBand, "Freq"), f);
        const int type = (int) proc.apvts.getRawParameterValue (eqId (dragBand, "Type"))->load();
        if (type <= 2) setParam (eqId (dragBand, "Gain"), juce::jlimit (-18.f, 18.f, yToDb (e.position.y)));
        if (auto* on = proc.apvts.getParameter (eqId (dragBand, "On"))) if (on->getValue() < 0.5f) on->setValueNotifyingHost (1.f);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging && dragBand >= 0)
            for (auto* id : { "Freq", "Gain" })
                if (auto* p = proc.apvts.getParameter (eqId (dragBand, id))) p->endChangeGesture();
        dragging = false;
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto bands = readBands();
        int hit = -1; float best = 18.f;
        for (int i = 0; i < 6; ++i) { const float d = nodePos (i, bands).getDistanceFrom (e.position); if (d < best) { best = d; hit = i; } }
        if (hit < 0) return;
        if (auto* p = proc.apvts.getParameter (eqId (hit, "Q")))
        {
            const float cur = p->convertFrom0to1 (p->getValue());
            p->setValueNotifyingHost (p->convertTo0to1 (juce::jlimit (0.1f, 18.f, cur * (1.f + w.deltaY * 0.6f))));
        }
        repaint();
    }

private:
    static constexpr int kOrder = 11, kN = 1 << kOrder;

    static juce::Colour bandColour (int i)
    {
        static const juce::Colour c[6] = { juce::Colour (0xffff6b6b), juce::Colour (0xffffa94d), juce::Colour (0xffffe066),
                                           juce::Colour (0xff69db7c), juce::Colour (0xff4dabf7), juce::Colour (0xffb084ff) };
        return c[i];
    }

    juce::Rectangle<float> plotArea() const { return getLocalBounds().toFloat().withTrimmedLeft (36.f).withTrimmedBottom (16.f).reduced (6.f, 6.f); }
    float fToX (float f) const { const auto a = plotArea(); return a.getX() + a.getWidth() * (std::log (f / 20.f) / std::log (1000.f)); }
    float xToF (float x) const { const auto a = plotArea(); return 20.f * std::pow (1000.f, (x - a.getX()) / a.getWidth()); }
    float dbToY (float db) const { const auto a = plotArea(); return a.getBottom() - (db + 18.f) / 36.f * a.getHeight(); }
    float yToDb (float y) const  { const auto a = plotArea(); return (a.getBottom() - y) / a.getHeight() * 36.f - 18.f; }

    std::array<aio::EQBand, 6> readBands() const
    {
        std::array<aio::EQBand, 6> b;
        auto g = [this] (const juce::String& id) { return proc.apvts.getRawParameterValue (id)->load(); };
        const bool all = g ("eqOn") > 0.5f;
        for (int i = 0; i < 6; ++i)
        {
            b[(size_t) i].on = all && g (eqId (i, "On")) > 0.5f;
            b[(size_t) i].type = (aio::FilterType) (int) g (eqId (i, "Type"));
            b[(size_t) i].freq = g (eqId (i, "Freq"));
            b[(size_t) i].gainDb = g (eqId (i, "Gain"));
            b[(size_t) i].q = g (eqId (i, "Q"));
        }
        return b;
    }

    juce::Point<float> nodePos (int i, const std::array<aio::EQBand, 6>& b) const
    {
        const auto& band = b[(size_t) i];
        const bool gainBased = band.type == aio::FilterType::Bell || band.type == aio::FilterType::LowShelf || band.type == aio::FilterType::HighShelf;
        return { fToX (band.freq), dbToY (gainBased ? band.gainDb : 0.f) };
    }

    void setParam (const juce::String& id, float v)
    {
        if (auto* p = proc.apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    void toggleBand (int i)
    {
        if (auto* p = proc.apvts.getParameter (eqId (i, "On"))) { p->beginChangeGesture(); p->setValueNotifyingHost (p->getValue() > 0.5f ? 0.f : 1.f); p->endChangeGesture(); }
        repaint();
    }

    void timerCallback() override
    {
        if (! isShowing()) return;
        float tmp[4096];
        const int got = proc.readAnalyzer (tmp, 4096);
        if (got > 0)
        {
            const int keep = kN - std::min (got, kN);
            std::memmove (history.data(), history.data() + (kN - keep), (size_t) keep * sizeof (float));
            const int take = std::min (got, kN);
            std::memcpy (history.data() + keep, tmp + (got - take), (size_t) take * sizeof (float));
        }
        std::array<float, 2 * kN> data {};
        std::copy (history.begin(), history.end(), data.begin());
        window.multiplyWithWindowingTable (data.data(), (size_t) kN);
        fft.performFrequencyOnlyForwardTransform (data.data());
        for (int k = 0; k < kN / 2; ++k)
        {
            const float db = juce::Decibels::gainToDecibels (data[(size_t) k] / (float) (kN / 4), -100.f);
            smooth[(size_t) k] = std::max (db, smooth[(size_t) k] - 2.0f);
        }
        repaint();
    }

    AIOProcessor& proc;
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::array<float, kN> history;
    std::array<float, kN / 2 + 2> smooth;
    int dragBand = -1;
    bool dragging = false;
};

// =================================================================================================
// Tuner: 12 pitch-class keys (click to edit the custom scale), detected / target readout
// =================================================================================================
class PitchDisplay : public juce::Component, private juce::Timer
{
public:
    explicit PitchDisplay (AIOProcessor& p) : proc (p) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::bg.brighter (0.03f)); g.fillRoundedRectangle (r, 8.f);

        const uint16_t mask = proc.getEffectiveTunerMask();
        const float det = proc.getDetectedMidi(), tgt = proc.getTargetMidi();
        const int detPc = det >= 0 ? ((int) std::lround (det) % 12 + 12) % 12 : -1;
        const int tgtPc = tgt >= 0 ? ((int) std::lround (tgt) % 12 + 12) % 12 : -1;

        auto keys = keyArea();
        const float kw = keys.getWidth() / 12.f;
        for (int i = 0; i < 12; ++i)
        {
            auto k = juce::Rectangle<float> (keys.getX() + i * kw, keys.getY(), kw, keys.getHeight()).reduced (3.f);
            const bool allowed = (mask >> i) & 1;
            const bool black = (i == 1 || i == 3 || i == 6 || i == 8 || i == 10);
            g.setColour (allowed ? (i == tgtPc ? col::accent : col::accent.withAlpha (0.55f)) : (black ? col::panel : col::panel.brighter (0.12f)));
            g.fillRoundedRectangle (k, 6.f);
            g.setColour (i == detPc ? col::accent2 : col::edge);
            g.drawRoundedRectangle (k, 6.f, i == detPc ? 2.5f : 1.f);
            g.setColour (allowed ? col::bg : col::dim);
            g.setFont (juce::Font (juce::FontOptions (15.f, juce::Font::bold)));
            g.drawText (aio::pcName (i), k.toNearestInt(), juce::Justification::centred);
        }

        // readout
        auto info = r.withTrimmedTop (keys.getBottom() + 12.f).reduced (14.f, 0.f);
        g.setColour (col::text);
        g.setFont (juce::Font (juce::FontOptions (26.f, juce::Font::bold)));
        const juce::String detName = det >= 0 ? noteName (det) : juce::String ("--");
        const juce::String tgtName = tgt >= 0 ? noteName (tgt) : juce::String ("--");
        g.drawText ("In: " + detName, info.removeFromLeft (info.getWidth() * 0.5f).toNearestInt(), juce::Justification::centredLeft);
        g.setColour (col::accent);
        g.drawText ("Out: " + tgtName, info.toNearestInt(), juce::Justification::centredLeft);

        // cents needle
        auto m = juce::Rectangle<float> (r.getX() + 24.f, r.getBottom() - 34.f, r.getWidth() - 48.f, 8.f);
        g.setColour (col::edge); g.fillRoundedRectangle (m, 4.f);
        g.setColour (col::dim); g.fillRect (m.getCentreX() - 1.f, m.getY() - 4.f, 2.f, m.getHeight() + 8.f);
        if (det >= 0 && tgt >= 0)
        {
            const float cents = juce::jlimit (-100.f, 100.f, (det - tgt) * 100.f);
            const float x = m.getCentreX() + cents / 100.f * (m.getWidth() * 0.5f);
            g.setColour (std::abs (cents) < 8.f ? col::accent : col::accent2);
            g.fillEllipse (x - 7.f, m.getCentreY() - 7.f, 14.f, 14.f);
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (12.f)));
            g.drawText (juce::String (cents, 0) + " ct", (int) m.getX(), (int) m.getBottom() + 8, (int) m.getWidth(), 14, juce::Justification::centred);
        }
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        g.drawText ("click a key to edit the scale (switches to Custom)", r.toNearestInt().removeFromTop (18).withTrimmedRight (14), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto keys = keyArea();
        if (! keys.contains (e.position)) return;
        const int i = juce::jlimit (0, 11, (int) ((e.position.x - keys.getX()) / (keys.getWidth() / 12.f)));
        auto& apvts = proc.apvts;
        const uint16_t mask = proc.getEffectiveTunerMask();

        if (auto* sc = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter ("tuneScale")))
        {
            if (sc->getIndex() != sc->choices.size() - 1)
            {
                // copy the currently effective scale into the custom toggles, then switch to Custom
                for (int k = 0; k < 12; ++k)
                    if (auto* b = apvts.getParameter ("tuneNote" + juce::String (k))) b->setValueNotifyingHost (((mask >> k) & 1) ? 1.f : 0.f);
                if (auto* link = apvts.getParameter ("tuneLink")) link->setValueNotifyingHost (0.f);
                sc->beginChangeGesture();
                *sc = sc->choices.size() - 1;
                sc->endChangeGesture();
            }
        }
        if (auto* b = apvts.getParameter ("tuneNote" + juce::String (i)))
        {
            b->beginChangeGesture();
            b->setValueNotifyingHost (b->getValue() > 0.5f ? 0.f : 1.f);
            b->endChangeGesture();
        }
        repaint();
    }

private:
    juce::Rectangle<float> keyArea() const { return getLocalBounds().toFloat().withTrimmedTop (22.f).withHeight (78.f).reduced (10.f, 0.f); }

    static juce::String noteName (float midi)
    {
        const int n = (int) std::lround (midi);
        return juce::String (aio::pcName (n)) + juce::String (n / 12 - 1);
    }

    void timerCallback() override { if (isShowing()) repaint(); }

    AIOProcessor& proc;
};

// =================================================================================================
// Mini piano roll for the generated pattern
// =================================================================================================
class PianoRollView : public juce::Component, private juce::Timer
{
public:
    explicit PianoRollView (AIOProcessor& p) : proc (p) { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::bg.brighter (0.03f)); g.fillRoundedRectangle (r, 8.f);

        auto pat = proc.getPatternForUI();
        if (! pat || pat->notes.empty()) return;

        auto area = r.reduced (8.f);
        auto header = area.removeFromTop (20.f);
        const double L = pat->lengthBeats;
        const float w = area.getWidth();

        int lo = 127, hi = 0;
        for (auto& n : pat->notes) { lo = std::min (lo, n.pitch); hi = std::max (hi, n.pitch); }
        lo -= 2; hi += 2;
        const float rowH = area.getHeight() / (float) (hi - lo + 1);

        // pitch rows (black keys darker)
        for (int p = lo; p <= hi; ++p)
        {
            const int pc = p % 12;
            const bool black = (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10);
            const float y = area.getBottom() - (float) (p - lo + 1) * rowH;
            g.setColour (black ? col::panel.withAlpha (0.55f) : col::panel.withAlpha (0.25f));
            g.fillRect (area.getX(), y, w, rowH);
        }

        // beat / bar lines
        for (double b = 0; b <= L + 1e-6; b += 1.0)
        {
            const float x = area.getX() + (float) (b / L) * w;
            const bool bar = std::fmod (b, 4.0) < 1e-6;
            g.setColour (bar ? col::edge.brighter (0.5f) : col::edge.withAlpha (0.5f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }

        // chord names
        g.setFont (juce::Font (juce::FontOptions (13.f, juce::Font::bold)));
        for (auto& c : pat->chords)
        {
            const float x = area.getX() + (float) (c.start / L) * w;
            g.setColour (col::violet);
            g.drawText (c.name, (int) x + 4, (int) header.getY(), (int) ((c.len / L) * w) - 4, (int) header.getHeight(), juce::Justification::centredLeft);
        }

        // notes
        for (auto& n : pat->notes)
        {
            const float x = area.getX() + (float) (n.start / L) * w;
            const float nw = std::max (2.f, (float) (n.len / L) * w - 1.f);
            const float y = area.getBottom() - (float) (n.pitch - lo + 1) * rowH;
            const juce::Colour c = n.part == aio::PartChords ? col::chords : n.part == aio::PartMelody ? col::melody : col::bass;
            g.setColour (c.withAlpha (0.35f + 0.65f * (float) n.vel / 127.f));
            g.fillRoundedRectangle (x, y + 0.5f, nw, std::max (2.5f, rowH - 1.f), 2.f);
        }

        // playhead
        if (proc.composeRunning.load())
        {
            const float x = area.getX() + (float) (proc.uiBeat.load() / L) * w;
            g.setColour (col::text);
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }
    }

private:
    void timerCallback() override { if (isShowing()) repaint(); }
    AIOProcessor& proc;
};
} // namespace aio::ui
