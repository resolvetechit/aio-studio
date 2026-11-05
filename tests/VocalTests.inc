// Included from Tests.cpp: the vocal note editor (engine, session, processor glue).
#include "vocal/VocalEngine.h"
#include "vocal/VocalSession.h"
#include <juce_dsp/juce_dsp.h>

namespace
{
struct SungNote { double t0, t1, midi, vibCents = 0.0, vibHz = 5.5; };

struct SungClip { std::vector<float> x; double sr; std::vector<SungNote> notes; };

SungClip sing (double sr, double seconds, const std::vector<SungNote>& ns, const std::vector<std::pair<double, double>>& noise = {}, uint32_t seed = 7)
{
    SungClip c;
    c.sr = sr; c.notes = ns;
    c.x.assign ((size_t) (sr * seconds), 0.f);
    std::mt19937 rng (seed);
    std::normal_distribution<float> nd (0.f, 1.f);
    double ph = 0;
    for (size_t n = 0; n < c.x.size(); ++n)
    {
        const double t = (double) n / sr;
        float v = 0.f;
        for (const auto& s : ns)
        {
            if (t < s.t0 || t >= s.t1) continue;
            const double cents = s.vibCents * std::sin (2.0 * kPi * s.vibHz * (t - s.t0));
            const double f = 440.0 * std::exp2 ((s.midi - 69.0 + cents / 100.0) / 12.0);
            ph += 2.0 * kPi * f / sr;
            double env = std::min (1.0, (t - s.t0) / 0.02) * std::min (1.0, (s.t1 - t) / 0.04);
            double sum = 0;
            for (int k = 1; k * f < 5000.0 && k <= 24; ++k)
                sum += std::sin ((double) k * ph) / k * (1.0 + 3.0 * std::exp (-std::pow ((k * f - 800.0) / 300.0, 2.0)));
            v += (float) (0.12 * env * sum);
        }
        for (const auto& nb : noise)
            if (t >= nb.first && t < nb.second) v += 0.04f * nd (rng);
        c.x[n] = v + 0.0005f * nd (rng);
    }
    return c;
}

double noteMedianMidi (const std::vector<float>& x, double sr, double t0, double t1)
{
    std::vector<double> m;
    for (double t = t0 + 0.12; t + 0.04 < t1 - 0.12; t += 0.02)
    {
        const size_t a = (size_t) (t * sr), len = (size_t) (0.04 * sr);
        if (a + len >= x.size()) break;
        const double f = estimateF0 (x, a, len, sr, 80.0, 800.0);
        m.push_back (69.0 + 12.0 * std::log2 (f / 440.0));
    }
    if (m.empty()) return -1;
    std::sort (m.begin(), m.end());
    return m[m.size() / 2];
}

double centroid (const std::vector<float>& x, double sr, double t0, double t1)
{
    constexpr int order = 13, N = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> buf (2 * N, 0.f);
    const size_t a = (size_t) (0.5 * (t0 + t1) * sr) - N / 2;
    for (int i = 0; i < N; ++i) buf[(size_t) i] = x[a + (size_t) i] * (float) (0.5 - 0.5 * std::cos (2.0 * kPi * i / N));
    fft.performFrequencyOnlyForwardTransform (buf.data());
    double sw = 0, s = 0;
    for (int i = 1; i < N / 2; ++i)
    {
        const double hz = (double) i * sr / N;
        if (hz < 200.0 || hz > 5000.0) continue;
        sw += hz * buf[(size_t) i]; s += buf[(size_t) i];
    }
    return s > 0 ? sw / s : 0.0;
}

double onsetTime (const std::vector<float>& x, double sr, double from, double to, double thresholdDb)
{
    const size_t win = (size_t) (0.005 * sr);
    for (size_t n = (size_t) (from * sr); n + win < std::min (x.size(), (size_t) (to * sr)); n += win / 2)
        if (rmsDb (x, n, n + win) > thresholdDb) return (double) n / sr;
    return -1.0;
}

std::vector<SungNote> melody3()
{
    return { { 0.30, 0.95, 60.0 - 0.30 }, { 1.15, 1.85, 64.0 + 0.40, 40.0 }, { 2.00, 2.70, 67.0 - 0.25 } };
}
} // namespace

// -----------------------------------------------------------------------------------------------------------------
static void testVocalAnalysis()
{
    std::printf ("\n-- vocal: pitch tracking + note detection --\n");
    for (double sr : { 44100.0, 48000.0 })
    {
        const auto clip = sing (sr, 3.2, melody3(), { { 1.0, 1.08 } });
        const auto t0 = std::chrono::steady_clock::now();
        const auto tr = analysePitch (clip.x.data(), clip.x.size(), sr);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        CHECK (tr.numFrames() > 500, "sr %.0f: track has %d frames (analysed 3.2 s in %.0f ms)", sr, tr.numFrames(), ms);

        for (const auto& n : clip.notes)
        {
            std::vector<double> err;
            int voiced = 0, total = 0;
            for (int i = 0; i < tr.numFrames(); ++i)
            {
                const double t = (double) i * tr.hop / sr;
                if (t < n.t0 + 0.1 || t > n.t1 - 0.1) continue;
                ++total;
                if (tr.midi[(size_t) i] >= 0.f) { ++voiced; err.push_back (std::abs (tr.midi[(size_t) i] - n.midi) * 100.0); }
            }
            std::sort (err.begin(), err.end());
            const double med = err.empty() ? 999.0 : err[err.size() / 2];
            CHECK (voiced > 0.9 * total && med < (n.vibCents > 0 ? 40.0 : 12.0),
                   "sr %.0f: note at %.2fs is %.0f%% voiced, median error %.1f cents", sr, n.t0, 100.0 * voiced / std::max (1, total), med);
        }
        int falseVoiced = 0, gapFrames = 0;
        for (int i = 0; i < tr.numFrames(); ++i)
        {
            const double t = (double) i * tr.hop / sr;
            if ((t > 0.02 && t < 0.25) || (t > 2.85 && t < 3.15)) { ++gapFrames; if (tr.midi[(size_t) i] >= 0.f) ++falseVoiced; }
        }
        CHECK (falseVoiced <= gapFrames / 20, "sr %.0f: silence is unvoiced (%d of %d frames wrongly voiced)", sr, falseVoiced, gapFrames);

        const auto notes = segmentNotes (tr);
        CHECK (notes.size() == 3, "sr %.0f: 3 notes found (%d)", sr, (int) notes.size());
        for (size_t i = 0; i < std::min<size_t> (3, notes.size()); ++i)
        {
            CHECK (std::abs (notes[i].centre - clip.notes[i].midi) < 0.2 && std::abs (notes[i].t0 - clip.notes[i].t0) < 0.06 && std::abs (notes[i].t1 - clip.notes[i].t1) < 0.08,
                   "sr %.0f: note %d centre %.2f (true %.2f), %.2f-%.2fs (true %.2f-%.2f)", sr, (int) i, notes[i].centre, clip.notes[i].midi, notes[i].t0, notes[i].t1, clip.notes[i].t0, clip.notes[i].t1);
        }
    }

    // legato steps: no silence between the notes
    {
        const auto clip = sing (44100.0, 2.4, { { 0.2, 0.75, 60.0 }, { 0.75, 1.30, 62.0 }, { 1.30, 1.9, 64.0 } });
        const auto tr = analysePitch (clip.x.data(), clip.x.size(), clip.sr);
        const auto notes = segmentNotes (tr);
        CHECK (notes.size() == 3, "legato C-D-E is split into 3 notes (%d)", (int) notes.size());
        if (notes.size() == 3)
            CHECK (std::abs (notes[1].t0 - 0.75) < 0.07 && std::abs (notes[2].t0 - 1.30) < 0.07, "legato boundaries at %.2f and %.2f s", notes[1].t0, notes[2].t0);
    }
    // same pitch repeated (la la la): split by the level dips
    {
        std::vector<SungNote> ns { { 0.2, 0.65, 62.0 }, { 0.70, 1.15, 62.0 }, { 1.20, 1.65, 62.0 } };
        auto clip = sing (44100.0, 2.0, ns);
        // fill the gaps between with a quiet continuation so that it is a dip, not silence
        auto soft = sing (44100.0, 2.0, { { 0.65, 0.70, 62.0 }, { 1.15, 1.20, 62.0 } });
        for (size_t i = 0; i < clip.x.size(); ++i) clip.x[i] += 0.12f * soft.x[i];
        const auto tr = analysePitch (clip.x.data(), clip.x.size(), clip.sr);
        const auto notes = segmentNotes (tr);
        CHECK (notes.size() == 3, "repeated pitch with level dips is split into 3 notes (%d)", (int) notes.size());
    }
    // wide vibrato stays one note
    {
        const auto clip = sing (44100.0, 2.0, { { 0.2, 1.8, 65.0, 70.0, 5.5 } });
        const auto tr = analysePitch (clip.x.data(), clip.x.size(), clip.sr);
        const auto notes = segmentNotes (tr);
        CHECK (notes.size() == 1, "a held note with +/-70 cent vibrato stays one note (%d)", (int) notes.size());
        if (notes.size() == 1)
        {
            float mn = 99, mx = -99;
            for (auto v : notes[0].curve->fast) { mn = std::min (mn, v); mx = std::max (mx, v); }
            CHECK (mx - mn > 0.6f && mx - mn < 1.8f, "vibrato is in the 'fast' curve (peak to peak %.2f semitones)", mx - mn);
        }
    }
    // silence / noise
    {
        std::vector<float> z (44100 * 2, 0.f);
        CHECK (segmentNotes (analysePitch (z.data(), z.size(), 44100.0)).empty(), "silence gives no notes");
        std::mt19937 rng (3);
        std::normal_distribution<float> nd (0.f, 0.1f);
        for (auto& v : z) v = nd (rng);
        const auto notes = segmentNotes (analysePitch (z.data(), z.size(), 44100.0));
        double dur = 0; for (auto& n : notes) dur += n.origLen();
        CHECK (dur < 0.4, "white noise gives (almost) no pitched notes (%.2f s)", dur);
    }
}

