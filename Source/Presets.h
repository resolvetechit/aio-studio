#pragma once
#include <juce_audio_processors/juce_audio_processors.h>

namespace aio
{
int numPresets();
const char* presetName (int index);
// Resets every non-compose parameter to its default, then applies the preset's overrides.
void applyPreset (juce::AudioProcessorValueTreeState& apvts, int index);
}
