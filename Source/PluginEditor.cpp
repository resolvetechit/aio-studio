#include "PluginEditor.h"
#include "Presets.h"
#include <deque>

using namespace aio;
using namespace aio::ui;

// =================================================================================================
// Panels
// =================================================================================================
class AIOEditor::TunePanel : public AIOEditor::PanelBase
{
public:
    explicit TunePanel (AIOProcessor& p)
        : on (p.apvts, "tuneOn", "PITCH CORRECTION ON"),
          key (p.apvts, "tuneKey", "Key"), scale (p.apvts, "tuneScale", "Scale"), voice (p.apvts, "tuneVoice", "Voice range"),
          link (p.apvts, "tuneLink", "Follow Compose key + scale", col::violet),
          speed (p.apvts, "tuneSpeed", "Retune Speed"), amount (p.apvts, "tuneAmount", "Amount"),
          fine (p.apvts, "tuneFine", "Fine Tune"), gate (p.apvts, "tuneGate", "Gate"),
          display (p)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &on, &key, &scale, &voice, &link, &speed, &amount, &fine, &gate, &display })
            addAndMakeVisible (c);
    }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, left, "Pitch Correction", col::accent);
        drawSection (g, right, "Pitch Monitor", col::accent);
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (12.f)));
        g.drawFittedText ("Real-time monophonic tuning for vocals and lead lines. Speed 0 ms = hard 'robot' tuning, "
                          "higher = natural. Adds ~32 ms latency while enabled (reported to the host).",
                          left.reduced (16, 0).removeFromBottom (92), juce::Justification::topLeft, 6);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        left = b.removeFromLeft (310);
        b.removeFromLeft (10);
        right = b;

        auto l = left.reduced (16, 36);
        on.setBounds (l.removeFromTop (32)); l.removeFromTop (10);
        key.setBounds (l.removeFromTop (42)); l.removeFromTop (6);
        scale.setBounds (l.removeFromTop (42)); l.removeFromTop (6);
        voice.setBounds (l.removeFromTop (42)); l.removeFromTop (10);
        link.setBounds (l.removeFromTop (28));

        auto r = right.reduced (16, 36);
        auto knobs = r.removeFromBottom (110);
        display.setBounds (r.withTrimmedBottom (10));
        grid (knobs, { &speed, &amount, &fine, &gate }, 4, 110);
    }

private:
    juce::Rectangle<int> left, right;
    Toggle on;
    Combo key, scale, voice;
    Toggle link;
    Knob speed, amount, fine, gate;
    PitchDisplay display;
};

// -------------------------------------------------------------------------------------------------
class AIOEditor::EQPanel : public AIOEditor::PanelBase
{
public:
    explicit EQPanel (AIOProcessor& p) : display (p), master (p.apvts, "eqOn", "EQ ON")
    {
        addAndMakeVisible (display);
        addAndMakeVisible (master);
        for (int i = 0; i < 6; ++i)
        {
            strips[(size_t) i].init (p.apvts, i);
            for (auto* c : strips[(size_t) i].all()) addAndMakeVisible (c);
        }
    }

