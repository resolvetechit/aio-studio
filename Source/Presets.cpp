#include "Presets.h"
#include "Params.h"

namespace aio
{
namespace
{
struct Set { const char* id; float value; };
struct Preset { const char* name; std::vector<Set> sets; };

const std::vector<Preset>& presets()
{
    static const std::vector<Preset> p = {
        { "Init", {} },

        { "Vocal Polish", {
            { "eqOn", 1 },
            { "eq1On", 1 }, { "eq1Freq", 90 },
            { "eq3Freq", 300 }, { "eq3Gain", -2.5f },
            { "eq5Freq", 4000 }, { "eq5Gain", 2.0f },
            { "eq6Freq", 11000 }, { "eq6Gain", 2.5f },
            { "compOn", 1 }, { "compThresh", -20 }, { "compRatio", 3 }, { "compAttack", 8 }, { "compRelease", 100 }, { "compMakeup", 4 },
            { "satOn", 1 }, { "satType", 0 }, { "satDrive", 4 }, { "satMix", 35 },
            { "dlyOn", 1 }, { "dlyDiv", 1 }, { "dlyFeedback", 25 }, { "dlyMix", 12 },
            { "revOn", 1 }, { "revSize", 40 }, { "revPre", 20 }, { "revMix", 14 } } },

        { "Hard Tune (T-Pain)", {
            { "tuneOn", 1 }, { "tuneSpeed", 0 }, { "tuneAmount", 100 },
            { "compOn", 1 }, { "compThresh", -22 }, { "compRatio", 4 }, { "compMakeup", 5 },
            { "revOn", 1 }, { "revSize", 35 }, { "revMix", 15 },
            { "dlyOn", 1 }, { "dlyMix", 15 } } },

        { "Natural Tune", {
            { "tuneOn", 1 }, { "tuneSpeed", 45 }, { "tuneAmount", 85 },
            { "eq1On", 1 }, { "eq1Freq", 80 } } },

        { "Warm Master Glue", {
            { "eqOn", 1 }, { "eq2Freq", 100 }, { "eq2Gain", 1.5f }, { "eq6Freq", 12000 }, { "eq6Gain", 1.5f },
            { "compOn", 1 }, { "compThresh", -14 }, { "compRatio", 2 }, { "compAttack", 30 }, { "compRelease", 200 }, { "compKnee", 10 }, { "compMakeup", 2 },
            { "satOn", 1 }, { "satType", 0 }, { "satDrive", 3 }, { "satMix", 25 },
            { "width", 115 }, { "limOn", 1 }, { "limCeil", -0.3f } } },

        { "Lo-Fi Crunch", {
            { "eqOn", 1 }, { "eq1On", 1 }, { "eq1Freq", 150 }, { "eq6On", 1 }, { "eq6Type", 4 }, { "eq6Freq", 5000 },
            { "satOn", 1 }, { "satType", 3 }, { "satDrive", 14 }, { "satTone", 6000 }, { "satMix", 60 },
            { "choOn", 1 }, { "choRate", 0.3f }, { "choDepth", 50 }, { "choMix", 40 },
            { "revOn", 1 }, { "revSize", 30 }, { "revMix", 12 } } },

        { "Wide Pad Space", {
            { "choOn", 1 }, { "choRate", 0.5f }, { "choDepth", 60 }, { "choMix", 50 },
            { "dlyOn", 1 }, { "dlyDiv", 4 }, { "dlyFeedback", 45 }, { "dlyMix", 30 },
            { "revOn", 1 }, { "revSize", 85 }, { "revDamp", 40 }, { "revPre", 30 }, { "revMix", 40 },
            { "width", 150 } } },

        { "Bass Weight", {
            { "eqOn", 1 }, { "eq2Freq", 60 }, { "eq2Gain", 3 }, { "eq3Freq", 250 }, { "eq3Gain", -3 },
            { "compOn", 1 }, { "compThresh", -16 }, { "compRatio", 4 }, { "compAttack", 5 }, { "compRelease", 80 },
            { "satOn", 1 }, { "satType", 1 }, { "satDrive", 9 }, { "satTone", 5000 }, { "satMix", 60 } } },
    };
    return p;
}

void setParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, float actual)
{
    if (auto* p = apvts.getParameter (id))
        p->setValueNotifyingHost (p->convertTo0to1 (actual));
}
} // namespace

int numPresets() { return (int) presets().size(); }

const char* presetName (int i) { return presets()[(size_t) std::clamp (i, 0, numPresets() - 1)].name; }

void applyPreset (juce::AudioProcessorValueTreeState& apvts, int index)
{
    index = std::clamp (index, 0, numPresets() - 1);
    for (auto* p : apvts.processor.getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto id = r->getParameterID();
            if (id.startsWith ("cmp") || id.startsWith ("syn") || id.startsWith ("ved") || id.startsWith ("tuneKey") || id.startsWith ("tuneScale")
                || id.startsWith ("mast") || id.startsWith ("sing") || id.startsWith ("tuneNote") || id.startsWith ("tuneVoice") || id.startsWith ("tuneLink"))
                continue;
            r->setValueNotifyingHost (r->getDefaultValue());
        }
    for (auto& s : presets()[(size_t) index].sets)
        setParam (apvts, s.id, s.value);
}
} // namespace aio
