// Offline test runner for the DSP modules, the chord / melody generator and the processor as a whole.
#include "PluginProcessor.h"
#include "compose/Composer.h"
#include "PluginEditor.h"
#include <cstdio>
#include <random>
#include <functional>
#include <chrono>
#include <complex>

using namespace aio;

static int failures = 0, checks = 0;
#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        ++checks;                                                           \
        if (! (cond)) { ++failures; std::printf ("FAIL  "); std::printf (__VA_ARGS__); std::printf ("   [line %d]\n", __LINE__); } \
        else          { std::printf ("ok    "); std::printf (__VA_ARGS__); std::printf ("\n"); } \
    } while (0)

static double rmsDb (const std::vector<float>& x, size_t a, size_t b)
{
    double e = 0; for (size_t i = a; i < b; ++i) e += (double) x[i] * x[i];
    return 10.0 * std::log10 (e / (double) (b - a) + 1e-20);
}

// independent f0 estimator (normalised autocorrelation + parabolic interpolation)
static double estimateF0 (const std::vector<float>& x, size_t start, size_t len, double sr, double fmin, double fmax)
{
    const int lagMin = (int) (sr / fmax), lagMax = (int) (sr / fmin);
    double best = -1; int bl = lagMin;
    std::vector<double> r ((size_t) lagMax + 2, 0.0);
    for (int lag = lagMin - 1; lag <= lagMax + 1; ++lag)
    {
        double s = 0, e1 = 0, e2 = 0;
        for (size_t i = 0; i + (size_t) lag < len; ++i)
        {
            const double a = x[start + i], b = x[start + i + (size_t) lag];
            s += a * b; e1 += a * a; e2 += b * b;
        }
        r[(size_t) lag] = s / std::sqrt (e1 * e2 + 1e-12);
    }
    for (int lag = lagMin; lag <= lagMax; ++lag) if (r[(size_t) lag] > best) { best = r[(size_t) lag]; bl = lag; }
    // prefer the shortest lag with nearly the best correlation (avoid sub-harmonic picks)
    for (int lag = lagMin; lag < bl; ++lag)
        if (r[(size_t) lag] > best - 0.03 && r[(size_t) lag] >= r[(size_t) lag - 1] && r[(size_t) lag] >= r[(size_t) lag + 1]) { bl = lag; break; }
    const double y0 = r[(size_t) bl - 1], y1 = r[(size_t) bl], y2 = r[(size_t) bl + 1];
    const double den = y0 - 2 * y1 + y2;
    const double off = std::abs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;
    return sr / ((double) bl + off);
}

static double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

static std::vector<float> voice (double sr, double seconds, const std::function<double (double)>& f0At)
{
    std::vector<float> x ((size_t) (sr * seconds));
    double ph = 0;
    for (size_t n = 0; n < x.size(); ++n)
    {
        const double t = (double) n / sr, f = f0At (t);
        ph += 2.0 * kPi * f / sr;
        double v = 0;
        for (int k = 1; k <= 10; ++k) v += std::sin (k * ph) / k;
        x[n] = (float) (0.25 * v);
    }
    return x;
}

// -----------------------------------------------------------------------------------------------------------------
static void testTheory()
{
    std::printf ("\n== theory / progressions ==\n");
    auto names = [] (int key, int scale, int prog, int kind)
    {
        ComposeSettings s; s.key = key; s.scale = scale; s.progression = prog; s.chordKind = kind; s.bars = 1; s.chordLen = 1;
        s.melodyOn = false; s.bassOn = false;
        auto p = generatePattern (s);
        std::string out;
        for (auto& c : p.chords) out += c.name + " ";
        return out;
    };
    CHECK (names (0, 0, 0, Triad) == "C G Am F ", "C major I-V-vi-IV triads -> %s", names (0, 0, 0, Triad).c_str());
    CHECK (names (0, 0, 0, Seventh) == "Cmaj7 G7 Am7 Fmaj7 ", "C major sevenths -> %s", names (0, 0, 0, Seventh).c_str());
    CHECK (names (0, 0, 3, Seventh) == "Dm7 G7 Cmaj7 Am7 ", "ii-V-I-vi sevenths -> %s", names (0, 0, 3, Seventh).c_str());
    CHECK (names (9, 1, 4, Triad) == "Am F C G ", "A minor i-VI-III-VII -> %s", names (9, 1, 4, Triad).c_str());
    CHECK (names (0, 0, 0, Sus4) == "Csus4 Gsus4 Asus4 Fsus4 ", "sus4 names -> %s", names (0, 0, 0, Sus4).c_str());
    CHECK (names (0, 0, 0, Add9) == "Cadd9 Gadd9 Amadd9 Fadd9 ", "add9 names -> %s", names (0, 0, 0, Add9).c_str());
    CHECK (names (9, 1, 5, Triad) == "Am G F E ", "Andalusian (harmonic V forced major) -> %s", names (9, 1, 5, Triad).c_str());
    CHECK (names (0, 0, 0, Ninth) == "Cmaj9 G9 Am9 Fmaj9 ", "ninth names -> %s", names (0, 0, 0, Ninth).c_str());
}

static void testComposer()
{
    std::printf ("\n== composer ==\n");
    int bad = 0, total = 0, strongBad = 0, strongTotal = 0, rangeBad = 0;
    for (int key = 0; key < 12; ++key)
        for (int scale = 0; scale < 12; ++scale)
            for (int style = 0; style < 4; ++style)
                for (int seed = 1; seed <= 3; ++seed)
                {
                    ComposeSettings s;
                    s.key = key; s.scale = scale; s.melodyStyle = style; s.melodySeed = (uint32_t) seed; s.chordSeed = (uint32_t) seed;
                    s.progression = seed == 3 ? numProgressionChoices() - 1 : seed - 1;
                    s.bars = seed; s.humanize = 0.f; s.swing = 0.f; s.chordRhythm = seed * 3; s.chordKind = Mixed;
                    const auto p = generatePattern (s);
                    const auto& sd = scaleDefs()[(size_t) scale];
                    for (auto& n : p.notes)
                    {
                        if (n.start < -1e-9 || n.len <= 0 || n.start + n.len > p.lengthBeats + 1e-6 || n.pitch < 20 || n.pitch > 110) ++rangeBad;
                        if (n.part != PartMelody) continue;
                        ++total;
                        const int rel = ((n.pitch - key) % 12 + 12) % 12;
                        bool inScale = false;
                        for (int i = 0; i < sd.n; ++i) inScale |= (sd.iv[i] == rel);
                        if (! inScale) ++bad;
                        if (style != 3 && (std::fmod (n.start, 2.0) < 1e-6))
                        {
                            ++strongTotal;
                            const ChordInfo* ch = nullptr;
                            for (auto& c : p.chords) if (n.start >= c.start - 1e-6 && n.start < c.start + c.len - 1e-6) ch = &c;
                            if (ch == nullptr || std::find (ch->pcs.begin(), ch->pcs.end(), n.pitch % 12) == ch->pcs.end())
                            {
                                if (++strongBad <= 6) { std::printf ("      strong-beat miss: key %d scale %d style %d seed %d t=%.2f pitch %d chord %s\n", key, scale, style, seed, n.start, n.pitch, ch ? ch->name.c_str() : "?"); }
                            }
                        }
                    }
                }
    CHECK (rangeBad == 0, "all notes inside the pattern and in a sane pitch range (%d violations)", rangeBad);
    CHECK (bad == 0, "all %d melody notes belong to the selected scale (%d outside)", total, bad);
    CHECK (strongBad == 0, "melody notes on strong beats are chord tones (%d of %d are not)", strongBad, strongTotal);

    ComposeSettings a; a.melodySeed = 5; a.chordSeed = 9; a.progression = numProgressionChoices() - 2;
    auto p1 = generatePattern (a), p2 = generatePattern (a);
    bool same = p1.notes.size() == p2.notes.size();
    for (size_t i = 0; same && i < p1.notes.size(); ++i)
        same = p1.notes[i].pitch == p2.notes[i].pitch && p1.notes[i].start == p2.notes[i].start;
    CHECK (same, "same seed -> identical pattern (%zu notes)", p1.notes.size());
    auto b = a; b.melodySeed = 6;
    auto p3 = generatePattern (b);
    bool differ = p3.notes.size() != p1.notes.size();
    for (size_t i = 0; ! differ && i < p1.notes.size(); ++i) differ = p1.notes[i].pitch != p3.notes[i].pitch || p1.notes[i].start != p3.notes[i].start;
    CHECK (differ, "different melody seed -> different melody");
    auto c = a; c.chordSeed = 10;
    auto p4 = generatePattern (c);
    std::string n1, n4;
    for (auto& ch : p1.chords) n1 += ch.name + " ";
    for (auto& ch : p4.chords) n4 += ch.name + " ";
    CHECK (n1 != n4, "different chord seed -> different random progression (%s vs %s)", n1.c_str(), n4.c_str());

    // voice leading: smooth should move less than root-position-everywhere
    auto movement = [] (bool smooth)
    {
        ComposeSettings s; s.progression = 0; s.chordRhythm = 0; s.smoothVoicing = smooth; s.melodyOn = false; s.bassOn = false; s.humanize = 0;
        auto p = generatePattern (s);
        std::vector<std::vector<int>> by (p.chords.size());
        for (auto& n : p.notes) for (size_t i = 0; i < p.chords.size(); ++i) if (n.start >= p.chords[i].start - 1e-6 && n.start < p.chords[i].start + p.chords[i].len) by[i].push_back (n.pitch);
        double tot = 0; for (size_t i = 1; i < by.size(); ++i) { double m0 = 0, m1 = 0; for (int x : by[i - 1]) m0 += x; for (int x : by[i]) m1 += x; tot += std::abs (m1 / by[i].size() - m0 / by[i - 1].size()); }
        return tot;
    };
    CHECK (movement (true) <= movement (false), "smooth voice-leading moves the chords no more than fixed voicing (%.1f vs %.1f semitones)", movement (true), movement (false));
}