    void paint (juce::Graphics& g) override
    {
        static const juce::Colour bc[6] = { juce::Colour (0xffff6b6b), juce::Colour (0xffffa94d), juce::Colour (0xffffe066),
                                            juce::Colour (0xff69db7c), juce::Colour (0xff4dabf7), juce::Colour (0xffb084ff) };
        for (int i = 0; i < 6; ++i)
            drawSection (g, rects[(size_t) i], "Band " + juce::String (i + 1), bc[i]);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto top = b.removeFromTop (b.getHeight() - 236);
        master.setBounds (top.removeFromTop (26).removeFromLeft (110));
        display.setBounds (top.withTrimmedTop (4));
        b.removeFromTop (8);
        const int w = b.getWidth() / 6;
        for (int i = 0; i < 6; ++i)
        {
            auto r = b.removeFromLeft (w).reduced (3, 0);
            rects[(size_t) i] = r;
            auto in = r.reduced (10, 28);
            auto& s = strips[(size_t) i];
            s.on->setBounds (in.removeFromTop (24)); in.removeFromTop (4);
            s.type->setBounds (in.removeFromTop (40)); in.removeFromTop (2);
            auto row = in.removeFromTop (96);
            const int cw = row.getWidth() / 3;
            s.freq->setBounds (row.removeFromLeft (cw));
            s.gain->setBounds (row.removeFromLeft (cw));
            s.q->setBounds (row);
        }
    }

private:
    struct Strip
    {
        std::unique_ptr<Toggle> on;
        std::unique_ptr<Combo> type;
        std::unique_ptr<Knob> freq, gain, q;
        void init (APVTS& a, int i)
        {
            on   = std::make_unique<Toggle> (a, eqId (i, "On"), "ENABLED");
            type = std::make_unique<Combo> (a, eqId (i, "Type"), "Type");
            freq = std::make_unique<Knob> (a, eqId (i, "Freq"), "Freq", 0);
            gain = std::make_unique<Knob> (a, eqId (i, "Gain"), "Gain", 1);
            q    = std::make_unique<Knob> (a, eqId (i, "Q"), "Q", 2);
        }
        std::array<juce::Component*, 5> all() { return { on.get(), type.get(), freq.get(), gain.get(), q.get() }; }
    };

    EQDisplay display;
    Toggle master;
    std::array<Strip, 6> strips;
    std::array<juce::Rectangle<int>, 6> rects;
};

// -------------------------------------------------------------------------------------------------
class AIOEditor::DynPanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit DynPanel (AIOProcessor& p)
        : proc (p),
          compOn (p.apvts, "compOn", "COMPRESSOR ON"),
          thresh (p.apvts, "compThresh", "Threshold", 1), ratio (p.apvts, "compRatio", "Ratio", 1),
          attack (p.apvts, "compAttack", "Attack", 1), release (p.apvts, "compRelease", "Release", 0),
          knee (p.apvts, "compKnee", "Knee", 1), makeup (p.apvts, "compMakeup", "Makeup", 1), cmix (p.apvts, "compMix", "Mix", 0),
          satOn (p.apvts, "satOn", "SATURATION ON"), satType (p.apvts, "satType", "Type"),
          drive (p.apvts, "satDrive", "Drive", 1), tone (p.apvts, "satTone", "Tone", 0),
          smix (p.apvts, "satMix", "Mix", 0), sout (p.apvts, "satOut", "Output", 1)
    {
        grLabel.setText ("GR", juce::dontSendNotification);
        grLabel.setJustificationType (juce::Justification::centred);
        grLabel.setColour (juce::Label::textColourId, col::dim);
        gr.reduction = true;
        for (auto* c : std::initializer_list<juce::Component*> { &compOn, &thresh, &ratio, &attack, &release, &knee, &makeup, &cmix, &gr, &grLabel,
                                                                  &satOn, &satType, &drive, &tone, &smix, &sout })
            addAndMakeVisible (c);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, left, "Compressor", col::accent);
        drawSection (g, right, "Saturation", col::accent2);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        left = b.removeFromLeft ((int) (b.getWidth() * 0.58f));
        b.removeFromLeft (10);
        right = b;

        auto l = left.reduced (16, 36);
        compOn.setBounds (l.removeFromTop (32).removeFromLeft (180)); l.removeFromTop (10);
        auto gm = l.removeFromRight (44);
        gr.setBounds (gm.withTrimmedTop (18).withTrimmedBottom (4).reduced (8, 0));
        grLabel.setBounds (gm.removeFromTop (16));
        grid (l, { &thresh, &ratio, &attack, &release, &knee, &makeup, &cmix }, 4, 128);

        auto r = right.reduced (16, 36);
        satOn.setBounds (r.removeFromTop (32).removeFromLeft (180)); r.removeFromTop (6);
        satType.setBounds (r.removeFromTop (42).removeFromLeft (200)); r.removeFromTop (8);
        grid (r, { &drive, &tone, &smix, &sout }, 2, 128);
    }

