// Included once from PluginEditor.cpp, after NewPanels.inc: the LAYERS panel (both builds) and the VOICE MORPH panel (effect build).

// =================================================================================================
// LAYERS
// =================================================================================================
class AIOEditor::LayersPanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit LayersPanel (AIOProcessor& p)
        : proc (p), keys (p),
          onT (p.apvts, "lyOn", "LAYERS ON", layersCol),
          latchT (p.apvts, "lyHold", "Latch keys", layersCol), muteT (p.apvts, "lyMuteSyn", "Mute synth", layersCol),
          source (p.apvts, "lySource", "Notes come from"),
          kLevel (p.apvts, "lyLevel", "Level", 1, layersCol), kBright (p.apvts, "lyBright", "Brightness", 0, layersCol),
          kMotion (p.apvts, "lyMotion", "Motion", 0, layersCol), kRel (p.apvts, "lyRelease", "Release", 0, layersCol),
          kVel (p.apvts, "lyVel", "Velocity", 0, layersCol)
    {
        for (int i = 0; i < aio::kLayers; ++i)
        {
            rows.add (new Row (p, i, [this] (int k) { select (k); }));
            details.add (new LayerDetail (p, i));
            addAndMakeVisible (rows.getLast());
            addChildComponent (details.getLast());
        }
        for (auto* c : std::initializer_list<juce::Component*> { &keys, &onT, &latchT, &muteT, &source, &kLevel, &kBright, &kMotion, &kRel, &kVel, &soundBox, &randomBtn, &hint })
            addAndMakeVisible (c);

        soundBox.setTextWhenNothingSelected ("Sound browser...");
        for (int i = 0; i < AIOProcessor::layerPresetCount(); ++i) soundBox.addItem (AIOProcessor::layerPresetName (i), i + 1);
        soundBox.onChange = [this]
        {
            const int id = soundBox.getSelectedId();
            if (id > 0) { proc.applyLayerPreset (id - 1); for (auto* d : details) d->refreshName(); }
        };
        randomBtn.setButtonText ("Randomise");
        randomBtn.onClick = [this] { proc.randomizeLayers(); soundBox.setSelectedId (0, juce::dontSendNotification); };
        hint.setText ("Nine sounds on every key. Click a layer to edit it, lock the ones you want to keep, then browse or randomise the rest.", juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (12.f)));
        hint.setColour (juce::Label::textColourId, col::dim);
        hint.setMinimumHorizontalScale (0.7f);
        select (0);
        startTimerHz (20);
    }
    ~LayersPanel() override { stopTimer(); }

    LayerKeys& getKeys() { return keys; }
    int selected() const { return sel; }
    void selectLayer (int i) { select (i); }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, listRect, "Layers", layersCol);
        drawSection (g, detailRect, "Layer " + juce::String (sel + 1) + " - " + juce::String (aio::layerSrcName ((int) proc.apvts.getRawParameterValue ("ly" + juce::String (sel) + "Src")->load())), layersCol);
        drawSection (g, macroRect, "All layers", layersCol);
        drawSection (g, notesRect, "Notes", layersCol);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto bar = b.removeFromTop (34);
        onT.setBounds (bar.removeFromLeft (150).withTrimmedRight (8));
        soundBox.setBounds (bar.removeFromLeft (230).reduced (0, 3).withTrimmedRight (8));
        randomBtn.setBounds (bar.removeFromLeft (110).reduced (0, 3).withTrimmedRight (8));
        hint.setBounds (bar.withTrimmedLeft (4));
        b.removeFromTop (6);

        auto main = b.removeFromTop (352); b.removeFromTop (6);
        listRect = main.removeFromLeft (main.getWidth() * 45 / 100);
        main.removeFromLeft (8);
        detailRect = main;
        {
            auto a = listRect.reduced (10, 30);
            const int rh = a.getHeight() / aio::kLayers;
            for (auto* r : rows) r->setBounds (a.removeFromTop (rh).reduced (0, 1));
            auto d = detailRect.reduced (12, 30);
            for (auto* x : details) x->setBounds (d);
        }

        macroRect = b.removeFromLeft (b.getWidth() * 4 / 10);
        b.removeFromLeft (8);
        notesRect = b;
        {
            auto a = macroRect.reduced (10, 30);
            const int kw = a.getWidth() / 5;
            for (auto* k : { &kLevel, &kBright, &kMotion, &kRel, &kVel }) k->setBounds (a.removeFromLeft (kw));
            auto n = notesRect.reduced (14, 32);
            auto side = n.removeFromRight (juce::jmin (260, n.getWidth() / 3));
            n.removeFromRight (12);
            keys.setBounds (n);
            source.setBounds (side.removeFromTop (44)); side.removeFromTop (6);
            auto row = side.removeFromTop (28);
            latchT.setBounds (row.removeFromLeft (row.getWidth() / 2));
            muteT.setBounds (row);
        }
    }

