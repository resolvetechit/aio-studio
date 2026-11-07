// =================================================================================================================
// Talk box tests: synthetic vowels (a glottal pulse train through formant resonators) drive the talk box
// =================================================================================================================
#include "../Source/dsp/TalkBox.h"
#include "../Source/Presets.h"
#include <chrono>

namespace
{
struct VowelF { double f[3], bw[3]; };
VowelF vowelOf (int v)
{
    static const VowelF t[5] = {
        { { 730, 1090, 2440 }, { 90, 110, 160 } },     // a
        { { 270, 2290, 3010 }, { 60, 110, 160 } },     // i
        { { 300, 870, 2240 },  { 60, 110, 160 } },     // u
        { { 530, 1840, 2480 }, { 70, 110, 160 } },     // e
        { { 570, 840, 2410 },  { 70, 110, 160 } } };   // o
    return t[v % 5];
}

// vowel sequence at a fixed pitch (f0Hz), seconds per vowel
std::vector<float> vowelVoice (double sr, double f0Hz, const std::vector<int>& vowels, double secEach, double rmsTarget = 0.12)
{
    std::vector<float> x;
    double nextPulse = 0.0, lp = 0.0;
    double y1[3] = { 0, 0, 0 }, y2[3] = { 0, 0, 0 };
    size_t idx = 0;
    for (int v : vowels)
    {
        const auto F = vowelOf (v);
        double a1[3], a2[3], g[3];
        for (int k = 0; k < 3; ++k)
        {
            const double r = std::exp (-aio::kPi * F.bw[k] / sr);
            a1[k] = 2.0 * r * std::cos (2.0 * aio::kPi * F.f[k] / sr); a2[k] = -r * r; g[k] = 1.0 - r;
        }
        const size_t n = (size_t) std::lround (secEach * sr);
        for (size_t i = 0; i < n; ++i, ++idx)
        {
            double s = 0.0;
            if ((double) idx >= nextPulse) { s = 1.0; nextPulse += sr / f0Hz; }
            lp += 0.18 * (s - lp);
            double sig = lp;
            for (int k = 0; k < 3; ++k)
            {
                const double y = g[k] * sig + a1[k] * y1[k] + a2[k] * y2[k];
                y2[k] = y1[k]; y1[k] = y; sig = y;
            }
            x.push_back ((float) sig);
        }
    }
    double e = 0; for (float v : x) e += (double) v * v;
    const double sc = rmsTarget / std::sqrt (e / std::max<size_t> (1, x.size()));
    for (auto& v : x) v = (float) (v * sc);
    return x;
}

// energy in 8 bands of 600 Hz, as dB, from a few Hann FFT frames inside [t0, t1)
std::array<double, 8> bandDb (const std::vector<float>& x, double sr, double t0, double t1)
{
    juce::dsp::FFT fft (12);
    const int N = 4096;
    std::array<double, 8> acc {};
    int frames = 0;
    for (double t = t0; t + (double) N / sr <= t1 + 1e-9; t += 1024.0 / sr)
    {
        std::vector<float> buf ((size_t) N * 2, 0.f);
        const size_t s0 = (size_t) std::lround (t * sr);
        if (s0 + (size_t) N > x.size()) break;
        for (int i = 0; i < N; ++i) buf[(size_t) i] = x[s0 + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * aio::kPi * (i + 0.5) / N));
        fft.performFrequencyOnlyForwardTransform (buf.data());
        for (int b = 0; b < 8; ++b)
        {
            double e = 0;
            const int k0 = (int) std::lround (b * 600.0 / sr * N), k1 = (int) std::lround ((b + 1) * 600.0 / sr * N);
            for (int k = std::max (1, k0); k < k1; ++k) e += (double) buf[(size_t) k] * buf[(size_t) k];
            acc[(size_t) b] += e;
        }
        ++frames;
    }
    std::array<double, 8> out {};
    for (int b = 0; b < 8; ++b) out[(size_t) b] = 10.0 * std::log10 (acc[(size_t) b] / std::max (1, frames) + 1.0e-12);
    return out;
}