static void testEQ()
{
    std::printf ("\n== EQ ==\n");
    const double sr = 48000;
    auto run = [&] (double f, const std::array<EQBand, 6>& bands)
    {
        ParametricEQ eq; eq.prepare (sr); eq.setBands (bands);
        std::vector<float> l ((size_t) sr), r;
        for (size_t n = 0; n < l.size(); ++n) l[n] = (float) (0.3 * std::sin (2 * kPi * f * (double) n / sr));
        r = l; eq.process (l.data(), r.data(), (int) l.size());
        return rmsDb (l, 24000, 48000) - rmsDb ([&] { std::vector<float> d (l.size()); for (size_t n = 0; n < d.size(); ++n) d[n] = (float) (0.3 * std::sin (2 * kPi * f * (double) n / sr)); return d; }(), 24000, 48000);
    };
    std::array<EQBand, 6> b {};
    b[2] = { true, FilterType::Bell, 1000.f, 6.f, 1.f };
    CHECK (std::abs (run (1000, b) - 6.0) < 0.2, "bell +6 dB @1k measures %.2f dB", run (1000, b));
    CHECK (std::abs (run (100, b)) < 0.3, "bell leaves 100 Hz alone (%.2f dB)", run (100, b));
    std::array<EQBand, 6> hp {};
    hp[0] = { true, FilterType::HighPass, 1000.f, 0.f, 0.707f };
    CHECK (run (100, hp) < -30.0, "high-pass @1k removes 100 Hz (%.1f dB)", run (100, hp));
    CHECK (std::abs (run (8000, hp)) < 0.3, "high-pass passes 8 kHz (%.2f dB)", run (8000, hp));
    const double resp = ParametricEQ::responseDb (b, sr, 1000.0);
    CHECK (std::abs (resp - 6.0) < 0.05, "UI curve response at the band centre = %.2f dB", resp);
}

static void testCompressor()
{
    std::printf ("\n== compressor ==\n");
    const double sr = 48000;
    Compressor c; c.prepare (sr);
    CompSettings s; s.threshDb = -18; s.ratio = 4; s.kneeDb = 0; s.attackMs = 1; s.releaseMs = 50; s.makeupDb = 0; s.mix = 1;
    std::vector<float> l ((size_t) sr * 2), r;
    for (size_t n = 0; n < l.size(); ++n) l[n] = (float) (0.5 * std::sin (2 * kPi * 1000.0 * (double) n / sr));
    r = l;
    c.process (l.data(), r.data(), (int) l.size(), s);
    const double out = rmsDb (l, 48000, 96000);
    const double in = 20 * std::log10 (0.5 / std::sqrt (2.0));
    // peak detector sees -6 dBFS, 12 dB over -> 9 dB of reduction
    CHECK (std::abs ((out - in) + 9.0) < 0.6, "-6 dBFS in, thr -18, 4:1 -> %.2f dB of reduction (expect ~9)", in - out);
    CHECK (c.grMeter.load() < -8.0f, "gain-reduction meter shows %.1f dB", c.grMeter.load());
}

static void testSatDelayReverb()
{
    std::printf ("\n== saturator / delay / reverb ==\n");
    const double sr = 44100;
    {
        Saturator s; s.prepare (sr, 512);
        std::vector<float> l (512), r;
        for (int t = 0; t < 4; ++t)
        {
            for (size_t n = 0; n < l.size(); ++n) l[n] = (float) (0.8 * std::sin (2 * kPi * 440.0 * (double) (n + (size_t) t * 512) / sr));
            r = l;
            SatSettings st; st.type = SatType::Tape; st.driveDb = 18; st.mix = 1;
            s.process (l.data(), r.data(), 512, st);
        }
        float pk = 0; bool fin = true; for (float v : l) { pk = std::max (pk, std::abs (v)); fin &= std::isfinite (v); }
        CHECK (fin && pk < 1.3f && pk > 0.3f, "tape saturation at +18 dB drive stays bounded (peak %.2f)", pk);
        for (int ty = 0; ty < 5; ++ty)
        {
            for (size_t n = 0; n < l.size(); ++n) l[n] = (float) (0.9 * std::sin (2 * kPi * 220.0 * (double) n / sr));
            r = l; SatSettings st; st.type = (SatType) ty; st.driveDb = 24;
            s.process (l.data(), r.data(), 512, st);
            bool ok = true; for (float v : l) ok &= std::isfinite (v) && std::abs (v) < 4.f;
            CHECK (ok, "saturator type %d output finite and bounded", ty);
        }
    }
    {
        StereoDelay d; d.prepare (sr);
        const int N = (int) sr;
        std::vector<float> l ((size_t) N, 0.f), r ((size_t) N, 0.f);
        l[0] = r[0] = 1.f;
        DelaySettings ds; ds.timeMs = 100; ds.feedback = 0; ds.pingPong = false; ds.mix = 1.f; ds.toneHz = 20000;
        d.process (l.data(), r.data(), N, ds);
        int peakAt = 0; float pk = 0;
        for (int i = 10; i < N; ++i) if (std::abs (l[(size_t) i]) > pk) { pk = std::abs (l[(size_t) i]); peakAt = i; }
        CHECK (std::abs (peakAt - (int) (0.1 * sr)) <= 2 && pk > 0.4f, "echo arrives at sample %d (expect %d), level %.2f", peakAt, (int) (0.1 * sr), pk);
    }
    {
        ReverbFx rv; rv.prepare (sr, 512);
        std::vector<float> l (512, 0.f), r (512, 0.f);
        l[0] = r[0] = 1.f;
        ReverbSettings rs; rs.mix = 1.f; rs.size = 0.7f;
        double e0 = 0, e1 = 0; bool fin = true;
        for (int b = 0; b < 200; ++b)
        {
            rv.process (l.data(), r.data(), 512, rs);
            for (float v : l) { fin &= std::isfinite (v); if (b < 20) e0 += (double) v * v; else if (b > 100) e1 += (double) v * v; }
            std::fill (l.begin(), l.end(), 0.f); std::fill (r.begin(), r.end(), 0.f);
        }
        CHECK (fin && e0 > 1e-6 && e1 < e0, "reverb produces a finite decaying tail (early energy %.4f > late %.6f)", e0, e1);
    }
}