// -----------------------------------------------------------------------------------------------------------------
static std::vector<VocalNote> analyseNotes (const SungClip& clip, int* hopOut = nullptr)
{
    const auto tr = analysePitch (clip.x.data(), clip.x.size(), clip.sr);
    if (hopOut) *hopOut = tr.hop;
    return segmentNotes (tr);
}

static void testVocalRender()
{
    std::printf ("\n-- vocal: auto-correct + manual edits render --\n");
    const double sr = 44100.0;
    const auto clip = sing (sr, 3.2, melody3(), { { 1.0, 1.08 } });
    int hop = 256;
    auto notes = analyseNotes (clip, &hop);
    if (notes.size() != 3) { CHECK (false, "need 3 notes for render tests (%d)", (int) notes.size()); return; }
    const auto& truth = clip.notes;

    // identity: nothing edited
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto out = renderVocal (clip.x, sr, hop, notes);
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        double e = 0, d = 0;
        for (size_t i = 0; i < clip.x.size() && i < out.size(); ++i) { e += (double) clip.x[i] * clip.x[i]; const double df = (double) clip.x[i] - out[i]; d += df * df; }
        const double snr = 10.0 * std::log10 (e / std::max (d, 1e-20));
        CHECK (out.size() == clip.x.size(), "identity render keeps the length (%zu vs %zu), took %.0f ms", out.size(), clip.x.size(), ms);
        CHECK (snr > 25.0, "identity render is transparent (SNR %.1f dB)", snr);
    }

    // auto: chromatic snap, drift flattened, vibrato kept
    {
        AutoSettings a; a.correct = 1.f; a.flatten = 1.f; a.vibrato = 1.f; a.timing = 0.f;
        auto ed = notes;
        applyAuto (ed, a);
        CHECK (std::abs (ed[0].target - 60.0) < 0.001 && std::abs (ed[1].target - 64.0) < 0.001 && std::abs (ed[2].target - 67.0) < 0.001,
               "auto snaps to %.2f %.2f %.2f", ed[0].target, ed[1].target, ed[2].target);
        const auto out = renderVocal (clip.x, sr, hop, ed);
        for (int i = 0; i < 3; ++i)
        {
            const double before = noteMedianMidi (clip.x, sr, truth[(size_t) i].t0, truth[(size_t) i].t1);
            const double after = noteMedianMidi (out, sr, truth[(size_t) i].t0, truth[(size_t) i].t1);
            const double want = std::round (truth[(size_t) i].midi);
            CHECK (std::abs (after - want) < 0.10 && std::abs (before - want) > 0.15,
                   "note %d: %.2f -> %.2f (target %.0f), error %.1f cents", i + 1, before, after, want, (after - want) * 100.0);
        }
        // the vibrato of note 2 survives
        {
            std::vector<double> m;
            for (double t = 1.3; t < 1.7; t += 0.01)
            {
                const double f = estimateF0 (out, (size_t) (t * sr), (size_t) (0.03 * sr), sr, 80.0, 800.0);
                m.push_back (69.0 + 12.0 * std::log2 (f / 440.0));
            }
            const double sp = *std::max_element (m.begin(), m.end()) - *std::min_element (m.begin(), m.end());
            CHECK (sp > 0.3, "vibrato is still there after correcting (swing %.2f semitones)", sp);
        }
        // no vibrato when asked
        {
            auto e2 = notes;
            AutoSettings b = a; b.vibrato = 0.f;
            applyAuto (e2, b);
            const auto o2 = renderVocal (clip.x, sr, hop, e2);
            std::vector<double> m;
            for (double t = 1.3; t < 1.7; t += 0.01)
            {
                const double f = estimateF0 (o2, (size_t) (t * sr), (size_t) (0.03 * sr), sr, 80.0, 800.0);
                m.push_back (69.0 + 12.0 * std::log2 (f / 440.0));
            }
            const double sp = *std::max_element (m.begin(), m.end()) - *std::min_element (m.begin(), m.end());
            std::vector<double> m0;
            for (double t = 1.3; t < 1.7; t += 0.01)
                m0.push_back (69.0 + 12.0 * std::log2 (estimateF0 (clip.x, (size_t) (t * sr), (size_t) (0.03 * sr), sr, 80.0, 800.0) / 440.0));
            const double sp0 = *std::max_element (m0.begin(), m0.end()) - *std::min_element (m0.begin(), m0.end());
            CHECK (sp < 0.35 * sp0, "vibrato 0%% flattens note 2 (swing %.2f -> %.2f semitones)", sp0, sp);
        }
        // scale mask: C major, a note a quarter tone above C# is pulled to D
        {
            AutoSettings c = a;
            c.mask = 0;
            for (int pc : { 0, 2, 4, 5, 7, 9, 11 }) c.mask |= (uint16_t) (1u << pc);
            CHECK (nearestAllowedMidi (61.4, c.mask) == 62 && nearestAllowedMidi (61.6, c.mask) == 62 && nearestAllowedMidi (60.4, c.mask) == 60 && nearestAllowedMidi (65.4, c.mask) == 65,
                   "C major mask: 61.4 -> %d, 60.4 -> %d, 65.4 -> %d", nearestAllowedMidi (61.4, c.mask), nearestAllowedMidi (60.4, c.mask), nearestAllowedMidi (65.4, c.mask));
        }
        // partial correction
        {
            auto e3 = notes;
            AutoSettings h = a; h.correct = 0.5f;
            applyAuto (e3, h);
            CHECK (std::abs (e3[0].target - (notes[0].centre + 0.5f * (60.f - notes[0].centre))) < 0.01, "correction amount 50%% moves half way (%.2f)", e3[0].target);
        }
    }

    // manual pitch edit: note 3 up a fifth with its own formant kept
    {
        auto ed = notes;
        ed[2].target = ed[2].centre + 7.f;
        const auto out = renderVocal (clip.x, sr, hop, ed);
        const double after = noteMedianMidi (out, sr, truth[2].t0, truth[2].t1);
        CHECK (std::abs (after - (truth[2].midi + 7.0)) < 0.12, "note 3 +7 semitones: %.2f (want %.2f)", after, truth[2].midi + 7.0);
        const double c0 = centroid (clip.x, sr, 2.0, 2.7), c1 = centroid (out, sr, 2.0, 2.7);
        CHECK (c1 > c0 * 0.8 && c1 < c0 * 1.5, "formants stay put when only the pitch moves (centroid %.0f -> %.0f Hz)", c0, c1);
        bool finite = true; float pk = 0;
        for (auto v : out) { if (! std::isfinite (v)) finite = false; pk = std::max (pk, std::abs (v)); }
        CHECK (finite && pk < 1.0f, "output is finite and not clipping (peak %.2f)", pk);
        // the neighbours are untouched
        CHECK (std::abs (noteMedianMidi (out, sr, truth[0].t0, truth[0].t1) - noteMedianMidi (clip.x, sr, truth[0].t0, truth[0].t1)) < 0.08, "note 1 is not affected by editing note 3");
    }
    // formant
    {
        auto ed = notes;
        ed[0].formant = 4.f;
        const auto out = renderVocal (clip.x, sr, hop, ed);
        const double c0 = centroid (clip.x, sr, 0.3, 0.95), c1 = centroid (out, sr, 0.3, 0.95);
        const double p0 = noteMedianMidi (clip.x, sr, 0.3, 0.95), p1 = noteMedianMidi (out, sr, 0.3, 0.95);
        CHECK (c1 > c0 * 1.04 && std::abs (p1 - p0) < 0.15, "formant +4 st brightens (%.0f -> %.0f Hz) without changing the pitch (%.2f -> %.2f)", c0, c1, p0, p1);
    }
    // gain + mute
    {
        auto ed = notes;
        ed[0].gainDb = 6.f; ed[1].muted = true;
        const auto out = renderVocal (clip.x, sr, hop, ed);
        const double g = rmsDb (out, (size_t) (0.5 * sr), (size_t) (0.8 * sr)) - rmsDb (clip.x, (size_t) (0.5 * sr), (size_t) (0.8 * sr));
        CHECK (std::abs (g - 6.0) < 0.6, "gain +6 dB reads %.2f dB", g);
        const double m = rmsDb (out, (size_t) (1.3 * sr), (size_t) (1.7 * sr)) - rmsDb (clip.x, (size_t) (1.3 * sr), (size_t) (1.7 * sr));
        CHECK (m < -35.0, "muted note is %.0f dB down", m);
    }
    // timing: move note 3 later, note 2 earlier-stretched
    {
        auto ed = notes;
        const double before = onsetTime (clip.x, sr, 1.9, 3.0, rmsDb (clip.x, (size_t) (2.2 * sr), (size_t) (2.5 * sr)) - 18.0);
        moveNote (ed, 2, ed[2].nt0 + 0.30, 10.0);
        CHECK (std::abs (ed[2].nt0 - (notes[2].t0 + 0.30)) < 1e-9, "move note 3 by +0.30 s allowed (%.3f)", ed[2].nt0 - notes[2].t0);
        const auto out = renderVocal (clip.x, sr, hop, ed);
        const double after = onsetTime (out, sr, 1.9, 3.4, rmsDb (clip.x, (size_t) (2.2 * sr), (size_t) (2.5 * sr)) - 18.0);
        CHECK (before > 0 && after > 0 && std::abs ((after - before) - 0.30) < 0.03, "note 3 onset moved %.3f s (wanted 0.30)", after - before);
        CHECK (std::abs (noteMedianMidi (out, sr, ed[2].nt0, ed[2].nt1) - truth[2].midi) < 0.12, "pitch survives the move (%.2f)", noteMedianMidi (out, sr, ed[2].nt0, ed[2].nt1));
        CHECK (out.size() == clip.x.size() + (size_t) std::llround (0.30 * sr) || std::abs ((double) out.size() - (double) clip.x.size() - 0.30 * sr) < 4.0, "clip grows by the shift (%zu)", out.size());

        // neighbours: cannot overlap
        auto e2 = notes;
        moveNote (e2, 1, e2[1].nt0 + 5.0, 10.0);
        CHECK (e2[1].nt1 < e2[2].nt0 + 1e-9, "a note can't be dragged through its neighbour (ends %.3f, next starts %.3f)", e2[1].nt1, e2[2].nt0);
        moveNote (e2, 1, -5.0, 10.0);
        CHECK (e2[1].nt0 >= e2[0].nt1 - 1e-9, "...or backwards (starts %.3f, prev ends %.3f)", e2[1].nt0, e2[0].nt1);

        // stretch note 1 longer
        auto e3 = notes;
        setNoteEnd (e3, 0, e3[0].nt1 + 0.12, 10.0);
        const auto o3 = renderVocal (clip.x, sr, hop, e3);
        const double endAfter = [&] {
            for (size_t n = (size_t) (1.13 * sr); n > (size_t) (0.4 * sr); n -= (size_t) (0.005 * sr))
                if (rmsDb (o3, n - (size_t) (0.005 * sr), n) > rmsDb (clip.x, (size_t) (0.5 * sr), (size_t) (0.8 * sr)) - 12.0) return (double) n / sr;
            return -1.0;
        }();
        CHECK (std::abs (endAfter - (0.95 + 0.12)) < 0.04, "note 1 stretched to end at %.3f s (wanted %.3f)", endAfter, 0.95 + 0.12);
        CHECK (std::abs (noteMedianMidi (o3, sr, 0.3, 1.07) - truth[0].midi) < 0.1, "pitch survives the stretch (%.2f)", noteMedianMidi (o3, sr, 0.3, 1.07));
    }
    // glide: with a long glide the step between notes becomes a slide (check the transition is not instantaneous)
    {
        auto legato = sing (sr, 2.0, { { 0.2, 0.9, 60.0 }, { 0.9, 1.6, 62.0 } });
        int h2 = 256;
        auto ln = analyseNotes (legato, &h2);
        if (ln.size() == 2)
        {
            ln[1].target += 5.f;
            RenderOptions hard; hard.glideMs = 0.f;
            RenderOptions soft; soft.glideMs = 120.f;
            const auto o1 = renderVocal (legato.x, sr, h2, ln, hard), o2 = renderVocal (legato.x, sr, h2, ln, soft);
            auto frac = [&] (const std::vector<float>& o)
            {
                int mid = 0, tot = 0;
                for (double t = 0.80; t < 1.05; t += 0.01)
                {
                    const double m = 69.0 + 12.0 * std::log2 (estimateF0 (o, (size_t) (t * sr), (size_t) (0.025 * sr), sr, 80.0, 800.0) / 440.0);
                    ++tot;
                    if (m > 60.6 && m < 66.4) ++mid;
                }
                return (double) mid / tot;
            };
            CHECK (frac (o2) > frac (o1) + 0.12, "glide 120 ms spends more time between the notes (%.0f%% vs %.0f%%)", frac (o2) * 100, frac (o1) * 100);
        }
        else CHECK (false, "glide test needs 2 notes (%d)", (int) ln.size());
    }
    // timing quantise
    {
        std::vector<SungNote> off { { 0.58, 1.0, 60.0 }, { 1.62, 2.0, 62.0 } };   // 120 bpm, 1/4 grid = 0.5 s : starts 0.08 late / 0.12 late
        auto c2 = sing (sr, 2.6, off);
        int h2 = 256;
        auto n2 = analyseNotes (c2, &h2);
        if (n2.size() == 2)
        {
            AutoSettings a; a.bpm = 120.0; a.gridPerBeat = 1; a.timing = 1.f; a.correct = 0.f; a.flatten = 0.f;
            applyAuto (n2, a);
            CHECK (std::abs (n2[0].nt0 - 0.5) < 0.07 && std::abs (n2[1].nt0 - 1.5) < 0.07, "quantised starts %.3f and %.3f (grid 0.5 s)", n2[0].nt0, n2[1].nt0);
            a.timing = 0.5f;
            applyAuto (n2, a);
            CHECK (n2[0].nt0 > 0.50 && n2[0].nt0 < n2[0].t0, "50%% strength lands half way (%.3f)", n2[0].nt0);
        }
        else CHECK (false, "timing test needs 2 notes (%d)", (int) n2.size());
    }
    // level evening
    {
        auto lv = notes;
        lv[0].level = -30.f; lv[1].level = -20.f; lv[2].level = -10.f;
        AutoSettings a; a.level = 1.f;
        applyAuto (lv, a);
        CHECK (std::abs (lv[0].gainDb - 9.f) < 0.01 + 1.0 && lv[1].gainDb == 0.f && lv[2].gainDb < 0.f, "level evening: %.1f %.1f %.1f dB", lv[0].gainDb, lv[1].gainDb, lv[2].gainDb);
    }
    // scope: only selected notes change
    {
        auto sc = notes;
        AutoSettings a; a.correct = 1.f;
        std::vector<char> scope { 0, 1, 0 };
        applyAuto (sc, a, &scope);
        CHECK (sc[0].target == notes[0].centre && sc[1].target == 64.f && sc[2].target == notes[2].centre, "auto on a selection leaves the other notes alone");
    }
}

