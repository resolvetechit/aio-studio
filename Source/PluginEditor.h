#pragma once
#include "ui/Widgets.h"
#include "ui/Displays.h"

namespace aio::ui { class VocalRoll; class TalkKeys; class LayerKeys; class CharacterPad; }

class AIOEditor : public juce::AudioProcessorEditor,
                  public juce::DragAndDropContainer,
                  private juce::Timer
{
public:
    explicit AIOEditor (AIOProcessor&);
    ~AIOEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void showTab (int index);
    void setComposeView (int view);      // 0 arrange, 1 settings
    aio::ui::VocalRoll* vocalRoll();
    aio::ui::LayerKeys* layerKeys();     // the on-screen keys of the LAYERS tab; used by tests
    aio::ui::CharacterPad* characterPad(); // the character map of the VOICE MORPH tab (nullptr in the instrument build); used by tests
    int  layersTabIndex() const { return layersTab; }
    int  morphTabIndex() const { return morphTab; }
    void selectTab (int index) { showTab (index); }
    aio::ui::TalkKeys*  talkKeys();      // the on-screen keys of the TALK BOX tab (nullptr in the instrument build); used by tests     // the note editor of the VOCAL EDIT tab (nullptr in the instrument build); used by tests

private:
    struct PanelBase : public juce::Component {};
    class TunePanel; class EQPanel; class DynPanel; class SpacePanel; class ComposePanel;
    class MasterPanel; class VoicePanel; class SingPanel; class VocalPanel; class TalkPanel; class LayersPanel; class MorphPanel;

    void timerCallback() override;

    AIOProcessor& proc;
    aio::ui::AIOLookAndFeel laf;

    juce::ComboBox presetBox;
    aio::ui::LevelMeter inMeter, outMeter;
    std::unique_ptr<aio::ui::Knob> inKnob, outKnob, ceilKnob;
    std::unique_ptr<aio::ui::Toggle> limToggle;
    float inDisp = 0.f, outDisp = 0.f;

    juce::OwnedArray<juce::TextButton> tabButtons;
    std::unique_ptr<TunePanel>    tune;
    std::unique_ptr<EQPanel>      eqPanel;
    std::unique_ptr<DynPanel>     dyn;
    std::unique_ptr<SpacePanel>   space;
    std::unique_ptr<ComposePanel> compose;
    std::unique_ptr<MasterPanel>  master;       // effect build only
    std::unique_ptr<VoicePanel>   voicePanel;
    std::unique_ptr<SingPanel>    singPanel;
    std::unique_ptr<LayersPanel>  layersPanel;
#if ! JucePlugin_IsSynth
    std::unique_ptr<VocalPanel>   vocalPanel;
    std::unique_ptr<TalkPanel>    talkPanel;
    std::unique_ptr<MorphPanel>   morphPanel;
#endif
    juce::Array<juce::Component*> panels;
    int currentTab = 0, layersTab = -1, morphTab = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AIOEditor)
};