double corr8 (std::array<double, 8> a, std::array<double, 8> b)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < 8; ++i) { ma += a[(size_t) i]; mb += b[(size_t) i]; }
    ma /= 8; mb /= 8;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < 8; ++i) { const double x = a[(size_t) i] - ma, y = b[(size_t) i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
    return sab / std::sqrt (saa * sbb + 1.0e-12);
}

double rmsOf (const std::vector<float>& x, double sr, double t0, double t1)
{
    const size_t a = (size_t) std::max (0.0, t0 * sr), b = std::min (x.size(), (size_t) (t1 * sr));
    double e = 0; for (size_t i = a; i < b; ++i) e += (double) x[i] * x[i];
    return std::sqrt (e / std::max<size_t> (1, b - a));
}

bool allFinite (const std::vector<float>& x, float limit)
{
    for (float v : x) if (! std::isfinite (v) || std::abs (v) > limit) return false;
    return true;
}

// run the talk box over a mono voice in 512-sample blocks; events (sample, note, on) are applied between blocks' sub-parts
struct TbEvent { size_t at; int note; bool on; };
std::vector<float> runTalk (aio::TalkBox& tb, const std::vector<float>& voice, const aio::TalkSettings& s, const std::vector<TbEvent>& ev = {})
{
    std::vector<float> out (voice.size());
    size_t pos = 0, e = 0;
    while (pos < voice.size())
    {
        size_t end = std::min (voice.size(), pos + 256);
        if (e < ev.size() && ev[e].at < end) end = std::max (pos + 1, ev[e].at);
        while (e < ev.size() && ev[e].at <= pos) { if (ev[e].on) tb.noteOn (ev[e].note, 100); else tb.noteOff (ev[e].note); ++e; }
        std::vector<float> l (voice.begin() + (long) pos, voice.begin() + (long) end), r = l;
        tb.process (l.data(), r.data(), (int) l.size(), s);
        for (size_t i = 0; i < l.size(); ++i) out[pos + i] = l[i];
        pos = end;
    }
    return out;
}
} // namespace

