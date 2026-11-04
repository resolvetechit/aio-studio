#pragma once

// Fallbacks so the same sources also compile inside the offline test runner,
// where juce_add_plugin() has not defined the JucePlugin_* macros.
#ifndef JucePlugin_IsSynth
 #define JucePlugin_IsSynth 0
#endif
#ifndef JucePlugin_Name
 #define JucePlugin_Name "AIO Studio"
#endif
#ifndef JucePlugin_ProducesMidiOutput
 #define JucePlugin_ProducesMidiOutput 0
#endif