// -----------------------------------------------------------------------------------------------------------------
static void testVocalEditing()
{
    std::printf("\n-- vocal: split / merge / undo / persistence --\n");
    const double sr = 44100.0;
    const auto clip = sing (sr, 3.2, melody3());
    int hop = 256;
    auto notes = analyseNotes (clip, &hop);
    if (notes.size() != 3) { CHECK (false, "need 3 notes (%d)", (int) notes.size()); return; }

    VocalDoc d;
    d.sr = sr; d.hop = hop; d.clipSeconds = 3.2; d.notes = notes;
    d.checkpoint();
    const double mid = 0.5 * (d.notes[1].nt0 + d.notes[1].nt1);
    CHECK (splitNote (d.notes, 1, mid, sr, hop), "split note 2 at its middle");
    CHECK (d.notes.size() == 4 && std::abs (d.notes[1].t1 - d.notes[2].t0) < 1e-9 && d.notes[1].nt1 == d.notes[2].nt0, "now 4 notes, touching at %.3f", d.notes[1].t1);
    CHECK (std::abs (d.notes[1].t1 - mid) < 0.01, "split point %.3f vs asked %.3f", d.notes[1].t1, mid);
    CHECK (! splitNote (d.notes, 0, d.notes[0].nt0 + 0.005, sr, hop), "can't split at the very edge");
    CHECK (mergeWithNext (d.notes, 1) && d.notes.size() == 3, "join them again");
    CHECK (std::abs (d.notes[1].centre - notes[1].centre) < 0.05 && std::abs (d.notes[1].t0 - notes[1].t0) < 1e-9 && std::abs (d.notes[1].t1 - notes[1].t1) < 1e-9, "joined note matches the original (%.3f vs %.3f)", d.notes[1].centre, notes[1].centre);

    d.notes[0].target += 3.f;
    CHECK (d.undo() && std::abs (d.notes[0].target - notes[0].target) < 1e-6 && d.notes.size() == 3, "undo restores the notes");
    CHECK (d.redo(), "redo");
    d.notes = notes;
    d.notes[2].muted = true; d.notes[2].gainDb = -3.f; d.notes[2].formant = 1.5f; d.notes[1].drift = 0.3f; d.notes[1].vib = 1.4f; moveNote (d.notes, 2, d.notes[2].nt0 + 0.1, 10.0);

    resetNote (d.notes, 2, 10.0);
    CHECK (! d.notes[2].muted && d.notes[2].gainDb == 0.f && d.notes[2].formant == 0.f && std::abs (d.notes[2].nt0 - notes[2].t0) < 1e-9, "reset note");

    // serialise
    d.notes[1].target = 70.25f;
    const auto bytes = serialiseNotes (d.notes, sr, hop);
    std::vector<VocalNote> back; double sr2 = 0; int hop2 = 0;
    CHECK (deserialiseNotes (bytes.data(), bytes.size(), back, sr2, hop2), "notes deserialise (%zu bytes)", bytes.size());
    bool same = back.size() == d.notes.size() && sr2 == sr && hop2 == hop;
    for (size_t i = 0; same && i < back.size(); ++i)
        same = back[i].t0 == d.notes[i].t0 && back[i].nt1 == d.notes[i].nt1 && back[i].target == d.notes[i].target && back[i].drift == d.notes[i].drift
               && back[i].vib == d.notes[i].vib && back[i].muted == d.notes[i].muted && back[i].curve->midi == d.notes[i].curve->midi
               && std::abs (back[i].curve->slow[3] - d.notes[i].curve->slow[3]) < 1e-5f;
    CHECK (same, "round trip keeps every field");
    CHECK (! deserialiseNotes (bytes.data(), bytes.size() / 2, back, sr2, hop2) && ! deserialiseNotes (bytes.data(), 3, back, sr2, hop2), "truncated data is rejected");

    // resampler
    {
        std::vector<float> s ((size_t) 44100);
        for (size_t i = 0; i < s.size(); ++i) s[i] = (float) (0.5 * std::sin (2.0 * kPi * 1000.0 * (double) i / 44100.0));
        const auto up = resampleSinc (s, 44100.0, 48000.0);
        CHECK (std::abs ((double) up.size() - 48000.0) < 2.0, "44.1 -> 48 kHz length %zu", up.size());
        const double f = estimateF0 (up, 10000, 4000, 48000.0, 500.0, 2000.0);
        const double a = rmsDb (up, 10000, 20000);
        CHECK (std::abs (f - 1000.0) < 2.0 && std::abs (a - rmsDb (s, 10000, 20000)) < 0.1, "resampled sine: %.1f Hz, level %.2f dB", f, a);
        const auto back2 = resampleSinc (up, 48000.0, 44100.0);
        double err = 0; for (size_t i = 5000; i < 30000; ++i) err = std::max (err, (double) std::abs (back2[i] - s[i]));
        CHECK (err < 0.01, "there and back again: max error %.4f", err);
    }
}