static void testTalkBox()
{
    std::printf ("\n-- talk box --\n");
    using namespace aio;

    // ---- the LPC toolbox
    {
        // white noise through a known all-pole filter: Levinson finds the coefficients, the lattice reproduces the filter
        std::vector<double> x (40000);
        uint32_t rng = 777u; double y1 = 0, y2 = 0;
        auto rnd = [&] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return (double) (rng & 0xFFFFFF) / 8388608.0 - 1.0; };
        for (auto& v : x) { const double y = rnd() + 1.6 * y1 - 0.9 * y2; y2 = y1; y1 = y; v = y; }
        double R[3] {}; for (size_t i = 2; i < x.size(); ++i) for (int m = 0; m <= 2; ++m) R[m] += x[i] * x[i - (size_t) m];
        double k[3] {}, a[3] {}, tmp[3] {};
        levinsonDurbin (R, 2, k, a, tmp);
        CHECK (std::abs (a[1] + 1.6) < 0.03 && std::abs (a[2] - 0.9) < 0.03, "Levinson recovers a known 2-pole filter (a1 %.3f, a2 %.3f)", a[1], a[2]);

        AllPoleLattice lat; lat.setOrder (2);
        double d1 = 0, d2 = 0, worst = 0;
        for (int i = 0; i < 400; ++i)
        {
            const double in = i == 0 ? 1.0 : (i == 150 ? -0.5 : 0.0);
            const double yl = lat.tick (in, k);
            const double yd = in - a[1] * d1 - a[2] * d2; d2 = d1; d1 = yd;
            worst = std::max (worst, std::abs (yl - yd));
        }
        CHECK (worst < 1.0e-9, "the lattice filter equals the direct-form filter (worst difference %.2e)", worst);
    }

    const double sr = 44100.0;
    const auto voice = vowelVoice (sr, 120.0, { 0, 1, 2, 3, 4 }, 0.7);
    TalkSettings base; base.on = true; base.source = 1; base.glideMs = 0.f; base.vibDepth = 0.f; base.sub = 0.f; base.drive = 0.f; base.toneHz = 16000.f; base.dynamics = 100.f;

    // ---- pitch: the synth note decides it, not the voice
    {
        TalkBox tb; tb.prepare (sr);
        const auto out = runTalk (tb, voice, base, { { 0, 57, true } });
        CHECK (allFinite (out, 4.f), "output is finite and bounded");
        const double m = noteMedianMidi (out, sr, 0.5, 3.4);
        CHECK (std::abs (m - 57.0) < 0.15, "the output is on the held note A3 (%.3f), not the voice (B2 at 120 Hz)", m);
        CHECK (rmsOf (out, sr, 0.3, 3.4) > 0.02, "and it is audible (rms %.3f)", rmsOf (out, sr, 0.3, 3.4));
    }

    // ---- the mouth: the vowel shapes the spectrum
    {
        TalkBox tb; tb.prepare (sr);
        const auto out = runTalk (tb, voice, base, { { 0, 57, true } });
        double minOwn = 1, maxCross = -1;
        std::vector<std::array<double, 8>> vb, ob;
        for (int v = 0; v < 5; ++v)
        {
            const double t0 = v * 0.7 + 0.25, t1 = v * 0.7 + 0.68;
            vb.push_back (bandDb (voice, sr, t0, t1)); ob.push_back (bandDb (out, sr, t0, t1));
        }
        for (int a = 0; a < 5; ++a)
            for (int b = 0; b < 5; ++b)
            {
                const double c = corr8 (ob[(size_t) a], vb[(size_t) b]);
                if (a == b) minOwn = std::min (minOwn, c); else maxCross = std::max (maxCross, c);
            }
        CHECK (minOwn > 0.8, "each vowel's spectrum follows the voice's (lowest match %.2f)", minOwn);
        int right = 0;
        for (int a = 0; a < 5; ++a)
        {
            int best = 0; double bc = -2;
            for (int b = 0; b < 5; ++b) { const double c = corr8 (ob[(size_t) a], vb[(size_t) b]); if (c > bc) { bc = c; best = b; } }
            juce::ignoreUnused (best);
            right += corr8 (ob[(size_t) a], vb[(size_t) a]) >= bc - 0.03;
        }
        CHECK (right == 5, "every vowel in the output matches the same vowel in the voice about as well as any other (%d / 5)", right);
        juce::ignoreUnused (maxCross);

        // "mouth size" moves the formants: the 'a' spectrum gets its energy at higher frequencies
        TalkBox tb2; tb2.prepare (sr);
        auto s2 = base; s2.shiftSt = 4.f;
        const auto up = runTalk (tb2, voice, s2, { { 0, 57, true } });
        auto centroid = [&] (const std::array<double, 8>& b) { double n = 0, d = 0; for (int i = 0; i < 8; ++i) { const double p = std::pow (10.0, b[(size_t) i] / 10.0); n += p * (i + 0.5) * 600.0; d += p; } return n / d; };
        const double c0 = centroid (bandDb (out, sr, 0.25, 0.68)), c1 = centroid (bandDb (up, sr, 0.25, 0.68));
        CHECK (c1 > c0 * 1.1, "mouth size +4 st raises the spectral centroid (%.0f -> %.0f Hz)", c0, c1);
    }

    // ---- silence in, or no key: silence out
    {
        TalkBox tb; tb.prepare (sr);
        std::vector<float> sil ((size_t) (sr * 1.0), 0.f);
        auto o1 = runTalk (tb, sil, base, { { 0, 57, true } });
        CHECK (rmsOf (o1, sr, 0.2, 1.0) < 1.0e-4, "no voice -> no sound (rms %.2e)", rmsOf (o1, sr, 0.2, 1.0));

        TalkBox tb2; tb2.prepare (sr);
        auto o2 = runTalk (tb2, voice, base, { { 0, 57, true }, { (size_t) (sr * 1.0), 57, false } });
        CHECK (rmsOf (o2, sr, 0.5, 1.0) > 0.02 && rmsOf (o2, sr, 1.4, 3.0) < 1.0e-3, "key released -> the talk box falls silent (%.4f -> %.5f)", rmsOf (o2, sr, 0.5, 1.0), rmsOf (o2, sr, 1.4, 3.0));

        TalkBox tb3; tb3.prepare (sr);
        const auto o3 = runTalk (tb3, voice, base, {});
        CHECK (rmsOf (o3, sr, 0.3, 3.0) < 1.0e-4, "voice but no key -> silent");
    }

    // ---- glide between notes
    {
        TalkBox tb; tb.prepare (sr);
        auto s = base; s.glideMs = 120.f;
        const auto v = vowelVoice (sr, 120.0, { 0, 0, 0 }, 0.9);
        const auto out = runTalk (tb, v, s, { { 0, 57, true }, { (size_t) (sr * 0.9), 64, true } });
        const auto pt = analysePitch (out.data(), out.size(), sr);
        auto at = [&] (double t) { return (double) pt.midi[(size_t) std::lround (t * sr / pt.hop)]; };
        const double before = at (0.8), mid = at (0.99), after = at (2.4);
        CHECK (std::abs (before - 57.0) < 0.15 && std::abs (after - 64.0) < 0.15, "legato notes: A3 then E4 (%.2f, %.2f)", before, after);
        CHECK (mid > 57.3 && mid < 63.7, "the pitch slides between them (%.2f a moment after the change)", mid);
    }

    // ---- notes from the voice, snapped to the scale
    {
        const double f0 = 440.0 * std::pow (2.0, (62.4 - 69.0) / 12.0);       // D4 + 40 cents
        const auto v = vowelVoice (sr, f0, { 0, 3, 0, 3 }, 0.7);
        auto s = base; s.source = 0; s.snap = 100.f; s.mask = 0x0AB5;          // C major: C D E F G A B
        TalkBox tb; tb.prepare (sr);
        const auto out = runTalk (tb, v, s);
        const double m = noteMedianMidi (out, sr, 1.2, 2.7);
        CHECK (std::abs (m - 62.0) < 0.2, "voice at D4 + 40 c is snapped onto D4 in C major (%.2f)", m);
        auto s0 = s; s0.snap = 0.f;
        TalkBox tb2; tb2.prepare (sr);
        const double m0 = noteMedianMidi (runTalk (tb2, v, s0), sr, 1.2, 2.7);
        CHECK (std::abs (m0 - 62.4) < 0.25, "with snap off it follows the voice exactly (%.2f)", m0);
        auto s1 = s; s1.octave = 1;
        TalkBox tb3; tb3.prepare (sr);
        const double m1 = noteMedianMidi (runTalk (tb3, v, s1), sr, 1.2, 2.7);
        CHECK (std::abs (m1 - 74.0) < 0.3, "octave +1 (%.2f)", m1);
    }

    // ---- consonants: noise in the voice becomes noise in the output
    {
        std::vector<float> v ((size_t) (sr * 1.2), 0.f);
        uint32_t rng = 99u; double lp1 = 0, lp2 = 0;
        for (size_t i = (size_t) (sr * 0.3); i < (size_t) (sr * 0.9); ++i)
        {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const double nz = (double) (rng & 0xFFFFFF) / 8388608.0 - 1.0;
            lp1 += 0.45 * (nz - lp1); lp2 += 0.45 * (lp1 - lp2);
            v[i] = (float) ((nz - lp2) * 0.25);                                     // high-passed noise, an "s"
        }
        auto hfRatio = [&] (const std::vector<float>& x)
        {
            const auto b = bandDb (x, sr, 0.4, 0.85);
            double hf = 0, all = 0;
            for (int i = 0; i < 8; ++i) { const double p = std::pow (10.0, b[(size_t) i] / 10.0); all += p; if (i >= 5) hf += p; }
            return hf / all;
        };
        auto s = base; s.consonants = 100.f;
        TalkBox tb; tb.prepare (sr);
        const auto on = runTalk (tb, v, s, { { 0, 57, true } });
        auto s2 = base; s2.consonants = 0.f;
        TalkBox tb2; tb2.prepare (sr);
        const auto off = runTalk (tb2, v, s2, { { 0, 57, true } });
        juce::ignoreUnused (hfRatio);
        auto voicedShare = [&] (const std::vector<float>& x)
        {
            const auto pt = analysePitch (x.data(), x.size(), sr);
            int v = 0, n = 0;
            for (int i = 0; i < pt.numFrames(); ++i)
            {
                const double t = (double) i * pt.hop / sr;
                if (t < 0.45 || t > 0.85) continue;
                ++n; v += pt.midi[(size_t) i] > 0.f;
            }
            return n > 0 ? (double) v / n : 0.0;
        };
        juce::ignoreUnused (voicedShare);
        auto periodicity = [&] (const std::vector<float>& x)
        {
            const size_t a = (size_t) (sr * 0.45), b = (size_t) (sr * 0.85);
            double best = 0;
            for (int lag = (int) (sr / 220.0) - 3; lag <= (int) (sr / 220.0) + 3; ++lag)
            {
                double sxy = 0, sxx = 0, syy = 0;
                for (size_t i = a; i < b; ++i) { sxy += (double) x[i] * x[i + (size_t) lag]; sxx += (double) x[i] * x[i]; syy += (double) x[i + (size_t) lag] * x[i + (size_t) lag]; }
                best = std::max (best, sxy / std::sqrt (sxx * syy + 1.0e-18));
            }
            return best;
        };
        const double vOn = periodicity (on), vOff = periodicity (off);
        CHECK (rmsOf (on, sr, 0.4, 0.85) > 0.01, "an 's' comes through (rms %.3f)", rmsOf (on, sr, 0.4, 0.85));
        CHECK (vOn < 0.4 && vOff > 0.7, "with consonants on the 's' is noise, with them off it stays a buzzy note (periodicity %.2f vs %.2f)", vOn, vOff);
    }

    // ---- loudness: follows the voice, or stays even
    {
        std::vector<float> loud = vowelVoice (sr, 120.0, { 0 }, 1.0, 0.20), soft = vowelVoice (sr, 120.0, { 0 }, 1.0, 0.02);
        std::vector<float> v; v.insert (v.end(), loud.begin(), loud.end()); v.insert (v.end(), soft.begin(), soft.end());
        auto run = [&] (float dyn)
        {
            TalkBox tb; tb.prepare (sr);
            auto s = base; s.dynamics = dyn; s.gateDb = -70.f;
            const auto o = runTalk (tb, v, s, { { 0, 57, true } });
            return 20.0 * std::log10 (rmsOf (o, sr, 0.5, 0.95) / rmsOf (o, sr, 1.5, 1.95));
        };
        const double d100 = run (100.f), d0 = run (0.f);
        CHECK (d100 > 15.0 && d100 < 24.0, "follow loudness 100%%: a 20 dB softer voice is about 20 dB softer (%.1f dB)", d100);
        CHECK (std::abs (d0) < 4.0, "follow loudness 0%%: even level (%.1f dB apart)", d0);
    }

    // ---- robustness: sample rates, extreme settings, hard input
    {
        for (double rate : { 44100.0, 48000.0, 96000.0 })
        {
            const auto v = vowelVoice (rate, 130.0, { 0, 1, 2 }, 0.5, 0.3);
            for (int variant = 0; variant < 4; ++variant)
            {
                TalkBox tb; tb.prepare (rate);
                auto s = base;
                s.wave = variant; s.shiftSt = variant % 2 ? 6.f : -6.f; s.sharp = variant < 2 ? 0.f : 100.f; s.drive = 100.f; s.presence = variant == 3 ? 100.f : 0.f;
                s.consonants = 100.f; s.glideMs = 300.f; s.vibDepth = 100.f; s.mix = 60.f;
                const auto o = runTalk (tb, v, s, { { 0, 40, true }, { (size_t) (rate * 0.5), 96, true } });
                if (! allFinite (o, 8.f)) { CHECK (false, "stable at %.0f Hz, extreme setting %d", rate, variant); }
            }
            CHECK (true, "stable at %.0f Hz with extreme settings (shift, sharpness, drive, wave)", rate);
        }
        TalkBox tb; tb.prepare (sr);
        std::vector<float> hard ((size_t) (sr * 1.0));
        uint32_t rng = 5u;
        for (auto& v : hard) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; v = ((rng & 0xFFFF) < 0x8000 ? 1.f : -1.f); }
        const auto o = runTalk (tb, hard, base, { { 0, 60, true } });
        CHECK (allFinite (o, 8.f), "full-scale noise in: no blow-up");
    }

    // ---- cost
    {
        TalkBox tb; tb.prepare (48000.0);
        const auto v = vowelVoice (48000.0, 120.0, { 0, 1, 2, 3, 4 }, 2.0);
        auto s = base; s.wave = 2;
        const auto t0 = std::chrono::steady_clock::now();
        runTalk (tb, v, s, { { 0, 57, true } });
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        const double load = ms / 10000.0 * 100.0;
        std::printf ("      talk box cost: %.0f ms for 10 s of audio (%.1f %% of one core)\n", ms, load);
        CHECK (load < 15.0, "cheap enough to run live (%.1f %% of a core)", load);
    }
}

