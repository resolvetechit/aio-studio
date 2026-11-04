#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "../PluginProcessor.h"

namespace aio::ui
{
namespace col
{
    const juce::Colour bg       { 0xff0e1014 };
    const juce::Colour panel    { 0xff161920 };
    const juce::Colour edge     { 0xff272c37 };
    const juce::Colour text     { 0xffdde1ea };
    const juce::Colour dim      { 0xff8791a5 };
    const juce::Colour accent   { 0xff2ee6c5 };   // teal  - tuner / eq / dynamics
    const juce::Colour accent2  { 0xffffa94d };   // amber - space
    const juce::Colour violet   { 0xffb084ff };   // violet - compose
    const juce::Colour chords   { 0xff5aa9ff };
    const juce::Colour melody   { 0xffff6fa5 };
    const juce::Colour bass     { 0xffa6e34f };
}

using APVTS = juce::AudioProcessorValueTreeState;

struct AIOLookAndFeel : public juce::LookAndFeel_V4
{
    AIOLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, col::bg);
        setColour (juce::Label::textColourId, col::text);
        setColour (juce::Slider::textBoxTextColourId, col::text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::rotarySliderFillColourId, col::accent);
        setColour (juce::Slider::thumbColourId, col::text);
        setColour (juce::Slider::trackColourId, col::accent);
        setColour (juce::Slider::backgroundColourId, col::edge);
        setColour (juce::ComboBox::backgroundColourId, col::panel.brighter (0.08f));
        setColour (juce::ComboBox::outlineColourId, col::edge);
        setColour (juce::ComboBox::textColourId, col::text);
        setColour (juce::ComboBox::arrowColourId, col::dim);
        setColour (juce::PopupMenu::backgroundColourId, col::panel);
        setColour (juce::PopupMenu::textColourId, col::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, col::accent.withAlpha (0.25f));
        setColour (juce::PopupMenu::highlightedTextColourId, col::text);
        setColour (juce::TextButton::buttonColourId, col::panel.brighter (0.1f));
        setColour (juce::TextButton::buttonOnColourId, col::accent);
        setColour (juce::TextButton::textColourOffId, col::text);
        setColour (juce::TextButton::textColourOnId, col::bg);
        setColour (juce::ToggleButton::textColourId, col::text);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& s) override
    {
        auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.f);
        const float d = std::min (b.getWidth(), b.getHeight());
        const auto c = b.getCentre();
        const float r = d * 0.5f;
        const auto fill = s.findColour (juce::Slider::rotarySliderFillColourId);

        juce::Path bgArc;
        bgArc.addCentredArc (c.x, c.y, r - 2.f, r - 2.f, 0.f, a0, a1, true);
        g.setColour (col::edge);
        g.strokePath (bgArc, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float ang = a0 + pos * (a1 - a0);
        const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const float from = bipolar ? a0 + (float) s.valueToProportionOfLength (0.0) * (a1 - a0) : a0;
        juce::Path val;
        val.addCentredArc (c.x, c.y, r - 2.f, r - 2.f, 0.f, std::min (from, ang), std::max (from, ang), true);
        g.setColour (s.isEnabled() ? fill : fill.withAlpha (0.3f));
        g.strokePath (val, juce::PathStrokeType (3.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        const float kr = r - 8.f;
        g.setColour (juce::Colour (0xff1d212b));
        g.fillEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2);
        g.setColour (col::edge.brighter (0.2f));
        g.drawEllipse (c.x - kr, c.y - kr, kr * 2, kr * 2, 1.f);

        juce::Path pointer;
        pointer.addRoundedRectangle (-1.4f, -kr + 3.f, 2.8f, kr * 0.5f, 1.2f);
        g.setColour (col::text);
        g.fillPath (pointer, juce::AffineTransform::rotation (ang).translated (c.x, c.y));
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hl, bool) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.f);
        const bool on = b.getToggleState();
        auto c = b.findColour (juce::ToggleButton::tickColourId);
        if (c == juce::Colour()) c = col::accent;
        g.setColour (on ? c.withAlpha (0.9f) : col::panel.brighter (hl ? 0.18f : 0.1f));
        g.fillRoundedRectangle (r, 5.f);
        g.setColour (on ? c : col::edge);
        g.drawRoundedRectangle (r, 5.f, 1.f);
        g.setColour (on ? col::bg : col::text);
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText (b.getButtonText(), r.toNearestInt(), juce::Justification::centred, true);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return juce::Font (juce::FontOptions (13.f)); }
    juce::Font getPopupMenuFont() override { return juce::Font (juce::FontOptions (13.5f)); }
};

inline void drawSection (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour accent)
{
    auto f = r.toFloat();
    g.setColour (col::panel);
    g.fillRoundedRectangle (f, 8.f);
    g.setColour (col::edge);
    g.drawRoundedRectangle (f.reduced (0.5f), 8.f, 1.f);
    g.setColour (accent);
    g.fillRoundedRectangle (f.getX() + 12.f, f.getY() + 10.f, 3.f, 12.f, 1.5f);
    g.setColour (col::text);
    g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
    g.drawText (title.toUpperCase(), (int) f.getX() + 20, (int) f.getY() + 6, r.getWidth() - 30, 20, juce::Justification::centredLeft);
}