// -----------------------------------------------------------------------------------------------------------------
static void testVocalSession()
{
    std::printf ("\n-- vocal: session (record, analyse, render, play, export, save) --\n");
    const double sr = 44100.0;
    const auto clip = sing (sr, 3.2, melody3(), { { 1.0, 1.08 } });

    VocalSession s;
    s.prepare (sr);
    CHECK (! s.hasClip(), "starts empty");
    s.startRecording (false);
    CHECK (s.recMode() == VocalSession::RecRecording, "record now starts immediately");
    const int B = 512;
    std::vector<float> l (B), r (B);
    bool replaced = false;
    for (size_t pos = 0; pos + B <= clip.x.size() / 2; pos += B)
    {
        for (int i = 0; i < B; ++i) l[(size_t) i] = r[(size_t) i] = clip.x[pos + (size_t) i];
        s.capture (l.data(), r.data(), B, true, 8.0 + (double) pos / sr * 2.0);
        VocalSession::PlayParams pp;
        replaced |= s.process (l.data(), r.data(), B, true, 8.0, 120.0, pp);
    }
    CHECK (! replaced, "the clip never replaces the input while recording");
    CHECK (s.recSeconds() > 1.5 && s.recSeconds() < 1.7, "recorded %.2f s", s.recSeconds());
    for (size_t pos = clip.x.size() / 2 / B * B; pos + B <= clip.x.size(); pos += B)
    {
        for (int i = 0; i < B; ++i) l[(size_t) i] = r[(size_t) i] = clip.x[pos + (size_t) i];
        s.capture (l.data(), r.data(), B, true, 9.0);
    }
    s.stopRecording();
    s.capture (l.data(), r.data(), B, true, 9.0);
    CHECK (s.recMode() == VocalSession::RecDone, "stop is honoured by the audio thread");
    CHECK (waitFor ([&] { s.poll(); return s.hasClip() && ! s.busy() && ! s.rendering() && ! s.doc().notes.empty(); }, 20000), "analysis finished: %d notes", (int) s.doc().notes.size());
    CHECK (s.doc().notes.size() == 3, "3 notes found in the recording (%d)", (int) s.doc().notes.size());
    double anchor = 0;
    CHECK (s.takeAnchor (anchor) && std::abs (anchor - 8.0) < 1e-6, "clip anchored at the beat where recording began (%.2f)", anchor);
    CHECK (std::abs (s.clipSeconds() - (double) (clip.x.size() / B * B) / sr) < 0.02, "clip length %.2f s", s.clipSeconds());

    // edit + render
    AutoSettings a; a.correct = 1.f; a.flatten = 1.f;
    auto& d = s.doc();
    d.checkpoint();
    applyAuto (d.notes, a);
    s.notesChanged();
    CHECK (waitFor ([&] { s.poll(); return ! s.rendering(); }, 20000), "render finished");
    const auto rendered = s.renderedCopy();
    CHECK (rendered.size() > 100000, "rendered %zu samples", rendered.size());
    CHECK (std::abs (noteMedianMidi (rendered, sr, 0.3, 0.95) - 60.0) < 0.1, "rendered note 1 sits on C4 (%.2f)", noteMedianMidi (rendered, sr, 0.3, 0.95));

    // playback follows the host beat
    {
        VocalSession::PlayParams pp; pp.follow = true; pp.anchorBeat = 8.0;
        std::fill (l.begin(), l.end(), 0.f); std::fill (r.begin(), r.end(), 0.f);
        const double beat = 8.0 + 0.5 * 2.0 * 0.0 + (0.4 * 2.0);           // 0.4 s into the clip at 120 bpm
        const bool rep = s.process (l.data(), r.data(), B, true, beat, 120.0, pp);
        const size_t p0 = (size_t) std::llround (0.4 * sr);
        double diff = 0; for (int i = 0; i < B; ++i) diff = std::max (diff, (double) std::abs (l[(size_t) i] - rendered[p0 + (size_t) i]));
        CHECK (rep && diff < 1e-6 && l == r, "host-synced playback is sample accurate (diff %.2g)", diff);
        pp.follow = false;
        CHECK (! s.process (l.data(), r.data(), B, true, beat, 120.0, pp), "without 'play with project' the live input passes through");
        pp.original = true; pp.follow = true;
        s.process (l.data(), r.data(), B, true, beat, 120.0, pp);
        double d2 = 0; for (int i = 0; i < B; ++i) d2 = std::max (d2, (double) std::abs (l[(size_t) i] - clip.x[p0 + (size_t) i]));
        CHECK (d2 < 1e-6, "A/B plays the untouched clip (diff %.2g)", d2);
    }
    // internal transport, loop and audition
    {
        VocalSession::PlayParams pp; pp.follow = false; pp.loop = true;
        s.seekSeconds (0.0);
        s.play (true);
        CHECK (s.process (l.data(), r.data(), B, false, 0.0, 120.0, pp), "internal play replaces the input");
        s.play (false);
        s.auditionRange (1.2, 1.2 + 2.0 * B / sr);
        int blocks = 0;
        while (s.isPlaying() && blocks < 20) { s.process (l.data(), r.data(), B, false, 0.0, 120.0, pp); ++blocks; }
        CHECK (blocks == 3 || blocks == 2, "audition stops by itself after %d blocks", blocks);
    }
    // export + save/load
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_vocal_test.wav");
        CHECK (s.exportWav (f).wasOk() && f.getSize() > 100000, "exported %lld bytes", (long long) f.getSize());
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        CHECK (rd && rd->numChannels == 1 && std::abs (rd->sampleRate - sr) < 1 && std::abs ((double) rd->lengthInSamples - (double) rendered.size()) < 2, "exported file is a mono %0.f Hz wav of the edited clip", sr);
        f.deleteFile();

        auto st = s.saveState();
        CHECK (st.getSize() > 1000, "saved %zu bytes of state", st.getSize());
        VocalSession s2;
        s2.prepare (sr);
        s2.loadState (st.getData(), st.getSize());
        CHECK (waitFor ([&] { s2.poll(); return s2.hasClip() && ! s2.busy() && ! s2.rendering(); }, 20000), "restored in a second session");
        bool same = s2.doc().notes.size() == s.doc().notes.size();
        for (size_t i = 0; same && i < s2.doc().notes.size(); ++i)
            same = s2.doc().notes[i].target == s.doc().notes[i].target && s2.doc().notes[i].t0 == s.doc().notes[i].t0;
        CHECK (same, "notes and edits came back");
        const auto r2 = s2.renderedCopy();
        double e = 0, dd = 0;
        for (size_t i = 0; i < r2.size() && i < rendered.size(); ++i) { e += (double) rendered[i] * rendered[i]; const double df = (double) rendered[i] - r2[i]; dd += df * df; }
        CHECK (r2.size() == rendered.size() && 10.0 * std::log10 (e / std::max (dd, 1e-20)) > 60.0, "the restored clip renders the same (24-bit FLAC copy)");
    }
    // file load
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_vocal_in.wav");
        f.deleteFile();
        {
            juce::WavAudioFormat wav;
            auto os = std::make_unique<juce::FileOutputStream> (f);
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, 2, 24, {}, 0));
            os.release();
            const auto c48 = sing (48000.0, 3.2, melody3());
            juce::AudioBuffer<float> b (2, (int) c48.x.size());
            b.copyFrom (0, 0, c48.x.data(), (int) c48.x.size()); b.copyFrom (1, 0, c48.x.data(), (int) c48.x.size());
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        VocalSession s3;
        s3.prepare (44100.0);                                               // host rate differs from the file's
        s3.loadFile (f);
        CHECK (waitFor ([&] { s3.poll(); return s3.hasClip() && ! s3.busy() && ! s3.rendering() && ! s3.doc().notes.empty(); }, 20000), "loaded a 48 kHz stereo wav into a 44.1 kHz host: %d notes", (int) s3.doc().notes.size());
        CHECK (std::abs (s3.clipSampleRate() - 48000.0) < 1 && std::abs (s3.clipSeconds() - 3.2) < 0.01, "clip keeps its own rate (%.0f) and length (%.2f s)", s3.clipSampleRate(), s3.clipSeconds());
        const auto cp = s3.renderedCopy();
        CHECK (std::abs ((double) cp.size() - 3.2 * 44100.0) < 50.0, "playback copy is at the host rate (%zu samples)", cp.size());
        f.deleteFile();
        VocalSession s4;
        s4.loadFile (juce::File ("/nonexistent/none.wav"));
        waitFor ([&] { s4.poll(); return ! s4.busy(); }, 3000);
        CHECK (! s4.hasClip() && s4.status().isNotEmpty(), "a missing file is reported (\"%s\")", s4.status().toRawUTF8());
    }
    // arming waits for the host
    {
        VocalSession s5;
        s5.prepare (sr);
        s5.startRecording (true);
        std::fill (l.begin(), l.end(), 0.1f); std::fill (r.begin(), r.end(), 0.1f);
        for (int i = 0; i < 10; ++i) s5.capture (l.data(), r.data(), B, false, 0.0);
        CHECK (s5.recMode() == VocalSession::RecArmed && s5.recSeconds() == 0.0, "armed but nothing recorded while the host is stopped");
        for (int i = 0; i < 10; ++i) s5.capture (l.data(), r.data(), B, true, 4.0 + i * 0.1);
        CHECK (s5.recMode() == VocalSession::RecRecording && s5.recSeconds() > 0.1, "starts when the host starts (%.2f s)", s5.recSeconds());
        s5.capture (l.data(), r.data(), B, false, 0.0);
        CHECK (s5.recMode() == VocalSession::RecDone, "stops when the host stops");
        s5.poll();
        s5.stopRecording();
    }
}

