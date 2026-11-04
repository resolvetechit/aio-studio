#pragma once
#include <array>
#include <vector>
#include <functional>
#include <cstdint>
#include <juce_dsp/juce_dsp.h>
#include "../dsp/Chain.h"

/*
   Auto Master engine.

     Analyser      streaming measurements: BS.1770 loudness, true peak, 1/3-octave spectrum, stereo balance
     deriveMaster  measurements + options  ->  EQ / compressor / saturation / mono-bass / gain settings (no audio needed)
     refineMaster  offline: tunes the compressor threshold on the real audio and solves the gain so the result lands on
                   the target loudness (through the same DSP chain the plug-in runs in real time)
     renderPass    runs a whole file through that chain (streaming), returning the measured result
*/
namespace aio
{
constexpr int kMB = 30;   // third-octave bands, ~25 Hz .. 20 kHz

inline float bandCentreHz (int j) { return 1000.f * std::pow (2.f, (float) (j - 16) / 3.f); }

struct MasterAnalysis
{
    bool   valid = false;
    double sampleRate = 44100.0, seconds = 0.0;
    double lufs = -200.0, lufsMomentaryMax = -200.0, loudHigh = -200.0, loudLow = -200.0;   // integrated, max, 95th / 10th percentile of 400 ms blocks
    double samplePeakDb = -200.0, truePeakDb = -200.0, plr = 0.0;
    double correlation = 1.0, sideMidDb = -100.0, lowSideMidDb = -100.0;
    std::array<float, kMB> bandDb {};
    std::array<bool, kMB> bandValid {};
};

class Analyser
{
public:
    Analyser();
    void prepare (double sampleRate);
    void reset();
    void push (const float* l, const float* r, int n);
    MasterAnalysis result() const;
    double seconds() const { return sr > 0 ? (double) total / sr : 0.0; }

private:
    void runFrame();

    static constexpr int kN = 4096, kHop = 2048;
    double sr = 44100.0;
    long long total = 0;
    LoudnessMeter lm;
    TruePeak tpL, tpR;
    Biquad lpM, lpS;
    double eM = 0, eS = 0, eLowM = 0, eLowS = 0, sLL = 0, sRR = 0, sLR = 0;
    juce::dsp::FFT fft { 12 };
    std::vector<float> ring, fftData, window;
    std::vector<double> acc;
    int ringPos = 0, sinceHop = 0;
    long long filled = 0, frames = 0;
};

struct MasterOptions
{
    int   style = 0;            // 0 Balanced, 1 Warm, 2 Bright, 3 Punchy, 4 Loud
    float targetLufs = -14.f;
    float ceilingDb = -1.f;
    float amount = 0.6f;        // tonal correction strength 0..1
    float glue = 0.5f;          // compression 0..1
    float warmth = 0.3f;        // saturation 0..1
    bool  monoBass = true;
};

inline const char* styleName (int s)
{
    static const char* n[5] = { "Balanced", "Warm", "Bright", "Punchy", "Loud" };
    return n[std::clamp (s, 0, 4)];
}

struct MasterSettings
{
    std::array<EQBand, ParametricEQ::kBands> eq;
    bool eqOn = true;
    bool compOn = false;  CompSettings comp;
    bool satOn = false;   SatSettings sat;
    float monoBassHz = 0.f;
    float outGainDb = 0.f, ceilingDb = -1.f;
    float targetMeanReductionDb = 2.f;

    // for the UI
    std::array<float, kMB> targetDb {}, errorDb {}, correctionDb {};
};

float targetCurveDb (float hz, int style);                                    // dB relative to 1 kHz (1/3-octave energy)
MasterSettings deriveMaster (const MasterAnalysis&, const MasterOptions&);
ChainParams chainFromSettings (const MasterSettings&);

struct RenderStats { double lufs = -200.0, truePeakDb = -200.0, samplePeakDb = -200.0; bool cancelled = false; };
using Sink = std::function<void (const float*, const float*, int)>;
using Progress = std::function<bool (float)>;      // return false to cancel

RenderStats renderPass (const float* l, const float* r, size_t n, double sr, const ChainParams& p, const Sink& sink, const Progress& progress);
MasterAnalysis analyseBuffer (const float* l, const float* r, size_t n, double sr, const Progress& progress = {});
MasterSettings refineMaster (const float* l, const float* r, size_t n, double sr, const MasterAnalysis& a,
                             MasterSettings s, const MasterOptions& o, RenderStats* finalStats, const Progress& progress);
} // namespace aio