static void testTuner()
{
    std::printf ("\n== pitch corrector ==\n");
    const double sr = 44100;

    auto run = [&] (const std::vector<float>& in, const TunerSettings& ts, int block)
    {
        PitchCorrector pc; pc.prepare (sr);
        std::vector<float> l = in, r = in;
        for (size_t p = 0; p < l.size(); p += (size_t) block)
            pc.process (l.data() + p, r.data() + p, (int) std::min<size_t> ((size_t) block, l.size() - p), ts);
        return l;
    };

    // 1) hard-tune a note that is 40 cents sharp of A3 onto A3 exactly
    {
        const double fIn = 220.0 * std::pow (2.0, 40.0 / 1200.0);
        auto x = voice (sr, 3.0, [&] (double) { return fIn; });
        TunerSettings ts; ts.on = true; ts.retuneMs = 0; ts.amount = 1; ts.mask = 0x0FFF; ts.minHz = 100; ts.maxHz = 800;
        auto y = run (x, ts, 512);
        const double fo = estimateF0 (y, 44100, 22050, sr, 100, 800);
        const double fi = estimateF0 (x, 44100, 22050, sr, 100, 800);
        CHECK (std::abs (cents (fi, fIn)) < 3, "test tone measures %.1f ct vs nominal (estimator sanity)", cents (fi, fIn));
        CHECK (std::abs (cents (fo, 220.0)) < 10, "40 ct sharp -> corrected output is %.1f ct from A3", cents (fo, 220.0));
        const double dr = rmsDb (y, 66150, 132300) - rmsDb (x, 66150 - 1411, 132300 - 1411);
        CHECK (std::abs (dr) < 1.5, "level preserved (%.2f dB difference)", dr);
        bool fin = true; for (float v : y) fin &= std::isfinite (v);
        CHECK (fin, "output finite");
    }
    // 2) flat note, 30 cents under D4 -> D4
    {
        const double d4 = 293.6648, fIn = d4 * std::pow (2.0, -30.0 / 1200.0);
        auto x = voice (sr, 3.0, [&] (double) { return fIn; });
        TunerSettings ts; ts.on = true; ts.retuneMs = 0; ts.mask = 0x0FFF; ts.minHz = 100; ts.maxHz = 800;
        auto y = run (x, ts, 256);
        const double fo = estimateF0 (y, 44100, 22050, sr, 100, 800);
        CHECK (std::abs (cents (fo, d4)) < 10, "30 ct flat -> output %.1f ct from D4", cents (fo, d4));
    }
    // 3) scale restriction: C major, input between D and D#, must snap to D not D#
    {
        const double fIn = 305.0;
        auto x = voice (sr, 3.0, [&] (double) { return fIn; });
        TunerSettings ts; ts.on = true; ts.retuneMs = 0; ts.minHz = 100; ts.maxHz = 800;
        const int iv[7] = { 0, 2, 4, 5, 7, 9, 11 };
        ts.mask = PitchCorrector::maskFromScale (0, iv, 7);
        auto y = run (x, ts, 512);
        const double fo = estimateF0 (y, 44100, 22050, sr, 100, 800);
        CHECK (std::abs (cents (fo, 293.6648)) < 12, "C major mask: 305 Hz -> %.1f ct from D4 (D# is not in the scale)", cents (fo, 293.6648));
    }
    // 4) vibrato: pitch deviation around the note shrinks with hard tune, stays with amount 0
    {
        auto vib = [&] (double t) { return 440.0 * std::pow (2.0, (10.0 + 35.0 * std::sin (2 * kPi * 5.5 * t)) / 1200.0); };   // +10 ct centre, +-35 ct vibrato
        auto x = voice (sr, 3.0, vib);
        auto spread = [&] (const std::vector<float>& s)
        {
            std::vector<double> c;
            for (size_t p = 44100; p + 2205 < s.size(); p += 1102)
                c.push_back (cents (estimateF0 (s, p, 2205, sr, 150, 900), 440.0));
            double m = 0; for (double v : c) m += v; m /= (double) c.size();
            double sd = 0; for (double v : c) sd += (v - m) * (v - m);
            return std::make_pair (m, std::sqrt (sd / (double) c.size()));
        };
        TunerSettings ts; ts.on = true; ts.retuneMs = 0; ts.mask = 0x0FFF; ts.minHz = 150; ts.maxHz = 900;
        auto yHard = run (x, ts, 512);
        ts.amount = 0.f;
        auto yOff = run (x, ts, 512);
        const auto sh = spread (yHard), so = spread (yOff), si = spread (x);
        std::printf ("      vibrato: input mean %.1f sd %.1f | amount 0%%: mean %.1f sd %.1f | hard tune: mean %.1f sd %.1f\n", si.first, si.second, so.first, so.second, sh.first, sh.second);
        CHECK (std::abs (sh.first) < 20 && sh.second < si.second * 0.6, "hard tune flattens vibrato onto A4 (mean %.1f ct, spread %.1f -> %.1f ct)", sh.first, si.second, sh.second);
        CHECK (std::abs (so.first - si.first) < 12 && std::abs (so.second - si.second) < 15, "amount 0 leaves the performance untouched (mean %.1f vs %.1f ct)", so.first, si.first);
    }
    // 5) unvoiced noise + silence passes through without blowing up
    {
        std::mt19937 rng (1);
        std::uniform_real_distribution<float> d (-0.2f, 0.2f);
        std::vector<float> x ((size_t) sr * 2, 0.f);
        for (size_t n = 0; n < 44100; ++n) x[n] = d (rng);
        TunerSettings ts; ts.on = true; ts.retuneMs = 0; ts.mask = 0x0FFF;
        auto y = run (x, ts, 333);
        bool fin = true; float pk = 0; for (float v : y) { fin &= std::isfinite (v); pk = std::max (pk, std::abs (v)); }
        CHECK (fin && pk < 0.6f, "noise burst + silence stays finite and bounded (peak %.2f)", pk);
        const double dr = rmsDb (y, 8000, 40000) - rmsDb (x, 8000 - 1411, 40000 - 1411);
        CHECK (std::abs (dr) < 2.0, "noise level preserved (%.2f dB)", dr);
    }
    // 6) glide across a note boundary does not produce clicks
    {
        auto x = voice (sr, 3.0, [&] (double t) { return 220.0 * std::pow (2.0, (t * 0.6) / 12.0 * 4.0); });   // 220 -> ~ 3 semitones/s *... smooth glide
        TunerSettings ts; ts.on = true; ts.retuneMs = 15; ts.mask = 0x0FFF; ts.minHz = 100; ts.maxHz = 800;
        auto y = run (x, ts, 128);
        double maxStep = 0; for (size_t n = 8000; n + 1 < y.size(); ++n) maxStep = std::max (maxStep, (double) std::abs (y[n + 1] - y[n]));
        double maxIn = 0;  for (size_t n = 8000; n + 1 < x.size(); ++n) maxIn  = std::max (maxIn, (double) std::abs (x[n + 1] - x[n]));
        CHECK (maxStep < maxIn * 1.8 + 0.02, "no discontinuities while gliding (max sample step %.3f vs input %.3f)", maxStep, maxIn);
    }
}

