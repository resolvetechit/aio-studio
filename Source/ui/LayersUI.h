#pragma once
// LAYERS and VOICE MORPH widgets (shared by the editor and the tests)
#include "Widgets.h"
#include "../dsp/LayerEngine.h"

namespace aio::ui
{
const juce::Colour layersCol { 0xff6ee7a8 };       // mint
const juce::Colour morphCol  { 0xffff7a59 };       // coral

inline juce::String keyName (int n) { return juce::String (aio::pcName (((n % 12) + 12) % 12)) + juce::String (n / 12 - 1); }

// =================================================================================================
// LAYERS keyboard (3 octaves, click / drag to play, optional latch)
// =================================================================================================
class LayerKeys : public juce::Component
{
public:
    explicit LayerKeys (AIOProcessor& p) : proc (p) {}
    static constexpr int firstNote = 36, numWhite = 28;         // C2 .. B5

    void paint (juce::Graphics& g) override
    {
        const float ww = (float) getWidth() / numWhite, bh = (float) getHeight() * 0.6f;
        for (int pass = 0; pass < 2; ++pass)
            for (int n = firstNote; n < firstNote + 48; ++n)
            {
                const bool black = isBlack (n);
                if ((pass == 0) == black) continue;
                const auto r = keyRect (n, ww, bh);
                juce::Colour c = black ? juce::Colour (0xff181a20) : juce::Colour (0xffe6e9f0);
                if (n == held) c = layersCol.interpolatedWith (juce::Colours::white, black ? 0.f : 0.2f);
                g.setColour (c); g.fillRoundedRectangle (r, 3.f);
                g.setColour (juce::Colour (0xff0b0c10)); g.drawRoundedRectangle (r, 3.f, 1.f);
                if (n % 12 == 0)
                {
                    g.setColour (juce::Colour (0xff4a5060));
                    g.setFont (juce::Font (juce::FontOptions (10.5f)));
                    g.drawText (keyName (n), r.toNearestInt().withTrimmedBottom (4), juce::Justification::centredBottom);
                }
            }
    }
    void mouseDown (const juce::MouseEvent& e) override
    {
        proc.setParam ("lySource", 0.f);                         // playing a key means "notes from the keys"
        const int n = noteAt (e.position);
        if (n < 0) return;
        const bool latch = proc.apvts.getRawParameterValue ("lyHold")->load() > 0.5f;
        if (latch && n == held) { held = -1; proc.setLayerKey (-1); }
        else { held = n; proc.setLayerKey (n); }
        repaint();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int n = noteAt (e.position);
        if (n >= 0 && n != held) { held = n; proc.setLayerKey (n); repaint(); }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (proc.apvts.getRawParameterValue ("lyHold")->load() > 0.5f) return;
        held = -1; proc.setLayerKey (-1); repaint();
    }
    void releaseKey() { held = -1; proc.setLayerKey (-1); repaint(); }
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
        for (int n = firstNote; n < firstNote + 48; ++n)
            if (isBlack (n) && keyRect (n, ww, bh).contains (p)) return n;
        for (int n = firstNote; n < firstNote + 48; ++n)
            if (! isBlack (n) && keyRect (n, ww, bh).contains (p)) return n;
        return -1;
    }
    AIOProcessor& proc;
    int held = -1;
};

// =================================================================================================
// One layer's detail controls
// =================================================================================================
class LayerDetail : public juce::Component
{
public:
    LayerDetail (AIOProcessor& p, int index)
        : proc (p), idx (index),
          kLevel  (p.apvts, id ("Level"),  "Level", 1, layersCol), kPan (p.apvts, id ("Pan"), "Pan", 0, layersCol),
          kCoarse (p.apvts, id ("Coarse"), "Pitch", 0, layersCol), kFine (p.apvts, id ("Fine"), "Fine tune", 0, layersCol),
          kAttack (p.apvts, id ("Attack"), "Attack", 0, layersCol), kRelease (p.apvts, id ("Release"), "Release", 0, layersCol),
          kCut    (p.apvts, id ("Cutoff"), "Filter", 0, layersCol), kReso (p.apvts, id ("Reso"), "Resonance", 0, layersCol),
          kMotion (p.apvts, id ("Motion"), "Motion", 0, layersCol), kDrive (p.apvts, id ("Drive"), "Drive", 0, layersCol),
          kLofi   (p.apvts, id ("LoFi"),   "Lo-fi", 0, layersCol),
          tTrack  (p.apvts, id ("Track"),  "Follows the keys", layersCol), tLoop (p.apvts, id ("Loop"), "Loop sample", layersCol)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &kLevel, &kPan, &kCoarse, &kFine, &kAttack, &kRelease, &kCut, &kReso, &kMotion, &kDrive, &kLofi,
                                                                  &tTrack, &tLoop, &loadBtn, &clearBtn, &sampleName })
            addAndMakeVisible (c);
        kCoarse.slider.setTextValueSuffix ({});
        kCoarse.slider.textFromValueFunction = [] (double v) { return (v > 0 ? "+" : "") + juce::String ((int) v) + " st"; };
        kCoarse.slider.updateText();
        kPan.slider.setTextValueSuffix ({});
        kPan.slider.textFromValueFunction = [] (double v) { return std::abs (v) < 1 ? juce::String ("centre") : (v < 0 ? juce::String (-(int) v) + " L" : juce::String ((int) v) + " R"); };
        kPan.slider.updateText();
        loadBtn.setButtonText ("Load sample...");
        clearBtn.setButtonText ("Remove");
        loadBtn.onClick = [this] { chooseSample(); };
        clearBtn.onClick = [this] { proc.clearLayerSample (idx); refreshName(); };
        sampleName.setFont (juce::Font (juce::FontOptions (12.f)));
        sampleName.setColour (juce::Label::textColourId, col::dim);
        sampleName.setMinimumHorizontalScale (0.7f);
        refreshName();
    }

    void refreshName()
    {
        const auto n = proc.layerSampleName (idx);
        sampleName.setText (n.isEmpty() ? "no sample loaded (choose 'Sample' as the sound to use one)" : "sample: " + n, juce::dontSendNotification);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto top = b.removeFromTop (28);
        loadBtn.setBounds (top.removeFromLeft (120)); top.removeFromLeft (6);
        clearBtn.setBounds (top.removeFromLeft (70)); top.removeFromLeft (10);
        sampleName.setBounds (top);
        b.removeFromTop (4);
        auto togRow = b.removeFromBottom (26);
        tTrack.setBounds (togRow.removeFromLeft (170));
        tLoop.setBounds (togRow.removeFromLeft (150));
        const int rowH = b.getHeight() / 2, cw = b.getWidth() / 6;
        auto r1 = b.removeFromTop (rowH), r2 = b;
        for (auto* k : { &kLevel, &kPan, &kCoarse, &kFine, &kAttack, &kRelease }) k->setBounds (r1.removeFromLeft (cw));
        for (auto* k : { &kCut, &kReso, &kMotion, &kDrive, &kLofi }) k->setBounds (r2.removeFromLeft (cw));
    }