static void testVocalProcessor()
{
    std::printf ("\n-- vocal: inside the processor --\n");
    AIOProcessor p;
    p.setPlayConfigDetails (2, 2, 44100.0, 512);
    p.prepareToPlay (44100.0, 512);
    auto& v = p.getVocal();
    const double sr = 44100.0;
    const auto clip = sing (sr, 3.2, melody3(), { { 1.0, 1.08 } });

    p.setParam ("tuneKey", 0.f);
    v.startRecording (false);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buf (2, 512);
    for (size_t pos = 0; pos + 512 <= clip.x.size(); pos += 512)
    {
        buf.copyFrom (0, 0, clip.x.data() + pos, 512); buf.copyFrom (1, 0, clip.x.data() + pos, 512);
        p.processBlock (buf, midi);
    }
    v.stopRecording();
    buf.clear();
    p.processBlock (buf, midi);
    CHECK (waitFor ([&] { v.poll(); return v.hasClip() && ! v.busy() && ! v.rendering() && v.doc().notes.size() == 3; }, 20000), "recorded through processBlock: %d notes", (int) v.doc().notes.size());

    p.setParam ("vedCorrect", 100.f); p.setParam ("vedFlatten", 100.f);
    p.autoEditVocal();
    CHECK (waitFor ([&] { v.poll(); return ! v.rendering(); }, 20000), "auto edit rendered");
    CHECK (std::abs (v.doc().notes[0].target - 60.0) < 1e-3, "auto edit set the targets (%.2f)", v.doc().notes[0].target);
    CHECK (v.doc().undo() && std::abs (v.doc().notes[0].target - v.doc().notes[0].centre) < 1e-4, "...and can be undone");

    // internal play makes sound from silence, through the whole chain
    v.doc().redo(); v.notesChanged();
    waitFor ([&] { return ! v.rendering(); }, 20000);
    v.seekSeconds (0.4);
    v.play (true);
    p.setParam ("vedFollow", 0.f);
    buf.clear();
    p.processBlock (buf, midi);
    CHECK (buf.getMagnitude (0, 512) > 0.01f, "internal playback is audible from an empty input (peak %.3f)", buf.getMagnitude (0, 512));
    v.play (false);

    // a state round trip
    juce::MemoryBlock mb;
    p.getStateInformation (mb);
    AIOProcessor q;
    q.setPlayConfigDetails (2, 2, 44100.0, 512);
    q.prepareToPlay (44100.0, 512);
    q.setStateInformation (mb.getData(), (int) mb.getSize());
    CHECK (waitFor ([&] { q.getVocal().poll(); return q.getVocal().hasClip() && ! q.getVocal().busy() && ! q.getVocal().rendering(); }, 20000)
               && q.getVocal().doc().notes.size() == 3 && std::abs (q.getVocal().doc().notes[0].target - 60.0) < 1e-3,
           "the clip and its edits survive a state save (%zu bytes) / load", mb.getSize());
}