static void testProcessor()
{
    std::printf ("\n== whole processor ==\n");
    AIOProcessor proc;
    proc.setPlayConfigDetails (2, 2, 44100.0, 512);
    proc.prepareToPlay (44100.0, 512);

    auto set = [&] (const char* id, float v) { auto* p = proc.apvts.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
    for (auto id : { "tuneOn", "compOn", "satOn", "choOn", "dlyOn", "revOn" }) set (id, 1);
    set ("eq2Gain", 4); set ("eq4Gain", -3); set ("satDrive", 12);

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> d (-0.3f, 0.3f);
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    bool fin = true; float pk = 0;
    double phase = 0;
    for (int b = 0; b < 300; ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            phase += 2 * kPi * 300.0 / 44100.0;
            const float v = (float) (0.3 * std::sin (phase) + 0.05 * d (rng));
            buf.setSample (0, i, v); buf.setSample (1, i, v);
        }
        proc.processBlock (buf, midi);
        for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i) { const float v = buf.getSample (c, i); fin &= std::isfinite (v); pk = std::max (pk, std::abs (v)); }
    }
    CHECK (fin && pk > 0.05f && pk <= 1.0f, "every module on: output finite, audible and limited (peak %.2f)", pk);
    CHECK (proc.getDetectedMidi() > 0.f && std::abs (proc.getDetectedMidi() - (69.f + 12.f * std::log2 (300.f / 440.f))) < 0.5f,
           "tuner display reports the detected pitch (%.2f, expect ~%.2f)", proc.getDetectedMidi(), 69.f + 12.f * std::log2 (300.f / 440.f));
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    CHECK (proc.getLatencySamples() == 1412 + 49 + 88, "latency report (%d samples) = tuner 1412 + saturator oversampler 49 + limiter look-ahead 88", proc.getLatencySamples());

    // odd block sizes, including one larger than prepared
    for (int bs : { 1, 7, 64, 333, 4096 })
    {
        juce::AudioBuffer<float> o (2, bs); o.clear();
        for (int i = 0; i < bs; ++i) { o.setSample (0, i, 0.2f * std::sin (0.05f * (float) i)); o.setSample (1, i, 0.2f * std::sin (0.05f * (float) i)); }
        proc.processBlock (o, midi);
        bool f2 = true; for (int c = 0; c < 2; ++c) for (int i = 0; i < bs; ++i) f2 &= std::isfinite (o.getSample (c, i));
        CHECK (f2, "block size %d processes cleanly", bs);
    }

    // composer preview through a clean chain
    AIOProcessor p2;
    p2.setPlayConfigDetails (2, 2, 44100.0, 512);
    p2.prepareToPlay (44100.0, 512);
    p2.previewPlaying = true;
    juce::AudioBuffer<float> sil (2, 512);
    double e = 0; bool fin2 = true;
    for (int b = 0; b < 400; ++b)
    {
        sil.clear();
        p2.processBlock (sil, midi);
        for (int i = 0; i < 512; ++i) { const float v = sil.getSample (0, i); e += (double) v * v; fin2 &= std::isfinite (v); }
    }
    CHECK (fin2 && e > 1.0, "compose preview makes sound on silent input (energy %.2f)", e);
    CHECK (p2.composeRunning.load(), "transport flag reports running");
    p2.previewPlaying = false;
    for (int b = 0; b < 200; ++b) { sil.clear(); p2.processBlock (sil, midi); }
    double tail = 0; sil.clear(); p2.processBlock (sil, midi);
    for (int i = 0; i < 512; ++i) tail += std::abs (sil.getSample (0, i));
    CHECK (tail < 1e-3, "stopping the preview releases all notes (tail %.5f)", tail);

    // MIDI export
    const auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_test.mid");
    CHECK (p2.writeMidiFile (f).wasOk() && f.getSize() > 50, "MIDI export wrote %lld bytes", (long long) f.getSize());
    juce::MidiFile mf;
    juce::FileInputStream in (f);
    CHECK (mf.readFrom (in) && mf.getNumTracks() == 3, "exported file re-reads as 3 tracks");
    int notes = 0;
    for (int t = 0; t < mf.getNumTracks(); ++t)
        for (int i = 0; i < mf.getTrack (t)->getNumEvents(); ++i) if (mf.getTrack (t)->getEventPointer (i)->message.isNoteOn()) ++notes;
    int expected = 0;
    for (auto& n : p2.getPatternForUI()->notes) { juce::ignoreUnused (n); ++expected; }
    CHECK (notes == expected && notes > 8, "MIDI file contains all %d pattern notes (%d)", expected, notes);

    // state round trip
    p2.newMelodySeed();
    juce::MemoryBlock mb;
    p2.getStateInformation (mb);
    AIOProcessor p3;
    p3.setStateInformation (mb.getData(), (int) mb.getSize());
    CHECK (p3.readComposeSettings().melodySeed == p2.readComposeSettings().melodySeed, "melody seed survives save/load");

    // presets
    bool presetsOk = true;
    for (int i = 0; i < p3.getNumPrograms(); ++i) { p3.setCurrentProgram (i); presetsOk &= p3.getProgramName (i).isNotEmpty(); }
    CHECK (presetsOk, "%d factory presets load", p3.getNumPrograms());
}


// =================================================================================================================
// Loudness, limiter, Auto Voice, Auto Master, Sing
static std::vector<float> sine (double sr, double sec, double hz, double amp, double phase0 = 0.0)
{
    std::vector<float> x ((size_t) (sr * sec));
    for (size_t n = 0; n < x.size(); ++n) x[n] = (float) (amp * std::sin (2.0 * kPi * hz * (double) n / sr + phase0));
    return x;
}

static void testLoudness()
{
    std::printf ("\n== loudness / true peak / limiter ==\n");
    {
        auto k = KWeighting::make (48000.0);
        const double eb[5] = { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
        const double sh[5] = { k.shelf.b0, k.shelf.b1, k.shelf.b2, k.shelf.a1, k.shelf.a2 };
        double d = 0; for (int i = 0; i < 5; ++i) d = std::max (d, std::abs (sh[i] - eb[i]));
        CHECK (d < 2e-4, "K-weighting shelf matches the published 48 kHz coefficients (max diff %.1e)", d);
        CHECK (std::abs (k.highpass.a1 - (-1.99004745483398)) < 1e-4 && std::abs (k.highpass.a2 - 0.99007225036621) < 1e-4, "K-weighting high-pass matches the published 48 kHz coefficients");
    }
    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        auto x = sine (sr, 12.0, 1000.0, std::pow (10.0, -20.0 / 20.0));
        LoudnessMeter lm; lm.prepare (sr); lm.push (x.data(), x.data(), (int) x.size());
        CHECK (std::abs (lm.integrated() - (-20.0)) < 0.15, "stereo 1 kHz sine at -20 dBFS reads %.2f LUFS @ %.0f Hz (expect -20.0)", lm.integrated(), sr);
    }
    {
        const double sr = 44100;
        auto a = sine (sr, 10.0, 1000.0, 0.1), z = std::vector<float> ((size_t) (sr * 10.0), 0.f);
        a.insert (a.end(), z.begin(), z.end());
        LoudnessMeter lm; lm.prepare (sr); lm.push (a.data(), a.data(), (int) a.size());
        CHECK (std::abs (lm.integrated() - (-20.0)) < 0.2, "silence is gated out of integrated loudness (%.2f LUFS)", lm.integrated());
    }
    {
        auto x = sine (48000.0, 0.5, 12000.0, 1.0, kPi / 4.0);       // fs/4 sine at 45 degrees: samples are +-0.707, true peak is 1.0
        TruePeak tp; for (float v : x) tp.tick (v);
        CHECK (std::abs (tp.samplePk() - 0.7071f) < 0.01f && tp.truePeak() > 0.96f && tp.truePeak() < 1.04f,
               "true peak finds the inter-sample peak (sample %.3f, true %.3f, expect 0.707 / 1.0)", tp.samplePk(), tp.truePeak());
    }
    {
        const double sr = 44100;
        LookaheadLimiter lim; lim.prepare (sr);
        std::mt19937 rng (3); std::normal_distribution<float> nd (0.f, 0.6f);
        const int N = 44100 * 2;
        std::vector<float> l ((size_t) N), r ((size_t) N);
        for (int i = 0; i < N; ++i) { const float burst = (i / 8000) % 2 ? 3.f : 1.f; l[(size_t) i] = nd (rng) * burst; r[(size_t) i] = nd (rng) * burst; }
        const auto inL = l;
        for (int p = 0; p < N; p += 333) lim.process (l.data() + p, r.data() + p, std::min (333, N - p), -1.f);
        float pk = 0; for (int i = 0; i < N; ++i) pk = std::max ({ pk, std::abs (l[(size_t) i]), std::abs (r[(size_t) i]) });
        CHECK (pk <= std::pow (10.f, -1.f / 20.f) + 1e-4f, "limiter never exceeds the ceiling on hot noise (peak %.4f dBFS)", 20.0 * std::log10 (pk));
        CHECK (lim.getLatency() == (int) std::lround (sr * 0.002), "limiter reports %d samples of latency", lim.getLatency());

        LookaheadLimiter l2; l2.prepare (sr);
        auto q = sine (sr, 1.0, 440.0, 0.25); auto q2 = q;
        l2.process (q.data(), q2.data(), (int) q.size(), -1.f);
        const int la = l2.getLatency();
        double err = 0; for (size_t i = 20000; i < 30000; ++i) err = std::max (err, (double) std::abs (q[i] - 0.25 * std::sin (2.0 * kPi * 440.0 * (double) (i - (size_t) la) / sr)));
        CHECK (err < 1e-3, "limiter is transparent below the ceiling (max deviation %.5f)", err);
    }
}