static void testTalkProcessor()
{
    std::printf ("\n-- talk box in the plug-in --\n");
    const double sr = 44100.0;
    const auto voice = vowelVoice (sr, 120.0, { 0, 3, 1, 4 }, 1.0);

    auto render = [&] (AIOProcessor& p, const std::vector<float>& in, int sampleOfMidi, const juce::MidiMessage* msg, std::vector<float>& outMono)
    {
        outMono.assign (in.size(), 0.f);
        const int B = 512;
        for (size_t pos = 0; pos + (size_t) B <= in.size(); pos += B)
        {
            juce::AudioBuffer<float> b (2, B);
            for (int i = 0; i < B; ++i) { b.setSample (0, i, in[pos + (size_t) i]); b.setSample (1, i, in[pos + (size_t) i]); }
            juce::MidiBuffer m;
            if (msg != nullptr && (int) pos <= sampleOfMidi && sampleOfMidi < (int) pos + B) m.addEvent (*msg, sampleOfMidi - (int) pos);
            p.processBlock (b, m);
            for (int i = 0; i < B; ++i) outMono[pos + (size_t) i] = 0.5f * (b.getSample (0, i) + b.getSample (1, i));
        }
    };

    // fixed note
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        p.setParam ("tbOn", 1.f); p.setParam ("tbSub", 0.f); p.setParam ("tbSource", 3.f); p.setParam ("tbFixed", 57.f); p.setParam ("tbGlide", 0.f); p.setParam ("tbVibDepth", 0.f);
        std::vector<float> out; render (p, voice, 0, nullptr, out);
        CHECK (std::abs (noteMedianMidi (out, sr, 0.6, 3.8) - 57.0) < 0.2, "plug-in: fixed note A3 (%.2f)", noteMedianMidi (out, sr, 0.6, 3.8));
        CHECK (rmsOf (out, sr, 0.6, 3.8) > 0.02 && allFinite (out, 2.f), "plug-in: audible and bounded");
        CHECK (p.getTalk().carrierMidi.load() > 50.f, "the carrier note is shown to the editor (%.1f)", p.getTalk().carrierMidi.load());
    }

    // off: the voice passes straight through
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        std::vector<float> out; render (p, voice, 0, nullptr, out);
        double best = 1e9;
        for (int lag = 0; lag < 400; ++lag)                      // the output limiter's look-ahead delays it a little
        {
            double d = 0; for (size_t i = 8192; i < out.size() - 400; ++i) d = std::max (d, (double) std::abs (out[i + (size_t) lag] - voice[i]));
            best = std::min (best, d);
        }
        CHECK (best < 0.02, "talk box off: the voice is untouched (worst difference %.4f)", best);
    }

    // MIDI keys
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        p.setParam ("tbOn", 1.f); p.setParam ("tbSub", 0.f); p.setParam ("tbSource", 1.f); p.setParam ("tbGlide", 0.f); p.setParam ("tbVibDepth", 0.f);
        const auto on = juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100);
        std::vector<float> out; render (p, voice, 20000, &on, out);
        CHECK (rmsOf (out, sr, 0.1, 0.4) < 1.0e-3, "MIDI mode: silent until a key is pressed (%.5f)", rmsOf (out, sr, 0.1, 0.4));
        CHECK (std::abs (noteMedianMidi (out, sr, 1.0, 3.8) - 64.0) < 0.2, "MIDI note E4 arrives (%.2f)", noteMedianMidi (out, sr, 1.0, 3.8));
        const auto off = juce::MidiMessage::noteOff (1, 64);
        std::vector<float> out2; render (p, voice, 8000, &off, out2);
        CHECK (rmsOf (out2, sr, 0.6, 3.8) < 1.0e-3, "note off silences it");
    }

    // on-screen key
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        p.setParam ("tbOn", 1.f); p.setParam ("tbSub", 0.f); p.setParam ("tbSource", 1.f); p.setParam ("tbGlide", 0.f); p.setParam ("tbVibDepth", 0.f);
        p.setTalkKey (60);
        std::vector<float> out; render (p, voice, 0, nullptr, out);
        CHECK (std::abs (noteMedianMidi (out, sr, 0.6, 3.8) - 60.0) < 0.2, "on-screen key C4 (%.2f)", noteMedianMidi (out, sr, 0.6, 3.8));
        p.setTalkKey (-1);
        std::vector<float> out2; render (p, voice, 0, nullptr, out2);
        CHECK (rmsOf (out2, sr, 0.6, 3.8) < 1.0e-3, "releasing the on-screen key silences it");
    }

    // Compose melody as the note source
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        p.setParam ("tbOn", 1.f); p.setParam ("tbSub", 0.f); p.setParam ("tbSource", 2.f); p.setParam ("tbPart", 0.f); p.setParam ("tbGlide", 0.f); p.setParam ("tbVibDepth", 0.f);
        p.regeneratePattern();
        p.previewPlaying.store (true);
        std::vector<float> out; render (p, voice, 0, nullptr, out);
        p.previewPlaying.store (false);
        const double r = rmsOf (out, sr, 0.5, 3.8);
        CHECK (r > 0.01, "Compose melody drives the talk box (rms %.3f)", r);
        CHECK (allFinite (out, 2.f), "...and stays bounded");
    }

    // presets + state
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        p.applyTalkPreset (1);
        CHECK (p.apvts.getRawParameterValue ("tbOn")->load() > 0.5f && std::abs (p.apvts.getRawParameterValue ("tbGlide")->load() - 140.f) < 0.5f, "a talk box preset switches it on and sets the knobs");
        juce::MemoryBlock mb; p.getStateInformation (mb);
        AIOProcessor q; q.setPlayConfigDetails (2, 2, sr, 512); q.prepareToPlay (sr, 512);
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (q.apvts.getRawParameterValue ("tbOn")->load() > 0.5f && std::abs (q.apvts.getRawParameterValue ("tbGlide")->load() - 140.f) < 0.5f, "talk box settings are saved with the project");
        CHECK (AIOProcessor::talkPresetCount() >= 6, "%d talk box presets", AIOProcessor::talkPresetCount());
        // a factory preset does not touch the talk box
        aio::applyPreset (p.apvts, 2);
        CHECK (p.apvts.getRawParameterValue ("tbOn")->load() > 0.5f, "loading a factory preset leaves the talk box alone");
    }
}