// =================================================================================================================
// the note editor widget, driven with synthetic mouse events
// =================================================================================================================
#include "../Source/ui/VocalUI.h"

namespace
{
juce::File writeTempWav (const std::vector<float>& x, double sr, const juce::String& name)
{
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile (name);
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 1, 24, {}, 0));
    if (w) { os.release(); juce::AudioBuffer<float> b (1, (int) x.size()); b.copyFrom (0, 0, x.data(), (int) x.size()); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
    return f;
}

bool loadClipAndWait (AIOProcessor& p, const SungClip& clip, const juce::String& name)
{
    auto& v = p.getVocal();
    v.loadFile (writeTempWav (clip.x, clip.sr, name));
    return waitFor ([&] { v.poll(); return v.hasClip() && ! v.busy() && ! v.rendering() && ! v.doc().notes.empty(); }, 30000);
}
} // namespace

static void prepareVocalSnapshot (AIOProcessor& proc)
{
    const double sr = 44100.0;
    auto ns = melody3();
    ns.push_back ({ 2.85, 3.5, 62.0 + 0.15 });
    const auto clip = sing (sr, 3.7, ns, { { 1.0, 1.08 } });
    auto& v = proc.getVocal();
    if (! loadClipAndWait (proc, clip, "aio_snap_vox.wav")) return;
    auto& d = v.doc();
    if (d.notes.size() >= 4)
    {
        d.notes[0].target = (float) std::round (d.notes[0].target);                    // snapped to the grid
        d.notes[1].target += 1.6f; d.notes[1].vib = 0.4f;                              // pushed up, less vibrato
        d.notes[2].muted = false; d.notes[2].gainDb = -3.f;
        aio::moveNote (d.notes, 3, d.notes[3].nt0 + 0.12, 60.0);
    }
    v.notesChanged();
    waitFor ([&] { v.poll(); return ! v.rendering(); }, 30000);
}