static void testPitchDetectorAndForce()
{
    std::printf ("\n== pitch detector / forced target ==\n");
    const double sr = 44100;
    {
        auto x = voice (sr, 1.5, [] (double) { return 220.0; });
        PitchDetector pd; pd.prepare (sr);
        int voiced = 0; double sum = 0;
        for (size_t i = 0; i < x.size(); ++i)
            if (pd.push ((double) x[i], -55.f, 70.f, 800.f) && i > 22050 && pd.voiced()) { ++voiced; sum += pd.midi(); }
        const double m = voiced ? sum / voiced : 0.0;
        CHECK (voiced > 20 && std::abs (m - (69.0 + 12.0 * std::log2 (220.0 / 440.0))) < 0.15, "detector reads A3 on a 220 Hz voice (%.2f, %d frames)", m, voiced);
        PitchDetector pn; pn.prepare (sr);
        std::mt19937 rng (1); std::normal_distribution<double> nd (0.0, 0.005);
        int v = 0; for (int i = 0; i < 44100; ++i) if (pn.push (nd (rng), -55.f, 70.f, 800.f) && pn.voiced()) ++v;
        CHECK (v == 0, "no pitch reported on quiet noise (%d frames)", v);
    }
    {
        const double fIn = 233.08;                      // A#3
        auto x = voice (sr, 3.0, [&] (double) { return fIn; });
        PitchCorrector pc; pc.prepare (sr);
        TunerSettings ts; ts.on = true; ts.retuneMs = 0; ts.mask = 0; ts.forcePc = 0; ts.minHz = 100; ts.maxHz = 800;
        std::vector<float> l = x, r = x;
        for (size_t p = 0; p < l.size(); p += 128) pc.process (l.data() + p, r.data() + p, (int) std::min<size_t> (128, l.size() - p), ts);
        const double fo = estimateF0 (l, 66150, 22050, sr, 100, 800);
        CHECK (std::abs (cents (fo, 261.626)) < 25, "forced pitch class C pulls A#3 up to C4 (%.1f ct off)", cents (fo, 261.626));
    }
}

static void testAutoVoice()
{
    std::printf ("\n== auto voice ==\n");
    const double sr = 44100;
    VoxSettings vs; vs.on = true; vs.mode = VoxVocal; vs.targetDb = -20.f;

    auto run = [&] (std::vector<float> x, const VoxSettings& s)
    {
        AutoVoice av; av.prepare (sr);
        std::vector<float> r = x;
        for (size_t p = 0; p < x.size(); p += 256) av.process (x.data() + p, r.data() + p, (int) std::min<size_t> (256, x.size() - p), s);
        return x;
    };
    {
        auto v = voice (sr, 18.0, [] (double t) { return 200.0 + 20.0 * std::sin (t * 5.0); });
        const size_t seg = (size_t) (sr * 6.0);
        const float g[3] = { 0.15f, 1.0f, 0.15f };
        for (size_t i = 0; i < v.size(); ++i) v[i] *= g[std::min<size_t> (i / seg, 2)];
        auto y = run (v, vs);
        double inLv[3], outLv[3];
        for (int s = 0; s < 3; ++s) { const size_t a = seg * (size_t) s + seg - (size_t) sr * 2, b = seg * (size_t) (s + 1); inLv[s] = rmsDb (v, a, b); outLv[s] = rmsDb (y, a, b); }
        const double inSpread = std::max (inLv[0], inLv[1]) - std::min (inLv[0], inLv[1]);
        const double outSpread = std::max (outLv[0], outLv[1]) - std::min (outLv[0], outLv[1]);
        CHECK (outSpread < inSpread * 0.6, "leveler narrows a %.1f dB level gap to %.1f dB (quiet %.1f / loud %.1f dB out)", inSpread, outSpread, outLv[0], outLv[1]);
        CHECK (std::abs (outLv[0] - outLv[2]) < 3.0, "quiet sections before and after the loud one match (%.1f vs %.1f dB)", outLv[0], outLv[2]);
        bool fin = true; for (float f : y) fin &= std::isfinite (f);
        CHECK (fin, "output finite");
    }
    {
        // a pause with room noise must not be pumped up
        auto v = voice (sr, 4.0, [] (double) { return 180.0; });
        std::vector<float> x ((size_t) (sr * 10.0), 0.f);
        std::mt19937 rng (5); std::normal_distribution<float> nd (0.f, 0.002f);
        for (size_t i = 0; i < x.size(); ++i) x[i] = nd (rng) + (i < v.size() ? v[i] : 0.f);
        VoxSettings noTone = vs; noTone.tone = 0.f; noTone.comp = 0.f;     // the EQ is allowed to colour noise; the leveler must not lift it
        auto y = run (x, noTone);
        const double inN = rmsDb (x, (size_t) (sr * 8), (size_t) (sr * 10)), outN = rmsDb (y, (size_t) (sr * 8), (size_t) (sr * 10));
        CHECK (outN <= inN + 0.5, "room noise in a long pause is not raised (%.1f -> %.1f dB)", inN, outN);
    }
    {
        // sibilance: vowel + a loud 5-9 kHz burst. With the de-esser up the burst must come down relative to the vowel.
        const size_t N = (size_t) (sr * 8.0);
        auto base = voice (sr, 8.0, [] (double) { return 190.0; });
        std::mt19937 rng (9); std::normal_distribution<float> nd (0.f, 1.f);
        Biquad hp; hp.c = Coeffs::make (FilterType::BandPass, sr, 6800.0, 0.0, 0.8);
        std::vector<float> x = base;
        for (size_t i = 0; i < N; ++i) { const float ns = hp.tick (nd (rng)); if ((i / (size_t) (sr * 0.5)) % 4 == 3) x[i] += 0.6f * ns; }
        VoxSettings off = vs; off.deess = 0.f; off.comp = 0.f; off.tone = 0.f; off.clean = 0.f; off.level = 0.f;
        VoxSettings on = off; on.deess = 1.f;
        auto a = run (x, off), b = run (x, on);
        auto hf = [&] (const std::vector<float>& s)
        {
            Biquad f; f.c = Coeffs::make (FilterType::BandPass, sr, 6800.0, 0.0, 1.4);
            double e = 0, n = 0; for (size_t i = (size_t) (sr * 3.5); i < (size_t) (sr * 4.0); ++i) { const double v = f.tick (s[i]); e += v * v; ++n; }
            return 10.0 * std::log10 (e / n + 1e-20);
        };
        CHECK (hf (b) < hf (a) - 3.0, "de-esser reduces the sibilant burst by %.1f dB", hf (a) - hf (b));
        const double vowelA = rmsDb (a, (size_t) (sr * 1.0), (size_t) (sr * 1.5)), vowelB = rmsDb (b, (size_t) (sr * 1.0), (size_t) (sr * 1.5));
        CHECK (std::abs (vowelA - vowelB) < 1.0, "vowels are left alone by the de-esser (%.2f dB change)", vowelB - vowelA);
    }
    {
        std::array<float, AutoVoice::kBands> m {}, g {};
        const auto& mp = AutoVoice::modeParams (VoxVocal);
        for (int k = 0; k < AutoVoice::kBands; ++k) m[(size_t) k] = mp.shape[(size_t) k] - 40.f;     // exactly on target (any overall level)
        AutoVoice::computeCorrection (m, VoxVocal, 1.f, g);
        float mx = 0; for (float v : g) mx = std::max (mx, std::abs (v));
        CHECK (mx < 0.01f, "a voice already on target gets no EQ (max %.3f dB)", mx);
        m[5] += 8.f; AutoVoice::computeCorrection (m, VoxVocal, 1.f, g);
        CHECK (g[5] < -2.f && g[5] >= -mp.maxEqGain - 0.01f, "a +8 dB bump at band 5 is pulled down by %.1f dB (limit %.1f)", -g[5], mp.maxEqGain);
        AutoVoice::computeCorrection (m, VoxVocal, 0.f, g);
        CHECK (std::abs (g[5]) < 0.01f, "tone amount 0 does nothing");
    }
}