private:
    void timerCallback() override { if (isShowing()) gr.setLevel (proc.getGainReductionDb()); }

    AIOProcessor& proc;
    juce::Rectangle<int> left, right;
    Toggle compOn;
    Knob thresh, ratio, attack, release, knee, makeup, cmix;
    LevelMeter gr;
    juce::Label grLabel;
    Toggle satOn;
    Combo satType;
    Knob drive, tone, smix, sout;
};

// -------------------------------------------------------------------------------------------------
class AIOEditor::SpacePanel : public AIOEditor::PanelBase
{
public:
    explicit SpacePanel (AIOProcessor& p)
        : proc (p),
          choOn (p.apvts, "choOn", "CHORUS ON", col::accent2),
          choRate (p.apvts, "choRate", "Rate", 2, col::accent2), choDepth (p.apvts, "choDepth", "Depth", 0, col::accent2),
          choMix (p.apvts, "choMix", "Mix", 0, col::accent2), width (p.apvts, "width", "Stereo Width", 0, col::accent2),
          dlyOn (p.apvts, "dlyOn", "DELAY ON", col::accent2), dlySync (p.apvts, "dlySync", "SYNC", col::accent2),
          dlyPing (p.apvts, "dlyPing", "PING-PONG", col::accent2), dlyDiv (p.apvts, "dlyDiv", "Division"),
          dlyTime (p.apvts, "dlyTime", "Time (free)", 0, col::accent2), dlyFb (p.apvts, "dlyFeedback", "Feedback", 0, col::accent2),
          dlyTone (p.apvts, "dlyTone", "Tone", 0, col::accent2), dlyMix (p.apvts, "dlyMix", "Mix", 0, col::accent2),
          revOn (p.apvts, "revOn", "REVERB ON", col::accent2),
          revSize (p.apvts, "revSize", "Size", 0, col::accent2), revDamp (p.apvts, "revDamp", "Damping", 0, col::accent2),
          revWidth (p.apvts, "revWidth", "Width", 0, col::accent2), revPre (p.apvts, "revPre", "Pre-Delay", 0, col::accent2),
          revLow (p.apvts, "revLow", "Low Cut", 0, col::accent2), revHigh (p.apvts, "revHigh", "High Cut", 0, col::accent2),
          revMix (p.apvts, "revMix", "Mix", 0, col::accent2)
    {
        revPreset.addItemList ({ "Room", "Hall", "Plate", "Cathedral", "Ambience" }, 1);
        revPreset.setTextWhenNothingSelected ("Reverb presets...");
        revPreset.onChange = [this]
        {
            struct R { float size, damp, pre, low, high; };
            static const R r[5] = { { 35, 55, 5, 100, 10000 }, { 75, 45, 25, 120, 12000 }, { 55, 25, 10, 150, 14000 },
                                    { 95, 35, 40, 100, 9000 }, { 25, 65, 0, 150, 8000 } };
            const auto& v = r[std::clamp (revPreset.getSelectedId() - 1, 0, 4)];
            set ("revOn", 1); set ("revSize", v.size); set ("revDamp", v.damp); set ("revPre", v.pre); set ("revLow", v.low); set ("revHigh", v.high);
        };
        for (auto* c : std::initializer_list<juce::Component*> { &choOn, &choRate, &choDepth, &choMix, &width,
                                                                  &dlyOn, &dlySync, &dlyPing, &dlyDiv, &dlyTime, &dlyFb, &dlyTone, &dlyMix,
                                                                  &revOn, &revPreset, &revSize, &revDamp, &revWidth, &revPre, &revLow, &revHigh, &revMix })
            addAndMakeVisible (c);
    }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, ra, "Chorus + Width", col::accent2);
        drawSection (g, rb, "Delay", col::accent2);
        drawSection (g, rc, "Reverb", col::accent2);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        ra = r.removeFromLeft ((int) (r.getWidth() * 0.25f)); r.removeFromLeft (10);
        rb = r.removeFromLeft ((int) (r.getWidth() * 0.44f)); r.removeFromLeft (10);
        rc = r;

        auto x = ra.reduced (16, 36);
        choOn.setBounds (x.removeFromTop (32).removeFromLeft (150)); x.removeFromTop (10);
        grid (x, { &choRate, &choDepth, &choMix, &width }, 2, 136);

        auto y = rb.reduced (16, 36);
        auto t = y.removeFromTop (32);
        const int tw = t.getWidth() / 3;
        dlyOn.setBounds (t.removeFromLeft (tw).reduced (0, 0).withTrimmedRight (6));
        dlySync.setBounds (t.removeFromLeft (tw).withTrimmedRight (6));
        dlyPing.setBounds (t);
        y.removeFromTop (8);
        dlyDiv.setBounds (y.removeFromTop (42).removeFromLeft (180)); y.removeFromTop (8);
        grid (y, { &dlyTime, &dlyFb, &dlyTone, &dlyMix }, 2, 128);

        auto z = rc.reduced (16, 36);
        auto zt = z.removeFromTop (32);
        revOn.setBounds (zt.removeFromLeft (130)); zt.removeFromLeft (10);
        revPreset.setBounds (zt.removeFromLeft (160)); z.removeFromTop (10);
        grid (z, { &revSize, &revDamp, &revWidth, &revPre, &revLow, &revHigh, &revMix }, 4, 128);
    }