static void testVocalUI()
{
    std::printf ("\n-- vocal: note editor widget (synthetic mouse) --\n");
    using namespace aio::ui;
    const double sr = 44100.0;
    AIOProcessor p;
    p.setPlayConfigDetails (2, 2, sr, 512);
    p.prepareToPlay (sr, 512);
    p.setParam ("vedSnap", 1.f);                       // drag snaps to semitones
    const auto clip = sing (sr, 3.2, melody3(), { { 1.0, 1.08 } });
    CHECK (loadClipAndWait (p, clip, "aio_ui_vox.wav") && p.getVocal().doc().notes.size() == 3, "clip loaded: %zu notes", p.getVocal().doc().notes.size());
    auto& v = p.getVocal();
    auto& notes = v.doc().notes;

    VocalRoll roll (p);
    roll.setBounds (0, 0, 1000, 420);
    int edits = 0, selChanges = 0;
    roll.onEdited = [&] { ++edits; };
    roll.onSelectionChanged = [&] { ++selChanges; };
    roll.clipChanged();
    roll.createComponentSnapshot (roll.getLocalBounds());

    auto mid = [&] (int i) { return roll.pointFor (0.5 * (notes[(size_t) i].nt0 + notes[(size_t) i].nt1), notes[(size_t) i].target); };
    bool inView = true;
    for (size_t i = 0; i < notes.size(); ++i) { const auto r = roll.blobRect (notes[i]); inView &= r.getX() >= VocalRoll::keyW && r.getRight() <= 1000.f && r.getY() >= VocalRoll::rulerH && r.getBottom() <= 420.f; }
    CHECK (inView, "fit-to-clip shows every note");

    // click selects
    auto p0 = mid (0);
    roll.mouseDown (mouseEv (roll, p0)); roll.mouseUp (mouseEv (roll, p0));
    CHECK (roll.selectedCount() == 1 && roll.firstSelected() == 0 && selChanges > 0, "click selects the note");

    // drag up two semitones: snaps onto a whole semitone
    const float centre0 = notes[0].centre;
    auto p1 = roll.pointFor (0.5 * (notes[0].nt0 + notes[0].nt1), notes[0].target + 2.0);
    roll.mouseDown (mouseEv (roll, p0));
    roll.mouseDrag (mouseEv (roll, { p0.x, 0.5f * (p0.y + p1.y) }, 0, true, p0));
    roll.mouseDrag (mouseEv (roll, p1, 0, true, p0));
    roll.mouseUp (mouseEv (roll, p1, 0, true, p0));
    CHECK (std::abs (notes[0].target - std::round (centre0 + 2.0)) < 1e-3, "dragging up 2 semitones snaps to a whole semitone (%.3f from %.3f)", notes[0].target, centre0);
    CHECK (edits > 0 && notes[0].pitchEdited(), "the edit is reported");
    CHECK (waitFor ([&] { v.poll(); return ! v.rendering(); }, 20000), "re-rendered after the drag");
    {
        const auto out = v.renderedCopy();
        const double m = noteMedianMidi (out, sr, notes[0].nt0, notes[0].nt1);
        CHECK (std::abs (m - notes[0].target) < 0.15, "the audio follows: %.2f vs target %.2f", m, notes[0].target);
    }

    // undo / redo
    const float edited = notes[0].target;
    CHECK (roll.undo() && std::abs (notes[0].target - centre0) < 1e-4, "undo restores the original pitch");
    CHECK (roll.redo() && std::abs (notes[0].target - edited) < 1e-4, "redo puts the edit back");

    // one undo step per drag (no matter how many mouse moves)
    {
        const size_t before = v.doc().undoStack.size();
        auto a = mid (1);
        roll.mouseDown (mouseEv (roll, a));
        for (int k = 1; k <= 8; ++k) roll.mouseDrag (mouseEv (roll, { a.x, a.y - 3.f * (float) k }, 0, true, a));
        roll.mouseUp (mouseEv (roll, { a.x, a.y - 24.f }, 0, true, a));
        CHECK (v.doc().undoStack.size() == before + 1, "a whole drag is a single undo step (%zu -> %zu)", before, v.doc().undoStack.size());
        CHECK (roll.undo() && std::abs (notes[1].target - notes[1].centre) < 1e-4, "...and it undoes in one go");
    }

    // a click without moving changes nothing and adds no undo step
    {
        const size_t before = v.doc().undoStack.size();
        auto a = mid (2);
        const float t = notes[2].target;
        roll.mouseDown (mouseEv (roll, a)); roll.mouseUp (mouseEv (roll, a));
        CHECK (v.doc().undoStack.size() == before && notes[2].target == t, "a click alone does not edit");
    }

    // move in time
    {
        const double t0 = notes[2].nt0;
        roll.selectOnly (2);
        auto a = mid (2);
        const float dx = (float) (0.20 * (roll.pointFor (1.0, 0).x - roll.pointFor (0.0, 0).x));
        roll.mouseDown (mouseEv (roll, a));
        roll.mouseDrag (mouseEv (roll, { a.x + dx, a.y }, 0, true, a));
        roll.mouseUp (mouseEv (roll, { a.x + dx, a.y }, 0, true, a));
        CHECK (std::abs (notes[2].nt0 - (t0 + 0.20)) < 1e-6 && std::abs (notes[2].target - notes[2].centre) < 1e-6, "dragging sideways moves the note in time (%.3f -> %.3f)", t0, notes[2].nt0);
        roll.undo();
        CHECK (std::abs (notes[2].nt0 - t0) < 1e-6, "undo moves it back");
    }

    // can't be dragged through its neighbour
    {
        roll.selectOnly (0);
        auto a = mid (0);
        const float dx = (float) (3.0 * (roll.pointFor (1.0, 0).x - roll.pointFor (0.0, 0).x));
        roll.mouseDown (mouseEv (roll, a));
        roll.mouseDrag (mouseEv (roll, { a.x + dx, a.y }, 0, true, a));
        roll.mouseUp (mouseEv (roll, { a.x + dx, a.y }, 0, true, a));
        CHECK (notes[0].nt1 <= notes[1].nt0 + 1e-9, "a note stops at its neighbour (%.3f <= %.3f)", notes[0].nt1, notes[1].nt0);
        roll.undo();
    }

    // stretch from the right edge
    {
        roll.selectOnly (0);
        const double e0 = notes[0].nt1;
        const auto r = roll.blobRect (notes[0]);
        const juce::Point<float> a (r.getRight() - 2.f, r.getCentreY());
        const float dx = (float) (0.08 * (roll.pointFor (1.0, 0).x - roll.pointFor (0.0, 0).x));
        roll.mouseDown (mouseEv (roll, a));
        roll.mouseDrag (mouseEv (roll, { a.x + dx, a.y }, 0, true, a));
        roll.mouseUp (mouseEv (roll, { a.x + dx, a.y }, 0, true, a));
        CHECK (std::abs (notes[0].nt1 - (e0 + 0.08)) < 1e-6, "dragging the right edge stretches the note (%.3f -> %.3f)", e0, notes[0].nt1);
        roll.undo();
    }

    // box select + shift click
    {
        roll.selectNone();
        const juce::Point<float> a (VocalRoll::keyW + 4.f, VocalRoll::rulerH + 4.f), b (996.f, 416.f);
        roll.mouseDown (mouseEv (roll, a));
        roll.mouseDrag (mouseEv (roll, b, 0, true, a));
        roll.mouseUp (mouseEv (roll, b, 0, true, a));
        CHECK (roll.selectedCount() == 3, "a box around everything selects all notes (%d)", roll.selectedCount());
        auto c = mid (1);
        roll.mouseDown (mouseEv (roll, c, juce::ModifierKeys::shiftModifier));
        roll.mouseUp (mouseEv (roll, c, juce::ModifierKeys::shiftModifier));
        CHECK (roll.selectedCount() == 2 && roll.selection()[1] == 0, "shift-click removes a note from the selection");
    }

    // several notes move together
    {
        roll.selectAll();
        const float t0 = notes[0].target, t1 = notes[2].target;
        auto a = mid (0);
        const float dy = -1.0f * (float) (roll.pointFor (0, 0).y - roll.pointFor (0, 1.0).y) * -1.f;   // one semitone up (screen y goes down)
        roll.mouseDown (mouseEv (roll, a));
        roll.mouseDrag (mouseEv (roll, { a.x, a.y - std::abs (dy) }, 0, true, a));
        roll.mouseUp (mouseEv (roll, { a.x, a.y - std::abs (dy) }, 0, true, a));
        CHECK (notes[0].target > t0 + 0.4 && notes[2].target > t1 + 0.4 && std::abs ((notes[0].target - t0) - (notes[2].target - t1)) < 0.6, "all selected notes follow the drag (%.2f, %.2f)", notes[0].target - t0, notes[2].target - t1);
        roll.undo();
    }

    // snap, flatten, mute, reset through the helpers the buttons use
    {
        roll.selectOnly (1);
        notes[1].target += 0.3f;
        roll.snapSelected (1);
        CHECK (std::abs (notes[1].target - std::round (notes[1].target)) < 1e-4, "snap to semitone");
        p.setParam ("tuneKey", 0.f); p.setParam ("tuneScale", 1.f);        // C major
        notes[1].target = 61.2f;
        roll.snapSelected (2);
        CHECK (std::abs (notes[1].target - 60.f) < 1e-4 || std::abs (notes[1].target - 62.f) < 1e-4, "snap to scale lands on a C major note (%.2f)", notes[1].target);
        roll.editSelected ([] (aio::VocalNote& n) { n.vib = 0.f; });
        CHECK (notes[1].vib == 0.f, "remove vibrato");
        roll.toggleMute();
        CHECK (notes[1].muted, "mute");
        roll.toggleMute();
        CHECK (! notes[1].muted, "unmute");
        roll.resetSelected();
        CHECK (! notes[1].edited(), "reset returns the note to how it was sung");
    }

    // split / join
    {
        const size_t n0 = notes.size();
        const double tSplit = 0.5 * (notes[1].nt0 + notes[1].nt1);
        CHECK (roll.splitAt (tSplit) && notes.size() == n0 + 1, "split makes two notes (%zu)", notes.size());
        CHECK (roll.selection().size() == notes.size(), "the selection follows the split");
        roll.selectNone();
        roll.selectOnly (1);
        CHECK (roll.joinSelected() && notes.size() == n0, "join puts them back (%zu)", notes.size());
        CHECK (! roll.splitAt (0.0), "splitting where there is no note does nothing");
    }

    // keyboard
    {
        roll.selectOnly (0);
        roll.keyPressed (juce::KeyPress (juce::KeyPress::deleteKey));
        CHECK (notes[0].muted, "Delete mutes the selection");
        roll.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
        CHECK (! notes[0].muted, "Ctrl+Z undoes it");
        roll.keyPressed (juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0));
        CHECK (roll.selectedCount() == (int) notes.size(), "Ctrl+A selects everything");
    }

    // zoom
    {
        const float before = roll.pointFor (1.0, 0).x - roll.pointFor (0.0, 0).x;
        roll.mouseWheelMove (mouseEv (roll, { 500.f, 200.f }, juce::ModifierKeys::ctrlModifier), juce::MouseWheelDetails { 0.f, 0.5f, false, false, false });
        const float after = roll.pointFor (1.0, 0).x - roll.pointFor (0.0, 0).x;
        CHECK (after > before * 1.2f, "ctrl + wheel zooms in time (%.1f -> %.1f px/s)", before, after);
    }

    // AUTO from the processor, then the roll shows the result
    {
        p.setParam ("vedCorrect", 100.f); p.setParam ("vedFlatten", 100.f); p.setParam ("vedVibrato", 100.f);
        roll.selectNone();
        p.autoEditVocal();
        waitFor ([&] { v.poll(); return ! v.rendering(); }, 20000);
        {
            const auto out = v.renderedCopy();
            const double m0 = noteMedianMidi (out, sr, notes[0].nt0, notes[0].nt1);
            CHECK (std::abs (m0 - std::round (m0)) < 0.2, "...and the audio of note 1 is on a whole semitone (%.2f)", m0);
        }
        CHECK (notes[0].pitchEdited() && notes[2].pitchEdited(), "AUTO marks the corrected notes as edited");
        roll.createComponentSnapshot (roll.getLocalBounds());
    }
}

static bool selectVocalNote (AIOEditor& ed, int index)
{
    if (auto* roll = ed.vocalRoll()) { roll->selectOnly (index); return true; }
    return false;
}