// stereo noise whose third-octave spectrum is exactly the Balanced target curve plus a known tonal defect
// (+4 dB bump around 250 Hz, -5 dB shelf above 5 kHz), built by random-phase overlap-add synthesis
static void makeShapedMix (std::vector<float>& l, std::vector<float>& r, double sr, double sec, double levelDb, bool withDefect = true)
{
    const int order = 15, N = 1 << order;
    const size_t total = (size_t) (sr * sec);
    juce::dsp::FFT fft (order);
    std::vector<std::complex<float>> spec ((size_t) N), td ((size_t) N);
    auto shapeDb = [&] (double f)
    {
        f = std::max (f, 20.0);
        double d = (double) targetCurveDb ((float) f, 0);
        if (withDefect) d += 4.0 * std::exp (-0.5 * std::pow (std::log2 (f / 250.0) / 0.5, 2.0)) - 5.0 / (1.0 + std::pow (5000.0 / f, 4.0));
        return d;
    };
    for (int ch = 0; ch < 2; ++ch)
    {
        std::mt19937 rng ((unsigned) (100 + ch));
        std::uniform_real_distribution<float> ud (0.f, 2.f * (float) kPi);
        auto& out = ch == 0 ? l : r;
        out.assign (total + (size_t) N, 0.f);
        for (size_t start = 0; start < total; start += (size_t) N / 2)
        {
            std::fill (spec.begin(), spec.end(), std::complex<float> (0.f, 0.f));
            for (int k = 1; k < N / 2; ++k)
            {
                const double f = (double) k * sr / N;
                const float mag = (float) std::sqrt (std::pow (10.0, shapeDb (f) / 10.0) / f);
                const float ph = ud (rng);
                spec[(size_t) k] = std::polar (mag, ph);
                spec[(size_t) (N - k)] = std::conj (spec[(size_t) k]);
            }
            fft.perform (spec.data(), td.data(), true);
            for (int i = 0; i < N; ++i)
            {
                const float w = (float) std::sin (kPi * ((double) i + 0.5) / N);       // sqrt of a Hann window
                out[start + (size_t) i] += td[(size_t) i].real() * w;
            }
        }
        out.resize (total);
    }
    double e = 0; for (size_t i = 0; i < total; ++i) e += (double) l[i] * l[i] + (double) r[i] * r[i];
    const float g = (float) (std::pow (10.0, levelDb / 20.0) / std::sqrt (e / (2.0 * (double) total)));
    for (size_t i = 0; i < total; ++i) { l[i] *= g; r[i] *= g; }
}

static double tonalError (const MasterAnalysis& a, int style)
{
    double ref = 0, nref = 0;
    for (int j = 0; j < kMB; ++j)
        if (a.bandValid[(size_t) j] && bandCentreHz (j) >= 100.f && bandCentreHz (j) <= 8000.f) { ref += a.bandDb[(size_t) j] - targetCurveDb (bandCentreHz (j), style); nref += 1; }
    ref /= std::max (1.0, nref);
    double e = 0, n = 0;
    for (int j = 0; j < kMB; ++j)
        if (a.bandValid[(size_t) j] && bandCentreHz (j) >= 40.f && bandCentreHz (j) <= 16000.f) { const double d = a.bandDb[(size_t) j] - targetCurveDb (bandCentreHz (j), style) - ref; e += d * d; n += 1; }
    return std::sqrt (e / std::max (1.0, n));
}

static void testAutoMaster()
{
    std::printf ("\n== auto master ==\n");
    const double sr = 44100;
    std::vector<float> l, r;
    makeShapedMix (l, r, sr, 20.0, -26.0);
    const auto before = analyseBuffer (l.data(), r.data(), l.size(), sr);
    CHECK (before.valid && before.lufs < -22.0 && before.lufs > -34.0, "analysis of the test mix: %.1f LUFS, true peak %.1f dB, %zu s", before.lufs, before.truePeakDb, (size_t) before.seconds);

    for (int style : { 0, 4 })
    {
        MasterOptions o; o.style = style; o.targetLufs = style == 4 ? -9.f : -14.f; o.ceilingDb = -1.f;
        auto s0 = deriveMaster (before, o);
        auto t0 = std::chrono::steady_clock::now();
        RenderStats st;
        auto s1 = refineMaster (l.data(), r.data(), l.size(), sr, before, s0, o, &st, {});
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        CHECK (std::abs (st.lufs - o.targetLufs) < 0.5, "%s: lands on %.1f LUFS target within 0.5 (got %.2f)", styleName (style), (double) o.targetLufs, st.lufs);
        CHECK (st.truePeakDb <= o.ceilingDb + 0.3, "%s: true peak %.2f dBTP stays under the %.1f dB ceiling (+0.3 tolerance)", styleName (style), st.truePeakDb, (double) o.ceilingDb);

        std::vector<float> ol, orr;
        const auto cp = chainFromSettings (s1);
        auto rs = renderPass (l.data(), r.data(), l.size(), sr, cp, [&] (const float* a, const float* b, int n) { ol.insert (ol.end(), a, a + n); orr.insert (orr.end(), b, b + n); }, {});
        CHECK (ol.size() == l.size(), "%s: rendered length equals input (%zu vs %zu), latency compensated", styleName (style), ol.size(), l.size());
        const auto after = analyseBuffer (ol.data(), orr.data(), ol.size(), sr);
        const double eB = tonalError (before, style), eA = tonalError (after, style);
        CHECK (eA < eB * 0.85, "%s: tonal error vs target curve drops from %.2f to %.2f dB rms (%.1f s for refine)", styleName (style), eB, eA, secs);
        CHECK (std::abs (after.lufs - rs.lufs) < 0.2, "%s: independent analysis agrees with the render stats (%.2f vs %.2f LUFS)", styleName (style), after.lufs, rs.lufs);
        bool fin = true; for (float v : ol) fin &= std::isfinite (v);
        CHECK (fin, "%s: finite output", styleName (style));
    }
    {
        // already-mastered material must not be wrecked: master the mastered file again, it should barely move tonally
        MasterOptions o; o.targetLufs = -14.f;
        auto s1 = refineMaster (l.data(), r.data(), l.size(), sr, before, deriveMaster (before, o), o, nullptr, {});
        std::vector<float> ol, orr;
        renderPass (l.data(), r.data(), l.size(), sr, chainFromSettings (s1), [&] (const float* a, const float* b, int n) { ol.insert (ol.end(), a, a + n); orr.insert (orr.end(), b, b + n); }, {});
        const auto a2 = analyseBuffer (ol.data(), orr.data(), ol.size(), sr);
        auto s2 = refineMaster (ol.data(), orr.data(), ol.size(), sr, a2, deriveMaster (a2, o), o, nullptr, {});
        float mx = 0, mx1 = 0;
        for (auto& b : s2.eq) if (b.on) mx = std::max (mx, std::abs (b.gainDb));
        for (auto& b : s1.eq) if (b.on) mx1 = std::max (mx1, std::abs (b.gainDb));
        CHECK (mx < mx1 * 0.7f + 0.2f, "mastering a mastered file again needs smaller EQ moves (%.1f dB vs %.1f dB the first time)", mx, mx1);
    }
}