private:
    void set (const char* id, float v)
    {
        if (auto* p = proc.apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    AIOProcessor& proc;
    juce::Rectangle<int> ra, rb, rc;
    Toggle choOn;
    Knob choRate, choDepth, choMix, width;
    Toggle dlyOn, dlySync, dlyPing;
    Combo dlyDiv;
    Knob dlyTime, dlyFb, dlyTone, dlyMix;
    Toggle revOn;
    juce::ComboBox revPreset;
    Knob revSize, revDamp, revWidth, revPre, revLow, revHigh, revMix;
};

// -------------------------------------------------------------------------------------------------
class DragMidiButton : public juce::TextButton
{
public:
    explicit DragMidiButton (AIOProcessor& p) : proc (p) { setButtonText ("Drag MIDI to FL Studio"); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! started && e.getDistanceFromDragStart() > 6)
        {
            started = true;
            const auto file = proc.makeTempMidiFile();
            if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
                c->performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this, nullptr);
        }
        else if (! started)
            juce::TextButton::mouseDrag (e);
    }
    void mouseUp (const juce::MouseEvent& e) override { started = false; juce::TextButton::mouseUp (e); }

private:
    AIOProcessor& proc;
    bool started = false;
};

class AIOEditor::ComposePanel : public AIOEditor::PanelBase
{
public:
    explicit ComposePanel (AIOProcessor& p)
        : proc (p),
          on (p.apvts, "cmpOn", "COMPOSE ON", col::violet),
          dragBtn (p),
          roll (p),
          key (p.apvts, "cmpKey", "Key", true), scale (p.apvts, "cmpScale", "Scale", true), prog (p.apvts, "cmpProg", "Progression", true),
          bars (p.apvts, "cmpBars", "Length", true), chordLen (p.apvts, "cmpChordLen", "Chord length", true),
          chordKind (p.apvts, "cmpChordKind", "Chord type", true), voicing (p.apvts, "cmpVoicing", "Voicing", true),
          rhythm (p.apvts, "cmpRhythm", "Chord rhythm", true), smooth (p.apvts, "cmpSmooth", "Smooth voice-leading", col::violet),
          cOct (p.apvts, "cmpChordOct", "Octave", 0, col::chords), cVel (p.apvts, "cmpChordVel", "Velocity", 0, col::chords),
          cHuman (p.apvts, "cmpHuman", "Humanize", 0, col::chords), cSwing (p.apvts, "cmpSwing", "Swing", 0, col::chords),
          melOn (p.apvts, "cmpMelOn", "MELODY ON", col::melody), melStyle (p.apvts, "cmpMelStyle", "Style"), melRange (p.apvts, "cmpMelRange", "Range"),
          mDens (p.apvts, "cmpMelDensity", "Density", 0, col::melody), mRest (p.apvts, "cmpMelRests", "Rests", 0, col::melody),
          mOct (p.apvts, "cmpMelOct", "Octave", 0, col::melody), mVel (p.apvts, "cmpMelVel", "Velocity", 0, col::melody),
          bassOn (p.apvts, "cmpBassOn", "BASS ON", col::bass), bassStyle (p.apvts, "cmpBassStyle", "Style"),
          wChord (p.apvts, "synWaveChord", "Chords"), wMel (p.apvts, "synWaveMel", "Melody"), wBass (p.apvts, "synWaveBass", "Bass"),
          lChord (p.apvts, "synLevelChord", "Chords", 0, col::violet), lMel (p.apvts, "synLevelMel", "Melody", 0, col::violet),
          lBass (p.apvts, "synLevelBass", "Bass", 0, col::violet), sCut (p.apvts, "synCutoff", "Cutoff", 0, col::violet),
          sAtk (p.apvts, "synAttack", "Attack", 0, col::violet), sRel (p.apvts, "synRelease", "Release", 0, col::violet)
    {
        preview.setClickingTogglesState (true);
        preview.setButtonText ("Play preview");
        preview.onClick = [this]
        {
            proc.previewPlaying = preview.getToggleState();
            preview.setButtonText (preview.getToggleState() ? "Stop preview" : "Play preview");
        };
        newChords.setButtonText ("New chords");
        newChords.onClick = [this] { proc.newChordSeed(); };
        newMelody.setButtonText ("New melody");
        newMelody.onClick = [this] { proc.newMelodySeed(); };
        saveBtn.setButtonText ("Save .mid");
        saveBtn.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Save MIDI", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                                             .getChildFile ("AIO Chords + Melody.mid"), "*.mid");
            chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                                      | juce::FileBrowserComponent::warnAboutOverwriting,
                                  [this] (const juce::FileChooser& fc)
                                  {
                                      const auto f = fc.getResult();
                                      if (f != juce::File()) proc.writeMidiFile (f.withFileExtension ("mid"));
                                  });
        };
        dragBtn.setTooltip ("Drag this onto a Piano Roll or the Playlist in FL Studio");

        for (auto* c : std::initializer_list<juce::Component*> {
                 &on, &preview, &newChords, &newMelody, &dragBtn, &saveBtn, &roll,
                 &key, &scale, &prog, &bars, &chordLen, &chordKind, &voicing, &rhythm, &smooth,
                 &cOct, &cVel, &cHuman, &cSwing, &melOn, &melStyle, &melRange, &mDens, &mRest, &mOct, &mVel,
                 &bassOn, &bassStyle, &wChord, &wMel, &wBass, &lChord, &lMel, &lBass, &sCut, &sAtk, &sRel })
            addAndMakeVisible (c);
    }

    ~ComposePanel() override { proc.previewPlaying = false; }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, harmony, "Harmony", col::violet);
        drawSection (g, sChords, "Chords", col::chords);
        drawSection (g, sMelody, "Melody", col::melody);
        drawSection (g, sBass, "Bass", col::bass);
        drawSection (g, sSynth, "Preview synth", col::violet);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto bar = b.removeFromTop (36);
        on.setBounds (bar.removeFromLeft (130)); bar.removeFromLeft (8);
        preview.setBounds (bar.removeFromLeft (120)); bar.removeFromLeft (8);
        newChords.setBounds (bar.removeFromLeft (110)); bar.removeFromLeft (8);
        newMelody.setBounds (bar.removeFromLeft (110)); bar.removeFromLeft (20);
        dragBtn.setBounds (bar.removeFromLeft (200)); bar.removeFromLeft (8);
        saveBtn.setBounds (bar.removeFromLeft (100));
        b.removeFromTop (8);

        auto bottom = b.removeFromBottom (214);
        b.removeFromBottom (8);
        harmony = b.removeFromLeft (300);
        b.removeFromLeft (8);
        roll.setBounds (b);

        auto h = harmony.reduced (14, 32);
        for (auto* c : std::initializer_list<Combo*> { &key, &scale, &prog, &bars, &chordLen, &chordKind, &voicing, &rhythm })
            c->setBounds (h.removeFromTop (28));
        h.removeFromTop (6);
        smooth.setBounds (h.removeFromTop (26));

        const int W = bottom.getWidth();
        sChords = bottom.removeFromLeft ((int) (W * 0.21f)); bottom.removeFromLeft (8);
        sMelody = bottom.removeFromLeft ((int) (W * 0.31f)); bottom.removeFromLeft (8);
        sBass   = bottom.removeFromLeft ((int) (W * 0.11f)); bottom.removeFromLeft (8);
        sSynth  = bottom;

        grid (sChords.reduced (8, 30), { &cOct, &cVel, &cHuman, &cSwing }, 2, 88);

        auto m = sMelody.reduced (10, 30);
        auto mt = m.removeFromTop (40);
        melOn.setBounds (mt.removeFromLeft (92).withHeight (26).translated (0, 14)); mt.removeFromLeft (6);
        melStyle.setBounds (mt.removeFromLeft (mt.getWidth() / 2 - 3)); mt.removeFromLeft (6);
        melRange.setBounds (mt);
        grid (m, { &mDens, &mRest, &mOct, &mVel }, 4, 88);

        auto bs = sBass.reduced (10, 30);
        bassOn.setBounds (bs.removeFromTop (26)); bs.removeFromTop (6);
        bassStyle.setBounds (bs.removeFromTop (42));

        auto sy = sSynth.reduced (10, 30);
        auto sw = sy.removeFromTop (40);
        const int ww = sw.getWidth() / 3;
        wChord.setBounds (sw.removeFromLeft (ww).withTrimmedRight (6));
        wMel.setBounds (sw.removeFromLeft (ww).withTrimmedRight (6));
        wBass.setBounds (sw);
        grid (sy, { &lChord, &lMel, &lBass, &sCut, &sAtk, &sRel }, 6, 88);
    }