// =================================================================================================================
#include "../Source/ui/TalkUI.h"

static void prepareTalkSnapshot (AIOProcessor& proc)
{
    const double sr = 44100.0;
    proc.setPlayConfigDetails (2, 2, sr, 512); proc.prepareToPlay (sr, 512);
    proc.applyTalkPreset (0);
    proc.setTalkKey (64);
    const auto voice = vowelVoice (sr, 120.0, { 1, 0 }, 0.6);
    for (size_t pos = 0; pos + 512 <= voice.size() - 4096; pos += 512)
    {
        juce::AudioBuffer<float> b (2, 512);
        for (int i = 0; i < 512; ++i) { b.setSample (0, i, voice[pos + (size_t) i]); b.setSample (1, i, voice[pos + (size_t) i]); }
        juce::MidiBuffer m;
        proc.processBlock (b, m);
    }
}

static void testTalkUI()
{
    std::printf ("\n-- talk box keys (synthetic mouse) --\n");
    const double sr = 44100.0;
    AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
    p.setParam ("tbOn", 1.f); p.setParam ("tbSource", 3.f); p.setParam ("tbGlide", 0.f); p.setParam ("tbVibDepth", 0.f); p.setParam ("tbSub", 0.f);
    aio::ui::TalkKeys keys (p);
    keys.setBounds (0, 0, 840, 150);
    const auto voice = vowelVoice (sr, 120.0, { 0, 3 }, 1.0);
    auto run = [&] (size_t from, size_t blocks)
    {
        for (size_t b = 0; b < blocks; ++b)
        {
            juce::AudioBuffer<float> buf (2, 512);
            for (int i = 0; i < 512; ++i) { const float v = voice[(from + b * 512 + (size_t) i) % voice.size()]; buf.setSample (0, i, v); buf.setSample (1, i, v); }
            juce::MidiBuffer m; p.processBlock (buf, m);
        }
    };
    keys.mouseDown (mouseEv (keys, keys.pointFor (60)));
    CHECK ((int) p.apvts.getRawParameterValue ("tbSource")->load() == 1, "pressing a key switches the notes to 'MIDI / keys'");
    CHECK (keys.heldNote() == 60, "C4 is held");
    run (0, 30);
    CHECK (std::abs (p.getTalk().carrierMidi.load() - 60.f) < 0.01f, "the synth plays C4 (%.2f)", p.getTalk().carrierMidi.load());
    keys.mouseDrag (mouseEv (keys, keys.pointFor (64), 0, true, keys.pointFor (60)));
    run (30 * 512, 30);
    CHECK (std::abs (p.getTalk().carrierMidi.load() - 64.f) < 0.01f, "dragging to another key slides the lead there (%.2f)", p.getTalk().carrierMidi.load());
    keys.mouseUp (mouseEv (keys, keys.pointFor (64), 0, true, keys.pointFor (60)));
    run (60 * 512, 60);
    CHECK (p.getTalk().carrierMidi.load() < 0.f && ! p.getTalk().active.load(), "letting go stops it");

    p.setParam ("tbLatch", 1.f);
    keys.mouseDown (mouseEv (keys, keys.pointFor (67))); keys.mouseUp (mouseEv (keys, keys.pointFor (67)));
    run (0, 30);
    CHECK (std::abs (p.getTalk().carrierMidi.load() - 67.f) < 0.01f, "latch keeps the note after the mouse is released (%.2f)", p.getTalk().carrierMidi.load());
    keys.mouseDown (mouseEv (keys, keys.pointFor (67))); keys.mouseUp (mouseEv (keys, keys.pointFor (67)));
    run (30 * 512, 60);
    CHECK (! p.getTalk().active.load(), "a second click on the same key un-latches it");
}