private:
    class Row : public juce::Component
    {
    public:
        Row (AIOProcessor& p, int index, std::function<void (int)> onSel)
            : proc (p), idx (index), select (std::move (onSel)),
              on (p.apvts, "ly" + juce::String (index) + "On", juce::String (index + 1), layersCol),
              lock (p.apvts, "ly" + juce::String (index) + "Lock", "lock", layersCol)
        {
            addAndMakeVisible (on); addAndMakeVisible (src); addAndMakeVisible (level); addAndMakeVisible (lock);
            if (auto* pc = dynamic_cast<juce::AudioParameterChoice*> (p.apvts.getParameter ("ly" + juce::String (index) + "Src"))) src.addItemList (pc->choices, 1);
            srcAtt = std::make_unique<APVTS::ComboBoxAttachment> (p.apvts, "ly" + juce::String (index) + "Src", src);
            level.setSliderStyle (juce::Slider::LinearHorizontal);
            level.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            level.setColour (juce::Slider::trackColourId, layersCol);
            levelAtt = std::make_unique<APVTS::SliderAttachment> (p.apvts, "ly" + juce::String (index) + "Level", level);
            src.onChange = [this] { if (auto* par = getParentComponent()) par->repaint(); };
        }
        void setSelected (bool s) { if (s != isSel) { isSel = s; repaint(); } }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (isSel ? layersCol.withAlpha (0.16f) : col::bg.withAlpha (0.6f));
            g.fillRoundedRectangle (r, 5.f);
            if (isSel) { g.setColour (layersCol.withAlpha (0.7f)); g.drawRoundedRectangle (r.reduced (0.5f), 5.f, 1.2f); }
            const float m = std::clamp (proc.getLayers().layerMeter[(size_t) idx].load() * 2.2f, 0.f, 1.f);
            const auto mr = juce::Rectangle<float> (r.getRight() - 118.f, r.getY() + r.getHeight() * 0.5f - 2.5f, 54.f, 5.f);
            g.setColour (col::edge); g.fillRoundedRectangle (mr, 2.f);
            g.setColour (layersCol); g.fillRoundedRectangle (mr.withWidth (mr.getWidth() * m), 2.f);
        }
        void resized() override
        {
            auto b = getLocalBounds().reduced (6, 2);
            on.setBounds (b.removeFromLeft (46));
            src.setBounds (b.removeFromLeft (118).reduced (0, 1)); b.removeFromLeft (8);
            lock.setBounds (b.removeFromRight (56));
            b.removeFromRight (64);                           // meter
            level.setBounds (b);
        }
        void mouseDown (const juce::MouseEvent&) override { select (idx); }

    private:
        AIOProcessor& proc;
        int idx; std::function<void (int)> select;
        bool isSel = false;
        Toggle on, lock;
        juce::ComboBox src;
        juce::Slider level;
        std::unique_ptr<APVTS::ComboBoxAttachment> srcAtt;
        std::unique_ptr<APVTS::SliderAttachment> levelAtt;
    };

    void select (int i)
    {
        sel = std::clamp (i, 0, aio::kLayers - 1);
        for (int k = 0; k < rows.size(); ++k) { rows[k]->setSelected (k == sel); details[k]->setVisible (k == sel); }
        repaint();
    }
    void timerCallback() override { for (auto* r : rows) r->repaint(); }

    AIOProcessor& proc;
    LayerKeys keys;
    Toggle onT, latchT, muteT;
    Combo source;
    Knob kLevel, kBright, kMotion, kRel, kVel;
    juce::ComboBox soundBox;
    juce::TextButton randomBtn;
    juce::Label hint;
    juce::OwnedArray<Row> rows;
    juce::OwnedArray<LayerDetail> details;
    juce::Rectangle<int> listRect, detailRect, macroRect, notesRect;
    int sel = 0;
};