static void testSing()
{
    std::printf ("\n== sing scoring ==\n");
    auto tr = std::make_shared<SingTrack>();
    tr->lengthBeats = 8.0;
    tr->notes = { { 0.0, 4.0, 60 }, { 4.0, 4.0, 64 } };
    SingConfig cfg; cfg.toleranceSemis = 1.0f; cfg.octaveAgnostic = true;

    auto sing = [&] (const std::function<float (double)>& pitchAt, double fromBeat, double toBeat, ScoreKeeper& sk)
    {
        for (double b = fromBeat; b < toBeat; b += 0.01) sk.addFrame (b, pitchAt (b), cfg);
    };
    {
        ScoreKeeper sk; sk.setTrack (tr);
        sing ([] (double b) { return b < 4.0 ? 60.f : 64.f; }, 0.0, 7.99, sk);
        CHECK (sk.score() > 9500.0, "perfect singing scores %.0f / 10000", sk.score());
        CHECK (sk.fraction (0) > 0.95 && sk.fraction (1) > 0.9, "both notes fully hit (%.2f, %.2f)", sk.fraction (0), sk.fraction (1));
        sing ([] (double b) { return b < 4.0 ? 60.f : 64.f; }, 8.0, 8.5, sk);
        CHECK (sk.passCount() == 1 && sk.lastScore() > 9500.0 && sk.takePassFinished(), "wrapping the loop closes the pass and records %.0f", sk.lastScore());
    }
    {
        ScoreKeeper sk; sk.setTrack (tr);
        sing ([] (double b) { return b < 4.0 ? 63.f : 67.f; }, 0.0, 7.99, sk);
        CHECK (sk.score() < 500.0, "3 semitones sharp scores %.0f", sk.score());
    }
    {
        ScoreKeeper sk; sk.setTrack (tr);
        sing ([] (double b) { return b < 4.0 ? 72.f : 52.f; }, 0.0, 7.99, sk);
        CHECK (sk.score() > 9000.0, "singing an octave up / down still counts in any-octave mode (%.0f)", sk.score());
        ScoreKeeper sk2; sk2.setTrack (tr); cfg.octaveAgnostic = false;
        sing ([] (double b) { return b < 4.0 ? 72.f : 52.f; }, 0.0, 7.99, sk2);
        cfg.octaveAgnostic = true;
        CHECK (sk2.score() < 500.0, "but not in strict-octave mode (%.0f)", sk2.score());
    }
    {
        ScoreKeeper sk; sk.setTrack (tr);
        sing ([] (double b) { return b < 0.1 ? 50.f : (b < 4.0 ? 60.f : 64.f); }, 0.0, 7.99, sk);
        CHECK (sk.score() > 8500.0, "missing the first 0.1 beat of a note is forgiven (%.0f)", sk.score());
    }
    {
        ScoreKeeper sk; sk.setTrack (tr);
        sing ([] (double) { return -1.f; }, 0.0, 16.0, sk);
        CHECK (sk.score() < 1.0, "silence scores zero (%.0f)", sk.score());
        CHECK (std::string (ScoreKeeper::rankName (0)) != std::string (ScoreKeeper::rankName (10000)) && ScoreKeeper::stars (10000) == 5, "ranks and stars span the range");
    }
    {
        auto s = serializeTrack (*tr);
        auto back = deserializeTrack (s);
        CHECK (back.notes.size() == 2 && back.notes[1].pitch == 64 && std::abs (back.lengthBeats - 8.0) < 1e-9, "track survives serialisation (\"%s\")", s.c_str());
        CHECK (deserializeTrack ("garbage").empty(), "garbage deserialises to an empty track, no crash");
    }
    {
        AIOProcessor proc;
        auto pat = proc.getPatternForUI();
        auto t = trackFromPattern (*pat);
        bool sorted = ! t.empty();
        for (size_t i = 1; i < t.notes.size(); ++i) sorted &= t.notes[i].start >= t.notes[i - 1].start + t.notes[i - 1].len - 1e-6;
        CHECK (sorted, "melody from the composer becomes a sorted, non-overlapping sing track (%zu notes)", t.notes.size());

        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_sing_test.mid");
        proc.writeMidiFile (f);
        auto err = proc.importSingMidi (f);
        auto imp = proc.getSingTrackForUI();
        CHECK (err.isEmpty() && imp && imp->notes.size() > 4, "MIDI import of the exported file works (%zu notes) %s", imp ? imp->notes.size() : 0, err.toRawUTF8());
        f.deleteFile();
        auto noFile = proc.importSingMidi (juce::File ("/nonexistent/x.mid"));
        CHECK (noFile.isNotEmpty(), "missing MIDI file reports an error");
    }
}

static bool waitFor (const std::function<bool()>& cond, int maxMs)
{
    for (int t = 0; t < maxMs; t += 20) { if (cond()) return true; juce::Thread::sleep (20); }
    return cond();
}

static void testMasterSession()
{
    std::printf ("\n== master session (file in -> file out) ==\n");
    const double sr = 44100;
    std::vector<float> l, r;
    makeShapedMix (l, r, sr, 12.0, -27.0);
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_master_test");
    dir.createDirectory();
    auto in = dir.getChildFile ("in.wav"), out = dir.getChildFile ("out.wav");
    in.deleteFile(); out.deleteFile();
    {
        juce::WavAudioFormat wav;
        juce::AudioBuffer<float> b (2, (int) l.size());
        b.copyFrom (0, 0, l.data(), (int) l.size()); b.copyFrom (1, 0, r.data(), (int) r.size());
        std::unique_ptr<juce::FileOutputStream> os (in.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
        if (w) { os.release(); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
    }

    AIOProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, 512);
    proc.prepareToPlay (sr, 512);
    auto& m = proc.getMaster();
    m.loadFile (in);
    CHECK (waitFor ([&] { return ! m.busy() && m.info().hasFile; }, 20000), "WAV loads (%.1f s, %.0f Hz)", m.info().seconds, m.info().sampleRate);
    CHECK (m.info().before.valid && m.info().waveform.size() > 100, "analysis + waveform available after load");

    auto set = [&] (const char* id, float v) { proc.setParam (id, v); };
    set ("mastTarget", -14.f); set ("limCeil", -1.f); set ("mastStyle", 0.f);
    proc.analyseAndMaster();
    MasterSettings ms; RenderStats rs; bool got = false;
    CHECK (waitFor ([&] { if (m.busy()) return false; got = m.takeResult (ms, rs); return got; }, 120000), "auto master finishes (%.2f LUFS predicted)", rs.lufs);
    if (got) proc.applyMasterSettings (ms);
    CHECK (std::abs (rs.lufs - (-14.0)) < 0.5 && rs.truePeakDb <= -0.7, "predicted result: %.2f LUFS, %.2f dBTP", rs.lufs, rs.truePeakDb);

    proc.exportMaster (out, 24);
    juce::String msg; bool exported = false;
    waitFor ([&] { if (m.busy()) return false; exported = m.takeExportDone (msg); return exported; }, 120000);
    CHECK (exported && out.existsAsFile(), "export wrote %s (%s)", out.getFileName().toRawUTF8(), msg.toRawUTF8());

    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
        CHECK (rd != nullptr && rd->numChannels == 2 && rd->bitsPerSample == 24 && (size_t) rd->lengthInSamples >= l.size() - 8 && (size_t) rd->lengthInSamples <= l.size() + 8,
               "exported file is 24-bit stereo and the same length (%lld samples)", rd ? (long long) rd->lengthInSamples : 0LL);
        if (rd)
        {
            juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
            rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
            const auto a = analyseBuffer (b.getReadPointer (0), b.getReadPointer (1), (size_t) b.getNumSamples(), sr);
            CHECK (std::abs (a.lufs - (-14.0)) < 0.6, "re-measuring the exported file gives %.2f LUFS", a.lufs);
            CHECK (a.truePeakDb <= -0.6, "exported true peak is %.2f dBTP (ceiling -1.0)", a.truePeakDb);
        }
    }

    // audition through the plug-in, mastered and A/B original
    for (int ab = 0; ab < 2; ++ab)
    {
        m.abOriginal = ab == 1;
        m.matchGainLin = 1.f;
        m.seek (0.0);
        m.setPlaying (true);
        CHECK (m.isPlaying(), "file playback starts");
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
        double e = 0; bool fin = true; float pk = 0;
        for (int b = 0; b < 200; ++b)
        {
            buf.clear();
            proc.processBlock (buf, midi);
            for (int i = 0; i < 512; ++i) for (int c = 0; c < 2; ++c) { const float v = buf.getSample (c, i); fin &= std::isfinite (v); e += (double) v * v; pk = std::max (pk, std::abs (v)); }
        }
        CHECK (fin && e > 1.0 && pk <= 1.0f, "%s playback is audible, finite and in range (peak %.2f)", ab ? "original" : "mastered", pk);
        m.setPlaying (false);
    }
    // the chain is reported to the host
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    CHECK (proc.getLatencySamples() >= 88, "host is told about the limiter look-ahead (%d samples)", proc.getLatencySamples());

    // bad inputs
    m.loadFile (dir.getChildFile ("nothing.wav"));
    waitFor ([&] { return ! m.busy(); }, 10000);
    CHECK (m.state() == MasterSession::Failed || ! m.info().hasFile, "loading a missing file fails cleanly (state %d, %s)", (int) m.state(), m.message().toRawUTF8());
    dir.deleteRecursively();
}