private:
    AIOProcessor& proc;
    juce::Rectangle<int> harmony, sChords, sMelody, sBass, sSynth;
    Toggle on;
    juce::TextButton preview, newChords, newMelody, saveBtn;
    DragMidiButton dragBtn;
    PianoRollView roll;
    Combo key, scale, prog, bars, chordLen, chordKind, voicing, rhythm;
    Toggle smooth;
    Knob cOct, cVel, cHuman, cSwing;
    Toggle melOn;
    Combo melStyle, melRange;
    Knob mDens, mRest, mOct, mVel;
    Toggle bassOn;
    Combo bassStyle;
    Combo wChord, wMel, wBass;
    Knob lChord, lMel, lBass, sCut, sAtk, sRel;
    std::unique_ptr<juce::FileChooser> chooser;
};

#include "ui/NewPanels.inc"

// =================================================================================================
// Editor
// =================================================================================================
AIOEditor::AIOEditor (AIOProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&laf);

    tune    = std::make_unique<TunePanel> (p);
    eqPanel = std::make_unique<EQPanel> (p);
    dyn     = std::make_unique<DynPanel> (p);
    space   = std::make_unique<SpacePanel> (p);
    compose = std::make_unique<ComposePanel> (p);
    panels = { tune.get(), eqPanel.get(), dyn.get(), space.get(), compose.get() };