// -------------------------------------------------------------------------------------------------
struct Knob : public juce::Component
{
    juce::Slider slider;
    juce::Label label;
    std::unique_ptr<APVTS::SliderAttachment> att;

    Knob (APVTS& apvts, const juce::String& id, const juce::String& name, int decimals = -1, juce::Colour c = col::accent)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 16);
        slider.setColour (juce::Slider::rotarySliderFillColourId, c);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxTextColourId, col::text);
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        slider.setRotaryParameters (juce::MathConstants<float>::pi * 1.2f, juce::MathConstants<float>::pi * 2.8f, true);
        slider.setDoubleClickReturnValue (true, apvts.getParameter (id)->convertFrom0to1 (apvts.getParameter (id)->getDefaultValue()));
        if (auto* p = apvts.getParameter (id))
            if (auto* fp = dynamic_cast<juce::AudioParameterFloat*> (p))
            {
                slider.setTextValueSuffix (" " + fp->label);
                if (decimals < 0)
                {
                    const auto& r = fp->range;
                    decimals = r.interval > 0.f ? (r.interval >= 1.f ? 0 : (r.interval >= 0.1f ? 1 : 2)) : (r.end >= 100.f ? 0 : 2);
                }
            }
        if (decimals >= 0) slider.setNumDecimalPlacesToDisplay (decimals);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (juce::Font (juce::FontOptions (12.f)));
        label.setColour (juce::Label::textColourId, col::dim);
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
        att = std::make_unique<APVTS::SliderAttachment> (apvts, id, slider);
    }
    void resized() override
    {
        auto b = getLocalBounds();
        label.setBounds (b.removeFromTop (16));
        slider.setBounds (b);
    }
};

struct Combo : public juce::Component
{
    juce::ComboBox box;
    juce::Label label;
    std::unique_ptr<APVTS::ComboBoxAttachment> att;
    bool labelLeft;

    Combo (APVTS& apvts, const juce::String& id, const juce::String& name, bool left = false) : labelLeft (left)
    {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id)))
            box.addItemList (p->choices, 1);
        box.setJustificationType (juce::Justification::centredLeft);
        label.setText (name, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions (12.f)));
        label.setColour (juce::Label::textColourId, col::dim);
        label.setJustificationType (left ? juce::Justification::centredLeft : juce::Justification::centredLeft);
        addAndMakeVisible (box);
        addAndMakeVisible (label);
        att = std::make_unique<APVTS::ComboBoxAttachment> (apvts, id, box);
    }
    void resized() override
    {
        auto b = getLocalBounds();
        if (labelLeft) { label.setBounds (b.removeFromLeft (juce::jmin (96, b.getWidth() / 2))); box.setBounds (b.reduced (0, 1)); }
        else           { label.setBounds (b.removeFromTop (16)); box.setBounds (b); }
    }
};

struct Toggle : public juce::ToggleButton
{
    std::unique_ptr<APVTS::ButtonAttachment> att;
    Toggle (APVTS& apvts, const juce::String& id, const juce::String& caption, juce::Colour c = col::accent)
    {
        setButtonText (caption);
        setColour (juce::ToggleButton::tickColourId, c);
        att = std::make_unique<APVTS::ButtonAttachment> (apvts, id, *this);
    }
};

inline void grid (juce::Rectangle<int> area, std::initializer_list<juce::Component*> items, int cols, int rowH)
{
    const int cellW = area.getWidth() / std::max (1, cols);
    int i = 0;
    for (auto* c : items)
    {
        c->setBounds (area.getX() + (i % cols) * cellW, area.getY() + (i / cols) * rowH, cellW, rowH);
        ++i;
    }
}

// -------------------------------------------------------------------------------------------------
struct LevelMeter : public juce::Component
{
    float value = 0.f, hold = 0.f;
    bool reduction = false;   // true: value is gain reduction in dB (draw downward from the top)
    void setLevel (float v) { value = v; repaint(); }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (col::bg); g.fillRoundedRectangle (r, 3.f);
        if (reduction)
        {
            const float h = juce::jmap (std::min (0.f, value), 0.f, -24.f, 0.f, r.getHeight());
            g.setColour (col::accent2);
            g.fillRoundedRectangle (r.getX(), r.getY(), r.getWidth(), h, 3.f);
        }
        else
        {
            const float db = juce::Decibels::gainToDecibels (value, -60.f);
            const float h = juce::jmap (db, -60.f, 0.f, 0.f, r.getHeight());
            g.setColour (db > -1.f ? juce::Colour (0xffff5a5a) : col::accent);
            g.fillRoundedRectangle (r.getX(), r.getBottom() - h, r.getWidth(), h, 3.f);
        }
        g.setColour (col::edge); g.drawRoundedRectangle (r.reduced (0.5f), 3.f, 1.f);
    }
};
} // namespace aio::ui
