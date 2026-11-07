#pragma once
#include "Widgets.h"

namespace aio::ui
{
inline const juce::Colour talkCol { 0xfff2a03d };

inline juce::String talkNoteName (double midi)
{
    const int n = (int) std::lround (midi);
    return juce::String (aio::pcName (((n % 12) + 12) % 12)) + juce::String (n / 12 - 1);
}

// ------------------------------------------------------------------------------------------------
// the "mouth": the voice's formant shape, live, plus level / gate / consonant / note read-outs
class MouthView : public juce::Component, private juce::Timer
{
public:
    explicit MouthView (AIOProcessor& p) : proc (p) { startTimerHz (30); }
    ~MouthView() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        g.setColour (col::bg.darker (0.25f)); g.fillRoundedRectangle (b, 6.f);
        g.setColour (col::edge); g.drawRoundedRectangle (b.reduced (0.5f), 6.f, 1.f);

        auto& tb = proc.getTalk();
        const bool on = proc.apvts.getRawParameterValue ("tbOn")->load() > 0.5f;
        auto plot = b.reduced (14.f, 12.f);
        plot.removeFromBottom (22.f);
        plot.removeFromRight (74.f);

        // grid + frequency labels
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        for (double f : { 200.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0 })
        {
            const float x = plot.getX() + plot.getWidth() * (float) (std::log (f / 100.0) / std::log (80.0));
            g.setColour (col::edge.withAlpha (0.7f)); g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (col::dim);
            g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, f == 1000.0 ? 0 : 0) + "k" : juce::String ((int) f), (int) x - 16, (int) plot.getBottom() + 3, 32, 14, juce::Justification::centred);
        }

        // formant curve
        constexpr int N = aio::TalkBox::kEnvPoints;
        float v[N]; float lo = 0.f;
        for (int i = 0; i < N; ++i) { v[i] = tb.envDb[(size_t) i].load(); lo = std::min (lo, v[i]); }
        const bool hasData = on && lo > -89.f && tb.gateMeter.load() > 0.05f;
        if (hasData)
        {
            juce::Path line, fill;
            auto px = [&] (int i) { return plot.getX() + plot.getWidth() * (float) i / (N - 1); };
            auto py = [&] (float db) { return plot.getY() + plot.getHeight() * juce::jlimit (0.f, 1.f, (8.f - db) / 50.f); };
            line.startNewSubPath (px (0), py (v[0]));
            for (int i = 1; i < N; ++i) line.lineTo (px (i), py (v[i]));
            fill = line;
            fill.lineTo (px (N - 1), plot.getBottom()); fill.lineTo (px (0), plot.getBottom()); fill.closeSubPath();
            g.setColour (talkCol.withAlpha (0.18f)); g.fillPath (fill);
            g.setColour (talkCol); g.strokePath (line, juce::PathStrokeType (2.f));

            // formant markers: the first three clear peaks
            int found = 0;
            for (int i = 7; i < N - 2 && found < 3; ++i)
                if (v[i] > v[i - 1] && v[i] >= v[i + 1] && v[i] > v[i - 2] && v[i] > v[i + 2] && v[i] > -30.f)
                {
                    const double f = 100.0 * std::pow (80.0, (double) i / (N - 1));
                    g.setColour (juce::Colours::white.withAlpha (0.85f));
                    g.fillEllipse (px (i) - 3.f, py (v[i]) - 3.f, 6.f, 6.f);
                    g.setFont (juce::Font (juce::FontOptions (11.f, juce::Font::bold)));
                    g.drawText ("F" + juce::String (++found) + "  " + juce::String ((int) std::round (f / 10.0) * 10), (int) px (i) - 34, (int) py (v[i]) - 20, 68, 14, juce::Justification::centred);
                    i += 3;
                }
        }
        else
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (13.f)));
            g.drawFittedText (on ? "Speak or rap into the mic.\nYour mouth shape appears here and shapes the synth."
                                 : "The talk box is off.", plot.toNearestInt(), juce::Justification::centred, 3);
        }

        // read-outs on the right
        auto r = b.reduced (12.f, 12.f).removeFromRight (70.f);
        const float cm = tb.carrierMidi.load();
        g.setColour (cm >= 0.f ? talkCol : col::dim);
        g.setFont (juce::Font (juce::FontOptions (26.f, juce::Font::bold)));
        g.drawText (cm >= 0.f ? talkNoteName (cm) : "--", r.removeFromTop (32.f).toNearestInt(), juce::Justification::centred);
        g.setColour (col::dim); g.setFont (juce::Font (juce::FontOptions (10.f)));
        g.drawText ("synth note", r.removeFromTop (12.f).toNearestInt(), juce::Justification::centred);
        r.removeFromTop (8.f);
        auto led = [&] (juce::Rectangle<float> a, const juce::String& t, float amt, juce::Colour c)
        {
            g.setColour (col::panel); g.fillRoundedRectangle (a, 3.f);
            g.setColour (c.withAlpha (0.25f + 0.75f * juce::jlimit (0.f, 1.f, amt))); g.fillRoundedRectangle (a.reduced (1.f), 3.f);
            g.setColour (col::bg); g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (t, a.toNearestInt(), juce::Justification::centred);
        };
        led (r.removeFromTop (18.f), "MOUTH", tb.gateMeter.load(), col::accent); r.removeFromTop (4.f);
        led (r.removeFromTop (18.f), "S T K", tb.unvoicedMeter.load(), col::accent2); r.removeFromTop (4.f);
        led (r.removeFromTop (18.f), "SYNTH", tb.active.load() ? 1.f : 0.f, talkCol);

        // level bars along the bottom
        auto bar = b.reduced (14.f, 0.f).removeFromBottom (18.f).withTrimmedBottom (4.f);
        bar.removeFromRight (74.f);
        auto meter = [&] (juce::Rectangle<float> a, const juce::String& t, float lin, juce::Colour c)
        {
            g.setColour (col::dim); g.setFont (juce::Font (juce::FontOptions (10.f)));
            g.drawText (t, a.removeFromLeft (28.f).toNearestInt(), juce::Justification::centredLeft);
            a = a.withSizeKeepingCentre (a.getWidth(), 6.f);
            g.setColour (col::panel); g.fillRoundedRectangle (a, 3.f);
            const float db = juce::Decibels::gainToDecibels (lin, -60.f);
            g.setColour (c); g.fillRoundedRectangle (a.withWidth (a.getWidth() * juce::jlimit (0.f, 1.f, (db + 60.f) / 60.f)), 3.f);
        };
        const float half = bar.getWidth() * 0.5f;
        meter (bar.removeFromLeft (half).withTrimmedRight (8.f), "MIC", tb.inLevel.load(), col::accent);
        meter (bar, "OUT", tb.outLevel.load(), talkCol);
    }