static void testVoxAndSingProcessor()
{
    std::printf ("\n== processor: voice + sing ==\n");
    const double sr = 44100;
    AIOProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, 512);
    proc.prepareToPlay (sr, 512);
    proc.setParam ("voxOn", 1.f);
    auto x = voice (sr, 4.0, [] (double) { return 220.0; });
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer midi;
    bool fin = true; double e = 0;
    for (size_t p = 0; p + 512 <= x.size(); p += 512)
    {
        for (int i = 0; i < 512; ++i) { buf.setSample (0, i, x[p + (size_t) i]); buf.setSample (1, i, x[p + (size_t) i]); }
        proc.processBlock (buf, midi);
        for (int i = 0; i < 512; ++i) { fin &= std::isfinite (buf.getSample (0, i)); e += (double) buf.getSample (0, i) * buf.getSample (0, i); }
    }
    CHECK (fin && e > 1.0, "Auto Voice on: audible, finite output");

    // Sing: the guide plays and pitch frames flow when the internal clock runs
    proc.setParam ("voxOn", 0.f);
    proc.setParam ("singOn", 1.f);
    proc.setParam ("singGuide", 1.f);
    proc.singPlaying = true;
    int frames = 0; double ge = 0;
    AIOProcessor::PitchFrame pf[256];
    for (int b = 0; b < 300; ++b)
    {
        buf.clear();
        for (int i = 0; i < 512; ++i) { const float v = x[(size_t) (b * 512 + i) % x.size()]; buf.setSample (0, i, v); buf.setSample (1, i, v); }
        proc.processBlock (buf, midi);
        frames += proc.popPitchFrames (pf, 256);
        for (int i = 0; i < 512; ++i) ge += (double) buf.getSample (0, i) * buf.getSample (0, i);
    }
    CHECK (proc.singRunning.load() && proc.singBeat.load() > 1.0, "Sing clock runs from the plug-in (beat %.2f)", proc.singBeat.load());
    CHECK (frames > 100, "pitch frames reach the UI queue (%d)", frames);
    proc.singPlaying = false;
    proc.setParam ("singAssist", 1.f);
    proc.singPlaying = true;
    bool fin2 = true;
    for (int b = 0; b < 200; ++b)
    {
        for (int i = 0; i < 512; ++i) { const float v = x[(size_t) (b * 512 + i) % x.size()]; buf.setSample (0, i, v); buf.setSample (1, i, v); }
        proc.processBlock (buf, midi);
        for (int i = 0; i < 512; ++i) fin2 &= std::isfinite (buf.getSample (0, i));
    }
    CHECK (fin2, "Sing assist (pitch correction toward the melody) stays finite");
    proc.singPlaying = false;
    juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
    CHECK (proc.getLatencySamples() >= 1412, "assist reports the pitch-corrector latency (%d samples)", proc.getLatencySamples());
}


static void benchMaster()
{
    const double sr = 44100;
    std::vector<float> l, r;
    makeShapedMix (l, r, sr, 300.0, -24.0);
    auto t0 = std::chrono::steady_clock::now();
    const auto a = analyseBuffer (l.data(), r.data(), l.size(), sr);
    auto t1 = std::chrono::steady_clock::now();
    MasterOptions o;
    RenderStats st;
    refineMaster (l.data(), r.data(), l.size(), sr, a, deriveMaster (a, o), o, &st, {});
    auto t2 = std::chrono::steady_clock::now();
    std::printf ("5-minute stereo file: analyse %.1f s, auto-master solve %.1f s (result %.2f LUFS, %.2f dBTP)\n",
                 std::chrono::duration<double> (t1 - t0).count(), std::chrono::duration<double> (t2 - t1).count(), st.lufs, st.truePeakDb);
}

static void snapshots (const juce::File& dir)
{
    const double sr = 44100;
    AIOProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, 512);
    proc.prepareToPlay (sr, 512);
    auto set = [&] (const char* id, float v) { proc.setParam (id, v); };
    set ("tuneOn", 1); set ("compOn", 1); set ("satOn", 1); set ("dlyOn", 1); set ("revOn", 1); set ("choOn", 1);
    set ("eq2Gain", 4); set ("eq3Freq", 300); set ("eq3Gain", -3); set ("eq5Gain", 5); set ("eq6Gain", 2.5f); set ("eq1On", 1); set ("eq1Freq", 90);
    set ("cmpOn", 1);
    proc.previewPlaying = true;

    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buf (2, 512);
    double ph = 0;
    for (int b = 0; b < 160; ++b)
    {
        for (int i = 0; i < 512; ++i)
        {
            ph += 2 * kPi * (230.0 + 8.0 * std::sin (b * 0.1)) / sr;
            double v = 0; for (int k = 1; k <= 8; ++k) v += std::sin (k * ph) / k;
            buf.setSample (0, i, (float) (0.2 * v)); buf.setSample (1, i, (float) (0.2 * v));
        }
        proc.processBlock (buf, midi);
    }
    proc.previewPlaying = false;
    set ("cmpOn", 0); set ("tuneOn", 0);

    // ---- Auto Master: a real file through the real pipeline
    {
        std::vector<float> l, r;
        makeShapedMix (l, r, sr, 25.0, -27.0);
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_snap_mix.wav");
        f.deleteFile();
        juce::WavAudioFormat wav;
        juce::AudioBuffer<float> wb (2, (int) l.size());
        wb.copyFrom (0, 0, l.data(), (int) l.size()); wb.copyFrom (1, 0, r.data(), (int) r.size());
        std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
        if (w) { os.release(); w->writeFromAudioSampleBuffer (wb, 0, wb.getNumSamples()); }
        w.reset();
        proc.getMaster().loadFile (f);
        waitFor ([&] { return ! proc.getMaster().busy(); }, 30000);
        proc.analyseAndMaster();
        waitFor ([&] { return ! proc.getMaster().busy(); }, 120000);
        MasterSettings ms; RenderStats rs;
        if (proc.getMaster().takeResult (ms, rs)) proc.applyMasterSettings (ms);
        proc.getMaster().seek (0.38);
    }

    AIOEditor ed (proc);
    ed.setSize (1180, 740);
    auto pump = [&] (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); };

    // ---- Auto Voice: a voice that drifts between too quiet and too loud, with a bit of S
    set ("voxOn", 1);
    ed.showTab (6);
    {
        auto v = voice (sr, 12.0, [] (double t) { return 190.0 + 25.0 * std::sin (t * 4.0); });
        for (size_t i = 0; i < v.size(); ++i) v[i] *= (float) (0.45 + 0.45 * std::sin ((double) i / sr * 0.9));
        for (size_t p = 0; p + 512 <= v.size(); p += 512)
        {
            for (int i = 0; i < 512; ++i) { buf.setSample (0, i, v[p + (size_t) i]); buf.setSample (1, i, v[p + (size_t) i]); }
            proc.processBlock (buf, midi);
            if ((p / 512) % 16 == 0) pump (5);
        }
    }

    // ---- Sing: a singer who is mostly right
    set ("voxOn", 0); set ("singOn", 1); set ("singGuide", 1); set ("cmpMelOn", 1);
    proc.regeneratePattern();
    ed.showTab (7);
    pump (50);
    proc.singPlaying = true;
    {
        std::mt19937 rng (4);
        double phs = 0;
        for (int b = 0; b < 420; ++b)
        {
            const double beat = proc.singBeat.load();
            auto tr = proc.getSingTrackForUI();
            const int idx = tr ? tr->noteAt (beat + 0.02) : -1;
            double midiTgt = idx >= 0 ? tr->notes[(size_t) idx].pitch : -1.0;
            if (midiTgt >= 0 && (idx % 5 == 3)) midiTgt += 1.7;          // a wrong note now and then
            if (midiTgt >= 0 && (idx % 7 == 2)) midiTgt -= 12.0;         // and one an octave down (still counts)
            for (int i = 0; i < 512; ++i)
            {
                double s = 0.0;
                if (midiTgt >= 0)
                {
                    const double f0 = 440.0 * std::pow (2.0, (midiTgt - 69.0 + 0.15 * std::sin ((double) b * 0.4)) / 12.0);
                    phs += 2 * kPi * f0 / sr;
                    for (int k = 1; k <= 8; ++k) s += std::sin (k * phs) / k;
                    s *= 0.2;
                }
                buf.setSample (0, i, (float) s); buf.setSample (1, i, (float) s);
            }
            proc.processBlock (buf, midi);
            if (b % 3 == 0) pump (3);
        }
        pump (60);
    }

    const char* names[8] = { "tune", "eq", "dynamics", "space", "compose", "master", "voice", "sing" };
    for (int t = 0; t < 8; ++t)
    {
        ed.showTab (t);
        for (int k = 0; k < 8; ++k) { proc.processBlock (buf, midi); }
        pump (120);
        auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
        juce::PNGImageFormat png;
        auto f = dir.getChildFile (juce::String (names[t]) + ".png");
        f.deleteFile();
        if (auto os = f.createOutputStream()) png.writeImageToStream (img, *os);
        std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    if (argc > 1 && juce::String (argv[1]) == "--bench") { benchMaster(); return 0; }
    if (argc > 2 && juce::String (argv[1]) == "--snap") { snapshots (juce::File (argv[2])); return 0; }
    testTheory();
    testComposer();
    testEQ();
    testCompressor();
    testSatDelayReverb();
    testTuner();
    testProcessor();
    testLoudness();
    testPitchDetectorAndForce();
    testAutoVoice();
    testAutoMaster();
    testSing();
    testMasterSession();
    testVoxAndSingProcessor();
    std::printf ("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