#if ! JucePlugin_IsSynth
    master     = std::make_unique<MasterPanel> (p);
    voicePanel = std::make_unique<VoicePanel> (p);
    singPanel  = std::make_unique<SingPanel> (p);
    panels.add (master.get()); panels.add (voicePanel.get()); panels.add (singPanel.get());
#endif
    for (auto* c : panels) addChildComponent (c);

    static const char* names[8] = { "TUNE", "EQ", "DYNAMICS + SAT", "SPACE", "COMPOSE", "MASTER", "VOICE", "SING" };
    static const juce::Colour tabColours[8] = { col::accent, col::accent, col::accent, col::accent2, col::violet, col::accent2, col::chords, col::melody };
    for (int i = 0; i < panels.size(); ++i)
    {
        auto* b = tabButtons.add (new juce::TextButton (names[i]));
        b->setClickingTogglesState (true);
        b->setRadioGroupId (4242);
        b->setColour (juce::TextButton::buttonOnColourId, tabColours[i]);
        b->onClick = [this, i] { showTab (i); };
        addAndMakeVisible (b);
    }

    for (int i = 0; i < aio::numPresets(); ++i) presetBox.addItem (aio::presetName (i), i + 1);
    presetBox.setSelectedId (proc.getCurrentProgram() + 1, juce::dontSendNotification);
    presetBox.setTextWhenNothingSelected ("Presets");
    presetBox.onChange = [this] { proc.setCurrentProgram (presetBox.getSelectedId() - 1); };
    addAndMakeVisible (presetBox);

    inKnob   = std::make_unique<Knob> (p.apvts, "inGain", "Input", 1);
    outKnob  = std::make_unique<Knob> (p.apvts, "outGain", "Output", 1);
    ceilKnob = std::make_unique<Knob> (p.apvts, "limCeil", "Ceiling", 1);
    limToggle = std::make_unique<Toggle> (p.apvts, "limOn", "LIMITER");
    for (auto* c : std::initializer_list<juce::Component*> { inKnob.get(), outKnob.get(), ceilKnob.get(), limToggle.get(), &inMeter, &outMeter })
        addAndMakeVisible (c);

    setResizable (true, true);
    setResizeLimits (1100, 700, 1700, 1100);
    setSize (1180, 740);

    tabButtons[0]->setToggleState (true, juce::dontSendNotification);
    showTab (0);
    startTimerHz (30);
}