private:
    void timerCallback() override { repaint(); }
    AIOProcessor& proc;
};

// ------------------------------------------------------------------------------------------------
// three octaves of keys.  Click / drag plays the lead (sliding between keys glides).
class TalkKeys : public juce::Component, private juce::Timer
{
public:
    explicit TalkKeys (AIOProcessor& p) : proc (p) { startTimerHz (30); }
    ~TalkKeys() override { stopTimer(); }

    static constexpr int firstNote = 48, numWhite = 21;      // C3 .. B5

    void paint (juce::Graphics& g) override
    {
        const float cm = proc.getTalk().carrierMidi.load();
        const int sounding = cm >= 0.f ? (int) std::lround (cm) : -1;
        const auto mask = proc.getEffectiveTunerMask();
        const float ww = (float) getWidth() / numWhite, bh = (float) getHeight() * 0.6f;
        auto whiteIdx = [] (int pc) { static const int w[12] = { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 }; return w[pc]; };

        for (int pass = 0; pass < 2; ++pass)
            for (int n = firstNote; n < firstNote + 36; ++n)
            {
                const int pc = n % 12;
                const bool black = whiteIdx (pc) < 0;
                if ((pass == 0) == black) continue;
                const auto r = keyRect (n, ww, bh);
                const bool down = n == held, play = n == sounding;
                const bool inScale = (mask & (1u << pc)) != 0 && mask != 0x0FFF;
                juce::Colour c = black ? juce::Colour (0xff181a20) : juce::Colour (0xffe6e9f0);
                if (inScale) c = black ? c.interpolatedWith (talkCol, 0.25f) : c.interpolatedWith (talkCol, 0.14f);
                if (play) c = talkCol.interpolatedWith (juce::Colours::white, black ? 0.f : 0.25f);
                if (down) c = c.brighter (0.2f);
                g.setColour (c); g.fillRoundedRectangle (r, 3.f);
                g.setColour (juce::Colour (0xff0b0c10)); g.drawRoundedRectangle (r, 3.f, 1.f);
                if (pc == 0)
                {
                    g.setColour (black ? col::text : juce::Colour (0xff4a5060));
                    g.setFont (juce::Font (juce::FontOptions (10.5f)));
                    g.drawText (talkNoteName (n), r.toNearestInt().withTrimmedBottom (4), juce::Justification::centredBottom);
                }
            }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        proc.setParam ("tbSource", 1.f);                        // playing a key means "notes from the keys"
        const int n = noteAt (e.position);
        const bool latch = proc.apvts.getRawParameterValue ("tbLatch")->load() > 0.5f;
        if (n < 0) return;
        if (latch && n == held) { held = -1; proc.setTalkKey (-1); }
        else { held = n; proc.setTalkKey (n); }
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int n = noteAt (e.position);
        if (n >= 0 && n != held) { held = n; proc.setTalkKey (n); repaint(); }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (proc.apvts.getRawParameterValue ("tbLatch")->load() > 0.5f) return;
        held = -1; proc.setTalkKey (-1); repaint();
    }
    void releaseKey() { held = -1; proc.setTalkKey (-1); repaint(); }
    int  heldNote() const { return held; }
    juce::Point<float> pointFor (int note) const
    {
        const float ww = (float) getWidth() / numWhite, bh = (float) getHeight() * 0.6f;
        const auto r = keyRect (note, ww, bh);
        return { r.getCentreX(), r.getBottom() - 10.f };
    }

private:
    static int whiteIndexOf (int n) { static const int w[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 }; return (n - firstNote) / 12 * 7 + w[n % 12]; }
    static bool isBlack (int n) { const int pc = n % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
    juce::Rectangle<float> keyRect (int n, float ww, float bh) const
    {
        if (! isBlack (n)) return { whiteIndexOf (n) * ww + 1.f, 0.f, ww - 2.f, (float) getHeight() };
        const float x = (whiteIndexOf (n) + 1) * ww;
        return { x - ww * 0.3f, 0.f, ww * 0.6f, bh };
    }
    int noteAt (juce::Point<float> p) const
    {
        const float ww = (float) getWidth() / numWhite, bh = (float) getHeight() * 0.6f;
        for (int n = firstNote; n < firstNote + 36; ++n)
            if (isBlack (n) && keyRect (n, ww, bh).contains (p)) return n;
        for (int n = firstNote; n < firstNote + 36; ++n)
            if (! isBlack (n) && keyRect (n, ww, bh).contains (p)) return n;
        return -1;
    }
    void timerCallback() override { repaint(); }
    AIOProcessor& proc;
    int held = -1;
};
} // namespace aio::ui