private:
    juce::String id (const char* s) const { return "ly" + juce::String (idx) + s; }
    void chooseSample()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a sample into this layer", juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto f = fc.getResult();
                                  if (! f.existsAsFile()) return;
                                  juce::String err;
                                  if (! proc.loadLayerSample (idx, f, err))
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Sample", err);
                                  refreshName();
                              });
    }

    AIOProcessor& proc;
    int idx;
    Knob kLevel, kPan, kCoarse, kFine, kAttack, kRelease, kCut, kReso, kMotion, kDrive, kLofi;
    Toggle tTrack, tLoop;
    juce::TextButton loadBtn, clearBtn;
    juce::Label sampleName;
    std::unique_ptr<juce::FileChooser> chooser;
};

class CharacterPad : public juce::Component
{
public:
    explicit CharacterPad (AIOProcessor& p) : proc (p) {}
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::bg); g.fillRoundedRectangle (r, 6.f);
        g.setColour (col::edge);
        for (int i = 1; i < 4; ++i)
        {
            g.drawLine (r.getX() + r.getWidth() * i / 4.f, r.getY(), r.getX() + r.getWidth() * i / 4.f, r.getBottom());
            g.drawLine (r.getX(), r.getY() + r.getHeight() * i / 4.f, r.getRight(), r.getY() + r.getHeight() * i / 4.f);
        }
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawText ("higher voice", getLocalBounds().withHeight (20).reduced (8, 0), juce::Justification::centredTop);
        g.drawText ("lower voice", getLocalBounds().withTrimmedTop (getHeight() - 20).reduced (8, 0), juce::Justification::centredBottom);
        g.drawText ("smaller throat", getLocalBounds().withTrimmedLeft (getWidth() - 100).reduced (6, 0), juce::Justification::centredRight);
        g.drawText ("bigger throat", getLocalBounds().withWidth (100).reduced (6, 0), juce::Justification::centredLeft);
        const auto d = dotPos();
        g.setColour (morphCol.withAlpha (0.25f)); g.fillEllipse (d.x - 14.f, d.y - 14.f, 28.f, 28.f);
        g.setColour (morphCol); g.fillEllipse (d.x - 7.f, d.y - 7.f, 14.f, 14.f);
        g.setColour (col::edge); g.drawRoundedRectangle (r.reduced (0.5f), 6.f, 1.f);
    }
    void mouseDown (const juce::MouseEvent& e) override { set (e.position); }
    void mouseDrag (const juce::MouseEvent& e) override { set (e.position); }
    void mouseDoubleClick (const juce::MouseEvent&) override { proc.setParam ("vcPitch", 0.f); proc.setParam ("vcFormant", 0.f); repaint(); }
    juce::Point<float> dotPos() const
    {
        const float fx = proc.apvts.getRawParameterValue ("vcFormant")->load(), fp = proc.apvts.getRawParameterValue ("vcPitch")->load();
        return { juce::jmap (fx, 8.f, -8.f, 12.f, (float) getWidth() - 12.f), juce::jmap (fp, 12.f, -9.f, 12.f, (float) getHeight() - 12.f) };
    }
    void refresh() { repaint(); }

private:
    void set (juce::Point<float> p)
    {
        const float fx = juce::jlimit (-8.f, 8.f, juce::jmap (p.x, 12.f, (float) getWidth() - 12.f, 8.f, -8.f));
        const float fp = juce::jlimit (-9.f, 12.f, juce::jmap (p.y, 12.f, (float) getHeight() - 12.f, 12.f, -9.f));
        proc.setParam ("vcFormant", std::round (fx * 10.f) / 10.f);
        proc.setParam ("vcPitch", std::round (fp * 10.f) / 10.f);
        proc.setParam ("vcOn", 1.f);
        repaint();
    }
    AIOProcessor& proc;
};
} // namespace aio::ui