AIOEditor::~AIOEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void AIOEditor::showTab (int index)
{
    currentTab = index;
    for (int i = 0; i < panels.size(); ++i) panels[i]->setVisible (i == index);
    if (index >= 0 && index < tabButtons.size()) tabButtons[index]->setToggleState (true, juce::dontSendNotification);
}

void AIOEditor::timerCallback()
{
    inDisp  = std::max (proc.inPeak.load(),  inDisp * 0.86f);
    outDisp = std::max (proc.outPeak.load(), outDisp * 0.86f);
    inMeter.setLevel (inDisp);
    outMeter.setLevel (outDisp);
}

void AIOEditor::paint (juce::Graphics& g)
{
    g.fillAll (col::bg);
    g.setColour (col::text);
    g.setFont (juce::Font (juce::FontOptions (26.f, juce::Font::bold)));
    g.drawText ("AIO STUDIO", 20, 14, 240, 32, juce::Justification::centredLeft);
    g.setColour (col::accent);
    g.fillRoundedRectangle (20.f, 50.f, 48.f, 3.f, 1.5f);
    g.setColour (col::dim);
    g.setFont (juce::Font (juce::FontOptions (11.5f)));
    #if JucePlugin_IsSynth
    g.drawText ("tune  /  eq  /  dynamics  /  space  /  compose", 20, 58, 300, 16, juce::Justification::centredLeft);
#else
    g.drawText ("tune / eq / dynamics / space / compose / master / voice / sing", 20, 58, 420, 16, juce::Justification::centredLeft);
#endif
}

void AIOEditor::resized()
{
    auto b = getLocalBounds();
    auto top = b.removeFromTop (92).reduced (16, 6);

    auto right = top.removeFromRight (420);
    outMeter.setBounds (right.removeFromRight (12).reduced (0, 4)); right.removeFromRight (4);
    limToggle->setBounds (right.removeFromRight (78).withHeight (26).translated (0, 22)); right.removeFromRight (4);
    ceilKnob->setBounds (right.removeFromRight (74));
    outKnob->setBounds (right.removeFromRight (74));
    inKnob->setBounds (right.removeFromRight (74));
    inMeter.setBounds (right.removeFromRight (12).reduced (0, 4));

    presetBox.setBounds (top.removeFromLeft (560).withTrimmedLeft (330).withHeight (28).translated (0, 22));

    auto tabs = b.removeFromTop (34).reduced (16, 0);
    const int tw = panels.size() > 5 ? 143 : 150;
    for (auto* t : tabButtons) { t->setBounds (tabs.removeFromLeft (tw).withTrimmedRight (6)); }

    b.removeFromTop (8);
    auto content = b.reduced (16, 0).withTrimmedBottom (14);
    for (auto* c : panels) c->setBounds (content);
}