#if ! JucePlugin_IsSynth
class AIOEditor::MorphPanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit MorphPanel (AIOProcessor& p)
        : proc (p), pad (p),
          onT (p.apvts, "vcOn", "VOICE MORPH ON", morphCol),
          kPitch (p.apvts, "vcPitch", "Pitch", 1, morphCol), kFormant (p.apvts, "vcFormant", "Formant (throat size)", 1, morphCol),
          kWarmth (p.apvts, "vcWarmth", "Warmth", 0, morphCol), kAir (p.apvts, "vcAir", "Air", 0, morphCol),
          kGrit (p.apvts, "vcGrit", "Grit", 0, morphCol), kLevel (p.apvts, "vcLevel", "Level", 1, morphCol)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &pad, &onT, &kPitch, &kFormant, &kWarmth, &kAir, &kGrit, &kLevel, &hint, &readout })
            addAndMakeVisible (c);
        kPitch.slider.setTextValueSuffix ({}); kFormant.slider.setTextValueSuffix ({});
        kPitch.slider.textFromValueFunction = [] (double v) { return (v > 0.04 ? "+" : "") + juce::String (v, 1) + " st"; };
        kFormant.slider.textFromValueFunction = [] (double v) { return (v > 0.04 ? "+" : "") + juce::String (v, 1) + " st"; };
        kPitch.slider.updateText(); kFormant.slider.updateText();
        for (int i = 0; i < AIOProcessor::voicePresetCount(); ++i)
        {
            auto* b = presetBtns.add (new juce::TextButton (AIOProcessor::voicePresetName (i)));
            b->onClick = [this, i] { proc.applyVoicePreset (i); pad.refresh(); };
            addAndMakeVisible (b);
        }
        hint.setText ("Changes the character of YOUR voice live: pitch, throat size, warmth, air and grit. It does not copy anyone else's voice. Use headphones and sing or speak into the plug-in's input. Big moves (beyond about 7 semitones) get more artefacts.", juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (12.f)));
        hint.setColour (juce::Label::textColourId, col::dim);
        hint.setJustificationType (juce::Justification::topLeft);
        readout.setFont (juce::Font (juce::FontOptions (22.f, juce::Font::bold)));
        readout.setColour (juce::Label::textColourId, morphCol);
        readout.setJustificationType (juce::Justification::centredLeft);
        startTimerHz (15);
    }
    ~MorphPanel() override { stopTimer(); }

    CharacterPad& getPad() { return pad; }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, padRect, "Character map (drag the dot, double-click to reset)", morphCol);
        drawSection (g, knobRect, "Voice", morphCol);
        drawSection (g, presetRect, "Starting points", morphCol);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto bar = b.removeFromTop (34);
        onT.setBounds (bar.removeFromLeft (190).withTrimmedRight (8));
        readout.setBounds (bar.withTrimmedLeft (10));
        b.removeFromTop (6);
        padRect = b.removeFromLeft (b.getWidth() * 5 / 11);
        b.removeFromLeft (8);
        pad.setBounds (padRect.reduced (14, 34));
        auto right = b;
        knobRect = right.removeFromTop (230);
        right.removeFromTop (8);
        presetRect = right.removeFromTop (right.getHeight() - 70);
        hint.setBounds (right.reduced (4, 2));
        {
            auto a = knobRect.reduced (12, 32);
            const int cw = a.getWidth() / 3, rh = a.getHeight() / 2;
            auto r1 = a.removeFromTop (rh), r2 = a;
            for (auto* k : { &kPitch, &kFormant, &kWarmth }) k->setBounds (r1.removeFromLeft (cw));
            for (auto* k : { &kAir, &kGrit, &kLevel }) k->setBounds (r2.removeFromLeft (cw));
            auto pr = presetRect.reduced (14, 36);
            const int cols = 3, bw = pr.getWidth() / cols, bh = pr.getHeight() / 3;
            for (int i = 0; i < presetBtns.size(); ++i)
                presetBtns[i]->setBounds (pr.getX() + (i % cols) * bw, pr.getY() + (i / cols) * bh, bw, bh);
            for (auto* pb : presetBtns) pb->setBounds (pb->getBounds().reduced (3));
        }
    }

private:
    void timerCallback() override
    {
        const float det = proc.getDetectedMidi();
        const float sh = proc.apvts.getRawParameterValue ("vcPitch")->load();
        if (proc.apvts.getRawParameterValue ("vcOn")->load() < 0.5f) readout.setText ("Off", juce::dontSendNotification);
        else if (det > 0.f) readout.setText (keyName ((int) std::lround (det)) + "  ->  " + keyName ((int) std::lround (det + sh)), juce::dontSendNotification);
        else readout.setText ("waiting for your voice...", juce::dontSendNotification);
        pad.refresh();
    }

    AIOProcessor& proc;
    CharacterPad pad;
    Toggle onT;
    Knob kPitch, kFormant, kWarmth, kAir, kGrit, kLevel;
    juce::OwnedArray<juce::TextButton> presetBtns;
    juce::Label hint, readout;
    juce::Rectangle<int> padRect, knobRect, presetRect;
};
#endif
