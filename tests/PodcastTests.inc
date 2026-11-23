// =================================================================================================================
// Podcast Ready: declip, denoise, loudness + the session / processor / UI around it
// =================================================================================================================
#include "../Source/podcast/Podcast.h"
#include "../Source/podcast/MediaDecode.h"

namespace
{
struct SpeechClip
{
    std::vector<float> x;
    std::vector<std::pair<size_t, size_t>> gaps, words;
};

// words of different loudness separated by pauses; voiced vowels so the spectrum looks like speech, not a tone
SpeechClip makeSpeech (double sr, int nWords, unsigned seed = 7)
{
    SpeechClip c;
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (0.f, 1.f);
    static const double levels[4] = { 0.20, 0.06, 0.12, 0.03 };
    for (int w = 0; w < nWords; ++w)
    {
        const double f0 = 105.0 + 30.0 * u (rng);
        const int len = 2 + (int) (u (rng) * 2.f);
        std::vector<int> vs;
        for (int k = 0; k < len; ++k) vs.push_back ((int) (u (rng) * 5.f));
        auto v = vowelVoice (sr, f0, vs, 0.16, levels[w % 4]);
        const size_t fade = (size_t) (0.02 * sr);
        for (size_t i = 0; i < fade && i < v.size(); ++i) { const float g = (float) i / (float) fade; v[i] *= g; v[v.size() - 1 - i] *= g; }
        const size_t a = c.x.size();
        c.x.insert (c.x.end(), v.begin(), v.end());
        c.words.push_back ({ a, c.x.size() });
        const size_t gap = (size_t) ((0.25 + 0.15 * u (rng)) * sr);
        c.gaps.push_back ({ c.x.size(), c.x.size() + gap });
        c.x.insert (c.x.end(), gap, 0.f);
    }
    return c;
}

void addNoise (std::vector<float>& x, double rms, unsigned seed = 3)
{
    std::mt19937 rng (seed);
    std::normal_distribution<float> g (0.f, 1.f);
    float lp = 0.f;
    for (auto& v : x) { lp += 0.6f * (g (rng) - lp); v += (float) rms * 1.25f * lp; }
}

double rmsRanges (const std::vector<float>& x, const std::vector<std::pair<size_t, size_t>>& rs, size_t skipEdge = 0)
{
    double s = 0; size_t n = 0;
    for (auto& r : rs)
        for (size_t i = r.first + skipEdge; i + skipEdge < r.second && i < x.size(); ++i) { s += (double) x[i] * x[i]; ++n; }
    return n > 0 ? std::sqrt (s / (double) n) : 0.0;
}

juce::AudioBuffer<float> toBuffer (const std::vector<float>& x, bool stereo = true)
{
    juce::AudioBuffer<float> b (stereo ? 2 : 1, (int) x.size());
    for (int c = 0; c < b.getNumChannels(); ++c) std::copy (x.begin(), x.end(), b.getWritePointer (c));
    return b;
}

std::vector<float> hardClip (const std::vector<float>& x, float gain, float level)
{
    std::vector<float> y (x.size());
    for (size_t i = 0; i < x.size(); ++i) y[i] = std::clamp (x[i] * gain, -level, level);
    return y;
}
} // namespace

static void testPodcastDSP()
{
    std::printf ("\n== Podcast Ready: DSP ==\n");
    using namespace aio::podcast;
    const double sr = 44100.0;
    const auto clean = makeSpeech (sr, 16);
    CHECK (clean.x.size() > (size_t) (8 * sr), "synthetic speech is %.1f s long", (double) clean.x.size() / sr);
    {
        // the FFT pair the denoiser relies on must be unity (with the 0.5 overlap-add scale of a sine window at 75 %)
        juce::dsp::FFT f (11);
        std::vector<float> d (4096, 0.f);
        for (int i = 0; i < 2048; ++i) d[(size_t) 2 * i] = std::sin (0.05f * (float) i) + 0.3f * std::cos (0.31f * (float) i);
        const auto ref = d;
        f.performRealOnlyForwardTransform (d.data(), true);
        for (int b = 1; b < 1024; ++b) { d[(size_t) 2 * (2048 - b)] = d[(size_t) 2 * b]; d[(size_t) 2 * (2048 - b) + 1] = -d[(size_t) 2 * b + 1]; }
        d[1] = 0.f; d[2049] = 0.f;
        f.performRealOnlyInverseTransform (d.data());
        double num = 0, den = 0; for (int i = 0; i < 2048; ++i) { num += (double) d[(size_t) 2 * i] * ref[(size_t) 2 * i]; den += (double) ref[(size_t) 2 * i] * ref[(size_t) 2 * i]; }
        std::printf ("      (fft round-trip gain %.4f)\n", num / den);
        CHECK (std::abs (num / den - 0.5) < 0.01, "the FFT pair has the 0.5 gain the denoiser compensates for (%.3f)", num / den);
    }

    // ------------------------------------------------------------------ declip
    {
        // scale so the loud words overshoot, then clip hard at 0.9
        float pk = 0.f; for (float v : clean.x) pk = std::max (pk, std::abs (v));
        const float gain = 1.9f / pk;
        std::vector<float> truth (clean.x.size());
        for (size_t i = 0; i < truth.size(); ++i) truth[i] = clean.x[i] * gain;
        const auto clipped = hardClip (clean.x, gain, 0.9f);
        const long long nClip = countClipped (clipped.data(), clipped.size());
        CHECK (nClip > 100, "the clipped test signal really has flat tops (%lld samples)", nClip);
        CHECK (countClipped (clean.x.data(), clean.x.size()) == 0, "clean speech reports no clipping");

        auto fixedSig = clipped;
        const long long fixedN = declipChannel (fixedSig.data(), fixedSig.size());
        CHECK (fixedN >= nClip * 9 / 10, "declip rebuilt %lld of %lld clipped samples", fixedN, nClip);
        double e0 = 0, e1 = 0; size_t cnt = 0; float newPeak = 0.f;
        for (size_t i = 0; i < clipped.size(); ++i)
            if (std::abs (clipped[i]) >= 0.8999f)
            {
                e0 += std::pow ((double) clipped[i] - truth[i], 2.0);
                e1 += std::pow ((double) fixedSig[i] - truth[i], 2.0);
                newPeak = std::max (newPeak, std::abs (fixedSig[i])); ++cnt;
            }
        CHECK (cnt > 0 && e1 < 0.35 * e0, "error on the clipped samples drops to %.0f %% of the clipped version", 100.0 * e1 / std::max (e0, 1e-12));
        CHECK (newPeak > 1.0f, "rebuilt peaks rise above the clip level (%.2f vs 0.90)", newPeak);
        bool fin = true; for (float v : fixedSig) fin = fin && std::isfinite (v);
        CHECK (fin, "declipped signal is finite");
        double untouched = 0;
        for (size_t i = 0; i < clipped.size(); ++i) if (std::abs (clipped[i]) < 0.85f) untouched = std::max (untouched, (double) std::abs (fixedSig[i] - clipped[i]));
        CHECK (untouched < 1.0e-6, "samples that were not clipped are left alone (max change %.2g)", untouched);

        auto copy2 = clean.x;
        CHECK (declipChannel (copy2.data(), copy2.size()) == 0, "declip does nothing to a clean file");

        // a clean file that is simply loud (peak normalised) must not be touched
        auto loud = clean.x; float lp = 0.f; for (float v : loud) lp = std::max (lp, std::abs (v));
        for (auto& v : loud) v = v / lp * 0.999f;
        CHECK (declipChannel (loud.data(), loud.size()) == 0, "a peak-normalised, unclipped file is left alone");
    }

    // ------------------------------------------------------------------ denoise
    {
        auto noisy = clean.x; addNoise (noisy, 0.006);
        const double gapBefore = rmsRanges (noisy, clean.gaps, 2000);
        const double wordBefore = rmsRanges (noisy, clean.words, 2000);
        auto l = noisy, r = noisy;
        bool eased = false;
        const bool ok = denoise (l.data(), r.data(), l.size(), sr, 0.6f, eased, nullptr);
        CHECK (ok && l.size() == noisy.size(), "denoise finishes and keeps the length");
        const double gapAfter = rmsRanges (l, clean.gaps, 2000);
        const double wordAfter = rmsRanges (l, clean.words, 2000);
        const double dropDb = 20.0 * std::log10 (gapBefore / std::max (gapAfter, 1e-9));
        CHECK (dropDb > 9.0, "background noise in the pauses drops by %.1f dB", dropDb);
        CHECK (std::abs (20.0 * std::log10 (wordAfter / wordBefore)) < 1.5, "speech level changes by only %.2f dB", 20.0 * std::log10 (wordAfter / wordBefore));
        CHECK (! eased, "pauses were found, so the denoiser did not back off");
        double dl = 0; for (size_t i = 0; i < l.size(); ++i) dl = std::max (dl, (double) std::abs (l[i] - r[i]));
        CHECK (dl < 1e-5, "both channels get the same treatment (diff %.2g)", dl);

        // amount 0 is a no-op
        auto z = noisy; bool e2 = false;
        denoise (z.data(), z.data(), z.size(), sr, 0.f, e2, nullptr);
        CHECK (z == noisy, "denoise at 0 changes nothing");

        // no pauses at all -> the denoiser eases off instead of eating the speech
        std::vector<float> dense;
        dense = vowelVoice (sr, 120.0, { 0 }, 6.0, 0.1);
        addNoise (dense, 0.004);
        auto dd = dense; bool e3 = false;
        denoise (dd.data(), dd.data(), dd.size(), sr, 0.6f, e3, nullptr);
        CHECK (e3, "no pauses -> the denoiser reports it eased off");
        double e = 0, f = 0; for (size_t i = 0; i < dense.size(); ++i) { e += (double) dense[i] * dense[i]; f += (double) dd[i] * dd[i]; }
        CHECK (std::abs (10.0 * std::log10 (f / e)) < 2.0, "...and the speech keeps its level (%.2f dB)", 10.0 * std::log10 (f / e));

        // cancel
        int calls = 0; bool e4 = false; auto cc = noisy;
        const bool done = denoise (cc.data(), cc.data(), cc.size(), sr, 0.5f, e4, [&] (float) { return ++calls < 2; });
        CHECK (! done, "denoise honours cancel");
    }

    // ------------------------------------------------------------------ the whole chain
    auto runChain = [&] (std::vector<float> in, float gainLin, const Options& o, Report& rep, bool stereo = true)
    {
        for (auto& v : in) v *= gainLin;
        auto b = toBuffer (in, stereo);
        const bool ok = process (b, sr, o, rep, nullptr);
        return std::make_pair (ok, std::move (b));
    };

    {
        auto speech = clean.x; addNoise (speech, 0.004);
        Options o;
        Report rep;
        const auto t0 = std::chrono::steady_clock::now();
        auto [ok, out] = runChain (speech, 1.f, o, rep);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("      (full chain: %.2f s for %.1f s of audio = %.0fx real time)\n", secs, (double) speech.size() / sr, ((double) speech.size() / sr) / secs);
        CHECK (ok && out.getNumSamples() == (int) speech.size(), "full chain runs and keeps the length");
        CHECK (secs < ((double) speech.size() / sr) / 3.0, "and is at least 3x faster than real time");
        CHECK (rep.before.valid && rep.after.valid, "before and after are measured");
        CHECK (std::abs (rep.after.lufs - (-16.0)) < 0.6, "reaches the -16 LUFS target (got %.2f)", rep.after.lufs);
        CHECK (rep.after.truePeakDb <= -0.9, "true peak stays under the ceiling (%.2f dBTP)", rep.after.truePeakDb);
        CHECK (rep.after.spreadDb < rep.before.spreadDb - 2.0, "loud-to-quiet spread shrinks %.1f -> %.1f dB", rep.before.spreadDb, rep.after.spreadDb);
        CHECK (rep.after.snrDb > rep.before.snrDb + 3.0, "speech-to-noise improves %.1f -> %.1f dB", rep.before.snrDb, rep.after.snrDb);
        bool fin = true; for (int c = 0; c < 2; ++c) for (int i = 0; i < out.getNumSamples(); ++i) fin = fin && std::isfinite (out.getSample (c, i));
        CHECK (fin, "output is finite");

        // quiet words get lifted relative to the loud ones: measure by word
        std::vector<float> o0 (out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples());
        double quiet = 0, loud = 0; int nq = 0, nl = 0;
        for (size_t w = 0; w < clean.words.size(); ++w)
        {
            const double r = rmsRanges (o0, { clean.words[w] }, 1500);
            if (w % 4 == 3) { quiet += r; ++nq; } else if (w % 4 == 0) { loud += r; ++nl; }
        }
        const double cleanQuiet = 0.03, cleanLoud = 0.20;
        const double before = 20.0 * std::log10 (cleanLoud / cleanQuiet), after = 20.0 * std::log10 ((loud / nl) / (quiet / nq));
        CHECK (after < before - 4.0, "the quietest words come up toward the loudest (%.1f dB apart -> %.1f dB)", before, after);
    }

    {
        // streaming: the answer must not depend on how the file is cut into chunks (clipped + noisy, so every stage is busy)
        float pk = 0.f; for (float v : clean.x) pk = std::max (pk, std::abs (v));
        auto noisyIn = clean.x; addNoise (noisyIn, 0.005 / (1.8 / pk));
        auto clipped = hardClip (noisyIn, 1.8f / pk, 0.8f);
        auto b1 = toBuffer (clipped), b2 = toBuffer (clipped), b3 = toBuffer (clipped), b4 = toBuffer (clipped);
        Report r1, r2, r3, r4;
        process (b4, sr, Options(), r4, nullptr, 8192);
        const bool ok1 = process (b1, sr, Options(), r1, nullptr, 1 << 16);
        const bool ok2 = process (b2, sr, Options(), r2, nullptr, 1000);
        const bool ok3 = process (b3, sr, Options(), r3, nullptr, 333);
        double d2 = 0, d3 = 0;
        for (int c = 0; c < 2; ++c) for (int i = 0; i < b1.getNumSamples(); ++i)
        { d2 = std::max (d2, (double) std::abs (b1.getSample (c, i) - b2.getSample (c, i))); d3 = std::max (d3, (double) std::abs (b1.getSample (c, i) - b3.getSample (c, i))); }
        double rmsDiffDb = 0;
        {
            double e = 0, dd = 0; int worst = 0;
            for (int i = 0; i < b1.getNumSamples(); ++i) { const double d = b1.getSample (0, i) - b3.getSample (0, i); e += (double) b1.getSample (0, i) * b1.getSample (0, i); dd += d * d; if (std::abs (d) > std::abs (b1.getSample (0, worst) - b3.getSample (0, worst))) worst = i; }
            double d4 = 0; for (int i = 0; i < b1.getNumSamples(); ++i) d4 = std::max (d4, (double) std::abs (b1.getSample (0, i) - b4.getSample (0, i)));
            rmsDiffDb = 10.0 * std::log10 (e / std::max (dd, 1e-20));
            std::printf ("      (rms diff %.1f dB below the signal, worst sample %d, 8192-chunk max diff %.2g)\n", rmsDiffDb, worst, d4);
        }
        CHECK (ok1 && ok2 && ok3 && rmsDiffDb > 55.0 && d2 < 6e-3 && d3 < 6e-3, "cutting the file into 65536 / 1000 / 333-sample chunks gives the same audio (%.0f dB below the signal, max diff %.2g / %.2g)", rmsDiffDb, d2, d3);
        CHECK (r1.declipped == r2.declipped && r1.declipped == r3.declipped && r1.declipped > 100, "...and the same declip repairs (%lld / %lld / %lld)", r1.declipped, r2.declipped, r3.declipped);
        CHECK (std::abs (r1.after.lufs - r2.after.lufs) < 0.05, "...and the same loudness (%.2f / %.2f)", r1.after.lufs, r2.after.lufs);
    }

    {
        // too quiet and too hot both land on the target
        Options o; Report r1, r2;
        auto speech = clean.x; addNoise (speech, 0.002);
        auto [ok1, q] = runChain (speech, 0.05f, o, r1);
        auto [ok2, h] = runChain (speech, 6.0f, o, r2);
        CHECK (ok1 && std::abs (r1.after.lufs + 16.0) < 0.7, "a very quiet file reaches the target (%.2f LUFS)", r1.after.lufs);
        CHECK (ok2 && std::abs (r2.after.lufs + 16.0) < 0.7, "a file that is far too loud reaches the target (%.2f LUFS)", r2.after.lufs);
        CHECK (r1.after.truePeakDb <= -0.9 && r2.after.truePeakDb <= -0.9, "peaks stay under the ceiling in both (%.2f / %.2f dBTP)", r1.after.truePeakDb, r2.after.truePeakDb);

        Options o2; o2.targetLufs = -19.f; Report r3;
        auto [ok3, m] = runChain (speech, 1.f, o2, r3);
        CHECK (ok3 && std::abs (r3.after.lufs + 19.0) < 0.7, "the -19 LUFS (mono podcast) target works (%.2f)", r3.after.lufs);
        Options o3; o3.ceilingDb = -3.f; Report r4;
        auto [ok4, c3] = runChain (speech, 1.f, o3, r4);
        CHECK (ok4 && r4.after.truePeakDb <= -2.9, "a -3 dBTP ceiling is respected (%.2f)", r4.after.truePeakDb);
    }

    {
        // clipped + noisy + uneven: the whole story
        float pk = 0.f; for (float v : clean.x) pk = std::max (pk, std::abs (v));
        auto noisyIn = clean.x; addNoise (noisyIn, 0.005 / (1.8 / pk));      // noise is already in the signal when the clipper hits
        auto clipped = hardClip (noisyIn, 1.8f / pk, 0.8f);
        Options o; Report rep;
        auto [ok, out] = runChain (clipped, 1.f, o, rep);
        CHECK (ok && rep.before.clipped > 100, "the damaged file is reported as clipped (%lld samples)", rep.before.clipped);
        CHECK (rep.declipped > 100, "declip rebuilt %lld samples", rep.declipped);
        CHECK (rep.after.clipped == 0, "no flat clipped tops remain in the result");
        CHECK (std::abs (rep.after.lufs + 16.0) < 0.7 && rep.after.truePeakDb <= -0.9, "and it still lands at the target (%.2f LUFS, %.2f dBTP)", rep.after.lufs, rep.after.truePeakDb);

        Options off; off.declip = false; Report rep2;
        auto [ok2, out2] = runChain (clipped, 1.f, off, rep2);
        CHECK (ok2 && rep2.declipped == 0, "declip can be switched off");
    }

    {
        // mono fold + one-channel input
        Options o; o.mono = true; Report rep;
        std::vector<float> st = clean.x; addNoise (st, 0.004);
        juce::AudioBuffer<float> b (2, (int) st.size());
        for (size_t i = 0; i < st.size(); ++i) { b.setSample (0, (int) i, st[i]); b.setSample (1, (int) i, st[(i + 7) % st.size()] * 0.5f); }
        const bool ok = process (b, sr, o, rep, nullptr);
        double d = 0; for (int i = 0; i < b.getNumSamples(); ++i) d = std::max (d, (double) std::abs (b.getSample (0, i) - b.getSample (1, i)));
        CHECK (ok && d < 1e-6, "mono option gives identical channels (diff %.2g)", d);

        Options o1; Report rep1;
        auto [okm, mono1] = runChain (st, 1.f, o1, rep1, false);
        CHECK (okm && mono1.getNumChannels() == 1 && std::abs (rep1.after.lufs + 16.0) < 0.7, "a one-channel buffer works (%.2f LUFS)", rep1.after.lufs);

        // refuses silence, honours cancel
        juce::AudioBuffer<float> sil (2, 50000); sil.clear();
        Report rs;
        CHECK (! process (sil, sr, Options(), rs, nullptr), "silence is refused");
        auto b2 = toBuffer (st);
        int n = 0;
        CHECK (! process (b2, sr, Options(), rs, [&] (float) { return ++n < 3; }), "the chain honours cancel");
    }

    {
        // other sample rates
        for (double r : { 48000.0, 96000.0 })
        {
            auto sp = makeSpeech (r, 10); addNoise (sp.x, 0.004);
            auto b = toBuffer (sp.x);
            Report rep;
            const bool ok = process (b, r, Options(), rep, nullptr);
            CHECK (ok && std::abs (rep.after.lufs + 16.0) < 0.7 && rep.after.truePeakDb <= -0.9, "%.0f Hz: %.2f LUFS, %.2f dBTP", r, rep.after.lufs, rep.after.truePeakDb);
        }
    }
}

// -----------------------------------------------------------------------------------------------------------------
static juce::File writeTestVoice (const juce::String& name, double sr, int words, bool clipIt, bool stereo = true, int bits = 24)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_podcast_test");
    dir.createDirectory();
    auto f = dir.getChildFile (name);
    f.deleteFile();
    auto sp = makeSpeech (sr, words);
    addNoise (sp.x, 0.004);
    if (clipIt)
    {
        float pk = 0.f; for (float v : sp.x) pk = std::max (pk, std::abs (v));
        sp.x = hardClip (sp.x, 1.8f / pk, 0.8f);
    }
    juce::AudioBuffer<float> b (stereo ? 2 : 1, (int) sp.x.size());
    for (int c = 0; c < b.getNumChannels(); ++c) b.copyFrom (c, 0, sp.x.data(), (int) sp.x.size());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, (unsigned) b.getNumChannels(), bits, {}, 0));
    if (w) { os.release(); w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples()); }
    return f;
}

static void testPodcastSession()
{
    std::printf ("\n== Podcast Ready: session, playback, export ==\n");
    const double sr = 44100.0;
    const auto in = writeTestVoice ("voice.wav", sr, 16, true);
    AIOProcessor proc;
    proc.setPlayConfigDetails (2, 2, sr, 512);
    proc.prepareToPlay (sr, 512);
    auto& pc = proc.getPodcast();

    CHECK (! pc.info().hasFile, "nothing is loaded at first");
    pc.loadFile (in);
    CHECK (waitFor ([&] { return ! pc.busy() && pc.info().hasFile; }, 20000), "the file loads (%.1f s, %.0f Hz, %d ch)", pc.info().seconds, pc.info().sampleRate, pc.info().channels);
    {
        const auto i = pc.info();
        CHECK (i.before.valid && i.before.clipped > 100 && i.waveform.size() > 100, "measured on load: %.1f LUFS, %.1f dBTP, %lld clipped samples", i.before.lufs, i.before.truePeakDb, i.before.clipped);
    }
    CHECK (! pc.info().hasResult, "no result before running");
    pc.setPlaying (true);
    CHECK (pc.isPlaying(), "the original can be previewed straight after loading");
    pc.setPlaying (false);

    proc.applyPodcastPreset (0);
    CHECK ((int) proc.apvts.getRawParameterValue ("pcTarget")->load() == -16 && proc.apvts.getRawParameterValue ("pcMono")->load() < 0.5f, "preset 'Podcast (stereo, -16)'");
    proc.runPodcast();
    CHECK (pc.busy(), "busy() is true as soon as it starts");
    CHECK (waitFor ([&] { return ! pc.busy(); }, 120000) && pc.state() == aio::PodcastSession::Idle, "processing finishes (%s)", pc.message().toRawUTF8());
    const auto i = pc.info();
    CHECK (i.hasResult && std::abs (i.report.after.lufs + 16.0) < 0.7, "result is at the target: %.2f LUFS", i.report.after.lufs);
    CHECK (i.report.after.truePeakDb <= -0.9, "true peak %.2f dBTP", i.report.after.truePeakDb);
    CHECK (i.report.declipped > 100 && i.report.after.clipped == 0, "clipping repaired (%lld samples rebuilt, %lld left flat)", i.report.declipped, i.report.after.clipped);
    CHECK (i.waveformAfter.size() == i.waveform.size(), "both waveforms are available");

    // ---- audition the rendered result through processBlock (the LIVE preview has its own tests below)
    pc.setLive (false);
    auto play = [&] (bool original, bool match, double& rms, float& peak)
    {
        pc.abOriginal = original; pc.matchVolume = match; pc.loop = true;
        pc.seek (0.1); pc.setPlaying (true);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
        double e = 0; size_t cnt = 0; peak = 0.f; bool fin = true;
        for (int b = 0; b < 300; ++b)
        {
            waitFor ([&] { return pc.bufferedMs() >= 30; }, 3000);      // the streaming player fills in the background
            buf.clear(); proc.processBlock (buf, m);
            for (int c = 0; c < 2; ++c) for (int k = 0; k < 512; ++k) { const float v = buf.getSample (c, k); fin = fin && std::isfinite (v); e += (double) v * v; ++cnt; peak = std::max (peak, std::abs (v)); }
        }
        pc.setPlaying (false);
        rms = std::sqrt (e / (double) cnt);
        return fin;
    };
    double rmsP = 0, rmsO = 0, rmsOm = 0; float pkP = 0, pkO = 0, pkOm = 0;
    proc.setParam ("limOn", 1.f); proc.setParam ("limCeil", -20.f);      // if the chain were NOT bypassed this would crush the preview
    CHECK (play (false, true, rmsP, pkP), "podcast-ready preview is finite");
    CHECK (rmsP > 0.01 && pkP > 0.3f && pkP < 1.0f, "...and audible (rms %.3f, peak %.2f)", rmsP, pkP);
    CHECK (pkP > std::pow (10.f, -10.f / 20.f), "...and the live chain is bypassed (peak %.2f although the limiter ceiling is -20 dB)", pkP);
    play (true, false, rmsO, pkO);
    play (true, true, rmsOm, pkOm);
    CHECK (std::abs (rmsOm / rmsO - (double) pc.matchGain()) < 0.05 * pc.matchGain(), "MATCH VOLUME applies the loudness difference (x%.2f)", pc.matchGain());
    CHECK (std::abs (20.0 * std::log10 (rmsOm / rmsP)) < 5.0, "...which brings the original within %.1f dB of the result", 20.0 * std::log10 (rmsOm / rmsP));
    CHECK (pkOm <= 1.0f, "...without clipping (peak %.2f)", pkOm);
    proc.setParam ("limOn", 0.f);

    // master playback wins over podcast playback, and starting one stops the other from the UI side
    CHECK (! pc.isPlaying(), "preview stops cleanly");

    // ---- exports
    auto dir = in.getParentDirectory();
    auto readBack = [&] (const juce::File& f, int& ch, int& bits, juce::AudioBuffer<float>& b)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
        if (rd == nullptr) return false;
        ch = (int) rd->numChannels; bits = (int) rd->bitsPerSample;
        b.setSize (ch, (int) rd->lengthInSamples);
        rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
        return true;
    };
    {
        auto out = dir.getChildFile ("ready24.wav"); out.deleteFile();
        proc.exportPodcast (out, 24);
        juce::String msg; bool done = false;
        waitFor ([&] { if (pc.busy()) return false; done = pc.takeExportDone (msg); return done; }, 60000);
        int ch = 0, bits = 0; juce::AudioBuffer<float> b;
        CHECK (done && readBack (out, ch, bits, b) && ch == 2 && bits == 24, "24-bit stereo WAV written (%s)", msg.toRawUTF8());
        const auto st = aio::podcast::measure (b.getReadPointer (0), b.getReadPointer (1), (size_t) b.getNumSamples(), sr);
        CHECK (std::abs (st.lufs + 16.0) < 0.7 && st.truePeakDb <= -0.8, "re-measuring the file: %.2f LUFS, %.2f dBTP", st.lufs, st.truePeakDb);
        CHECK (std::abs (b.getNumSamples() - (int) (i.seconds * sr)) < 4, "same length as the source");
    }
    {
        auto out = dir.getChildFile ("ready16.wav"); out.deleteFile();
        proc.exportPodcast (out, 16);
        waitFor ([&] { return ! pc.busy(); }, 60000);
        int ch = 0, bits = 0; juce::AudioBuffer<float> b;
        CHECK (readBack (out, ch, bits, b) && bits == 16, "16-bit WAV written");
        const auto st = aio::podcast::measure (b.getReadPointer (0), b.getReadPointer (ch > 1 ? 1 : 0), (size_t) b.getNumSamples(), sr);
        CHECK (st.truePeakDb <= -0.6, "16-bit true peak %.2f dBTP (dither + rounding stay under the ceiling)", st.truePeakDb);
    }
    {
        auto out = dir.getChildFile ("ready.flac"); out.deleteFile();
        proc.exportPodcast (out, 24);
        waitFor ([&] { return ! pc.busy(); }, 60000);
        int ch = 0, bits = 0; juce::AudioBuffer<float> b;
        CHECK (readBack (out, ch, bits, b) && bits == 24 && ch == 2, "FLAC 24-bit written (%d ch)", ch);
    }
    {
        // mono output
        proc.setParam ("pcMono", 1.f); proc.setParam ("pcTarget", -19.f);
        proc.runPodcast();
        waitFor ([&] { return ! pc.busy(); }, 120000);
        CHECK (std::abs (pc.info().report.after.lufs + 19.0) < 0.7, "mono run at -19 LUFS (%.2f)", pc.info().report.after.lufs);
        auto out = dir.getChildFile ("ready_mono.wav"); out.deleteFile();
        proc.exportPodcast (out, 24);
        waitFor ([&] { return ! pc.busy(); }, 60000);
        int ch = 0, bits = 0; juce::AudioBuffer<float> b;
        CHECK (readBack (out, ch, bits, b) && ch == 1, "the mono option writes a one-channel file (%d ch)", ch);
        proc.setParam ("pcMono", 0.f); proc.setParam ("pcTarget", -16.f);
    }

    // ---- mono source + a different host sample rate
    {
        const auto mono = writeTestVoice ("mono.wav", 48000.0, 10, false, false, 16);
        AIOProcessor p2; p2.setPlayConfigDetails (2, 2, 44100.0, 512); p2.prepareToPlay (44100.0, 512);
        auto& q = p2.getPodcast();
        q.loadFile (mono);
        CHECK (waitFor ([&] { return ! q.busy() && q.info().hasFile; }, 20000) && q.info().channels == 1, "a mono 48 kHz file loads (%d ch)", q.info().channels);
        p2.runPodcast();
        waitFor ([&] { return ! q.busy(); }, 120000);
        CHECK (q.info().hasResult && std::abs (q.info().report.after.lufs + 16.0) < 0.7, "mono source reaches the target (%.2f LUFS)", q.info().report.after.lufs);
        q.abOriginal = false; q.seek (0.0); q.setPlaying (true);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
        double e = 0, d = 0; int secs = 0;
        for (int b = 0; b < 100; ++b) { waitFor ([&] { return q.bufferedMs() >= 30; }, 3000); buf.clear(); p2.processBlock (buf, m); for (int k = 0; k < 512; ++k) { e += buf.getSample (0, k) * buf.getSample (0, k); d += std::abs (buf.getSample (0, k) - buf.getSample (1, k)); } ++secs; }
        q.setPlaying (false);
        CHECK (e > 1.0 && d < 1e-3, "preview resamples 48 kHz -> 44.1 kHz and plays on both channels (energy %.1f)", e);
        const double pos0 = q.position();
        q.seek (0.5);
        CHECK (std::abs (q.position() - 0.5) < 0.01 && pos0 >= 0.0, "seeking works (%.2f)", q.position());
    }

    // ---- no clicks in the preview when the file and the host run at different sample rates
    {
        for (const double fileSr : { 44100.0, 48000.0, 96000.0, 22050.0 })
        {
            auto dir2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_podcast_test");
            auto sine = dir2.getChildFile ("sine.wav"); sine.deleteFile();
            {
                juce::AudioBuffer<float> b (1, (int) (fileSr * 20.0));
                for (int k = 0; k < b.getNumSamples(); ++k) b.setSample (0, k, 0.5f * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * k / fileSr) * (0.6f + 0.4f * std::sin (k * 3.0e-5f)));
                juce::WavAudioFormat wav; std::unique_ptr<juce::FileOutputStream> os (sine.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), fileSr, 1, 24, {}, 0)); os.release();
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            }
            AIOProcessor p5; p5.setPlayConfigDetails (2, 2, 44100.0, 512); p5.prepareToPlay (44100.0, 512);
            auto& q = p5.getPodcast();
            q.loadFile (sine);
            waitFor ([&] { return ! q.busy() && q.info().hasFile; }, 20000);
            auto roughness = [&] (bool original)
            {
                q.abOriginal = original; q.matchVolume = false; q.loop = true; q.seek (0.0); q.setPlaying (true);
                std::vector<float> out; juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
                for (int b = 0; b < 400; ++b) { waitFor ([&] { return q.bufferedMs() >= 30; }, 3000); buf.clear(); if (getenv ("DIRECT")) q.readPlayback (buf.getWritePointer (0), buf.getWritePointer (1), 512); else p5.processBlock (buf, m); for (int k = 0; k < 512; ++k) out.push_back (buf.getSample (0, k)); }
                q.setPlaying (false);
                double e2 = 0, mx = 0; size_t cnt = 0;
                for (size_t k = 2048; k + 1 < out.size(); ++k) { const double d = (double) out[k + 1] - 2.0 * out[k] + out[k - 1]; e2 += d * d; mx = std::max (mx, std::abs (d)); ++cnt; if (std::abs (d) > 0.05 && getenv ("DBGCLICK")) std::printf ("  click at %zu (%.4f) vals %.3f %.3f %.3f\n", k, d, out[k-1], out[k], out[k+1]); }
                return mx / std::max (1e-12, std::sqrt (e2 / (double) cnt));
            };
            const double rOrig = roughness (true);
            CHECK (rOrig < 3.0, "%.0f Hz file on a 44.1 kHz host: the original plays without clicks (peak/rms of the 2nd difference %.2f, a smooth tone is 1.4)", fileSr, rOrig);
            p5.runPodcast();
            waitFor ([&] { return ! q.busy(); }, 60000);
            const double rLive = roughness (false);
            CHECK (rLive < 6.0, "...and so does the LIVE podcast-ready preview (%.2f)", rLive);
            q.setLive (false);
            const double rProc = roughness (false);
            CHECK (rProc < 6.0, "...and so does the rendered result (%.2f)", rProc);
        }
    }

    // ---- temporary files are cleaned up
    {
        auto countTemp = [] { return juce::File::getSpecialLocation (juce::File::tempDirectory).findChildFiles (juce::File::findDirectories, false, "AIOStudioPodcast_*").size(); };
        const int before = countTemp();
        {
            AIOProcessor p4; p4.setPlayConfigDetails (2, 2, sr, 512); p4.prepareToPlay (sr, 512);
            p4.getPodcast().loadFile (in);
            waitFor ([&] { return ! p4.getPodcast().busy(); }, 20000);
            p4.runPodcast();
            waitFor ([&] { return ! p4.getPodcast().busy(); }, 120000);
            CHECK (countTemp() == before + 1, "the result lives in one temporary folder while it is needed");
            p4.runPodcast();
            waitFor ([&] { return ! p4.getPodcast().busy(); }, 120000);
            CHECK (countTemp() == before + 1, "running again replaces it instead of piling up");
        }
        CHECK (countTemp() == before, "closing the plug-in removes the temporary files");
    }

    // ---- bad input
    {
        auto bad = dir.getChildFile ("notaudio.wav"); bad.replaceWithText ("this is not a wav file at all");
        AIOProcessor p3; p3.setPlayConfigDetails (2, 2, sr, 512); p3.prepareToPlay (sr, 512);
        p3.getPodcast().loadFile (bad);
        waitFor ([&] { return ! p3.getPodcast().busy(); }, 10000);
        CHECK (p3.getPodcast().state() == aio::PodcastSession::Failed && ! p3.getPodcast().info().hasFile, "garbage is refused: \"%s\"", p3.getPodcast().message().toRawUTF8());
        p3.runPodcast();
        waitFor ([&] { return ! p3.getPodcast().busy(); }, 10000);
        CHECK (p3.getPodcast().state() == aio::PodcastSession::Failed, "running with nothing loaded fails politely (\"%s\")", p3.getPodcast().message().toRawUTF8());

        juce::AudioBuffer<float> z (2, 40000); z.clear();
        auto silent = dir.getChildFile ("silent.wav"); silent.deleteFile();
        { juce::WavAudioFormat wav; std::unique_ptr<juce::FileOutputStream> os (silent.createOutputStream());
          std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 16, {}, 0)); if (w) { os.release(); w->writeFromAudioSampleBuffer (z, 0, z.getNumSamples()); } }
        p3.getPodcast().loadFile (silent);
        waitFor ([&] { return ! p3.getPodcast().busy(); }, 10000);
        CHECK (p3.getPodcast().state() == aio::PodcastSession::Failed, "a silent file is refused: \"%s\"", p3.getPodcast().message().toRawUTF8());

        // cancel mid-run
        AIOProcessor p4; p4.setPlayConfigDetails (2, 2, sr, 512); p4.prepareToPlay (sr, 512);
        p4.getPodcast().loadFile (in);
        waitFor ([&] { return ! p4.getPodcast().busy() && p4.getPodcast().info().hasFile; }, 20000);
        p4.runPodcast();
        p4.getPodcast().cancel();
        CHECK (waitFor ([&] { return ! p4.getPodcast().busy(); }, 60000), "cancel stops the job");
        CHECK (p4.getPodcast().state() != aio::PodcastSession::Processing, "...and leaves the session usable");
    }

    // ---- parameters
    {
        AIOProcessor p5;
        for (int k = 0; k < AIOProcessor::podcastPresetCount(); ++k)
        {
            p5.applyPodcastPreset (k);
            const auto o = p5.readPodcastOptions();
            CHECK (o.targetLufs >= -24.f && o.targetLufs <= -12.f && o.ceilingDb < 0.f && o.denoise >= 0.f && o.denoise <= 1.f, "preset %d '%s': %.0f LUFS, %.1f dBTP, noise %.0f %%", k, AIOProcessor::podcastPresetName (k), o.targetLufs, o.ceilingDb, o.denoise * 100.f);
        }
        p5.setParam ("pcDenoise", 80.f);
        p5.setCurrentProgram (3);
        CHECK (std::abs (p5.apvts.getRawParameterValue ("pcDenoise")->load() - 80.f) < 0.01f, "factory presets leave the podcast settings alone");
        juce::MemoryBlock mb; p5.getStateInformation (mb);
        AIOProcessor p6; p6.setStateInformation (mb.getData(), (int) mb.getSize());
        CHECK (std::abs (p6.apvts.getRawParameterValue ("pcDenoise")->load() - 80.f) < 0.01f, "podcast settings are saved with the project");
    }
}

// -----------------------------------------------------------------------------------------------------------------
// Regression: the processing itself must not add clicks (the denoiser once handed its FFT the wrong data layout, which put a step
// at every 512-sample hop, and the tone EQ jumped at the end of every long block).
static void testPodcastSmooth()
{
    std::printf ("\n== Podcast Ready: no clicks from the processing ==\n");
    using namespace aio::podcast;
    const double sr = 44100.0;
    std::vector<float> x ((size_t) (sr * 20));
    for (size_t k = 0; k < x.size(); ++k) x[k] = 0.5f * std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * (double) k / sr) * (0.6f + 0.4f * std::sin ((float) k * 3.0e-5f));
    auto rough = [] (const std::vector<float>& y)
    {
        double e2 = 0, mx = 0; size_t cnt = 0;
        for (size_t k = 4096; k + 1 < y.size(); ++k) { const double d = (double) y[k + 1] - 2.0 * y[k] + y[k - 1]; e2 += d * d; mx = std::max (mx, std::abs (d)); ++cnt; }
        return mx / std::sqrt (e2 / (double) cnt);
    };
    const double r0 = rough (x);
    auto base = [] { Options o; o.declip = false; o.denoise = 0; o.level = 0; o.tone = 0; o.deess = 0; o.comp = 0; return o; };
    struct Case { const char* name; Options o; };
    std::vector<Case> cases;
    { auto o = base(); o.denoise = 0.5f; cases.push_back ({ "noise reduction", o }); }
    { auto o = base(); o.denoise = 1.0f; cases.push_back ({ "noise reduction (100 %)", o }); }
    { auto o = base(); o.tone = 0.7f; cases.push_back ({ "clear tone", o }); }
    { auto o = base(); o.level = 1.f; o.comp = 0.7f; cases.push_back ({ "leveler + compression", o }); }
    cases.push_back ({ "all defaults", Options() });
    for (auto& c : cases)
    {
        auto b = toBuffer (x); Report rep; process (b, sr, c.o, rep, nullptr);
        std::vector<float> y (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        const double r = rough (y);
        CHECK (r < 6.0, "%s keeps a steady tone smooth (2nd-difference peak/rms %.1f, the input has %.1f)", c.name, r, r0);
    }
    // speech: no sample-to-sample step stands out from its surroundings more than in the input
    {
        auto sp = makeSpeech (sr, 16); addNoise (sp.x, 0.004);
        auto count = [] (const std::vector<float>& y)
        {
            int n = 0;
            for (size_t k = 400; k + 400 < y.size(); ++k)
            {
                const double d = (double) y[k + 1] - 2.0 * y[k] + y[k - 1];
                double e = 0; for (size_t j = k - 200; j < k + 200; j += 4) { const double dd = (double) y[j + 1] - 2.0 * y[j] + y[j - 1]; e += dd * dd; }
                if (std::abs (d) > 12.0 * (std::sqrt (e / 100.0) + 1e-9) && std::abs (d) > 0.004) ++n;
            }
            return n;
        };
        auto b = toBuffer (sp.x); Report rep; process (b, sr, Options(), rep, nullptr);
        std::vector<float> y (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples());
        CHECK (count (y) <= count (sp.x) + 2, "speech through the default chain has no stray clicks (%d, input %d)", count (y), count (sp.x));
    }
    // the live chain is cheap enough to run on the fly
    {
        auto sp = makeSpeech (sr, 40); addNoise (sp.x, 0.004);
        auto an = std::make_shared<Analysis> (analyse ([&] (float* l, float* r, long long pos, int n) { std::copy (sp.x.begin() + pos, sp.x.begin() + pos + n, l); std::copy (sp.x.begin() + pos, sp.x.begin() + pos + n, r); return true; },
                                                       (long long) sp.x.size(), sr, nullptr));
        LiveChain lc (an, Options(), sr, false, 0.0);
        std::vector<float> oL, oR;
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        for (size_t pos = 0; pos + 2048 <= sp.x.size(); pos += 2048) lc.push (sp.x.data() + pos, sp.x.data() + pos, 2048, oL, oR);
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const double audio = (double) oL.size() / sr;
        CHECK (audio / secs > 8.0, "the live chain runs %.0fx faster than real time (it needs to be well above 1)", audio / secs);
    }
}

// -----------------------------------------------------------------------------------------------------------------
static void testPodcastLive()
{
    std::printf ("\n== Podcast Ready: live preview (knobs act while it plays) ==\n");
    const double sr = 44100.0;
    const auto in = writeTestVoice ("live_voice.wav", sr, 26, true);
    AIOProcessor proc; proc.setPlayConfigDetails (2, 2, sr, 512); proc.prepareToPlay (sr, 512);
    auto& pc = proc.getPodcast();
    proc.applyPodcastPreset (0);
    CHECK (pc.live() && ! pc.liveActive(), "LIVE is on by default, but nothing is loaded yet");
    pc.loadFile (in);
    waitFor ([&] { return ! pc.busy() && pc.info().hasFile; }, 20000);
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
    // plays in real time and returns what came out
    auto capture = [&] (double seconds, std::vector<float>& L, std::vector<float>& R)
    {
        const int blocks = (int) (seconds * sr / 512.0);
        for (int b = 0; b < blocks; ++b)
        {
            const int ub = pc.underruns();
            buf.clear(); proc.processBlock (buf, m);
            if (pc.underruns() != ub && getenv ("DBGUR")) std::printf ("      underrun at block %d of %d (buffered %d ms, pos %.3f)\n", b, blocks, pc.bufferedMs(), pc.position());
            for (int k = 0; k < 512; ++k) { L.push_back (buf.getSample (0, k)); R.push_back (buf.getSample (1, k)); }
            juce::Thread::sleep (11);
        }
    };
    auto lufsOf = [&] (const std::vector<float>& L, const std::vector<float>& R, size_t a, size_t b)
    {
        return aio::podcast::measure (L.data() + a, R.data() + a, b - a, sr).lufs;
    };
    pc.abOriginal = false; pc.loop = true; pc.seek (0.0);
    pc.setPlaying (true);
    CHECK (waitFor ([&] { return pc.liveActive() && pc.bufferedMs() > 100; }, 5000), "the live preview is ready without pressing MAKE PODCAST READY (%d ms buffered)", pc.bufferedMs());
    std::vector<float> L, R;
    const int u0 = pc.underruns();
    capture (4.0, L, R);
    const double l1 = lufsOf (L, R, (size_t) (1.0 * sr), L.size());
    CHECK (std::abs (l1 + 16.0) < 3.0, "live preview sits near the target loudness (%.1f LUFS for -16)", l1);
    CHECK (pc.underruns() == u0, "...and plays without a single dropout (%d)", pc.underruns() - u0);

    // ---- move the target knob while it plays: -16 -> -24 LUFS. It should follow within a second or two
    proc.setParam ("pcTarget", -24.f);
    const size_t at = L.size();
    capture (5.0, L, R);
    const double early = lufsOf (L, R, at, at + (size_t) (0.4 * sr));
    const double late = lufsOf (L, R, L.size() - (size_t) (1.5 * sr), L.size());
    CHECK (late < l1 - 5.0 && std::abs (late + 24.0) < 3.0, "turning the target down is heard while playing: %.1f -> %.1f LUFS (target -24)", l1, late);
    CHECK (pc.underruns() == u0, "...with no dropout while the chain was rebuilt (%d)", pc.underruns() - u0);
    (void) early;

    // ---- smoothness across the change: a sine goes through the same switch without a click
    {
        auto dir2 = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_podcast_test");
        auto sine = dir2.getChildFile ("live_sine.wav"); sine.deleteFile();
        {
            juce::AudioBuffer<float> b (1, (int) (sr * 30.0));
            for (int k = 0; k < b.getNumSamples(); ++k) b.setSample (0, k, 0.4f * std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * k / sr));
            juce::WavAudioFormat wav; std::unique_ptr<juce::FileOutputStream> os (sine.createOutputStream());
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 1, 24, {}, 0)); os.release();
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        pc.setPlaying (false);
        proc.setParam ("pcTarget", -16.f); proc.setParam ("pcDenoise", 0.f); proc.setParam ("pcTone", 0.f); proc.setParam ("pcComp", 0.f); proc.setParam ("pcLevel", 0.f);
        pc.loadFile (sine);
        waitFor ([&] { return ! pc.busy() && pc.info().hasFile; }, 20000);
        pc.abOriginal = false; pc.seek (0.0); pc.setPlaying (true);
        const bool okw = waitFor ([&] { return pc.liveActive() && pc.bufferedMs() > 100; }, 5000);
        if (getenv ("DBGUR")) std::printf ("      sine wait ok=%d live=%d buffered=%d state=%d msg=%s\n", (int) okw, (int) pc.liveActive(), pc.bufferedMs(), (int) pc.state(), pc.message().toRawUTF8());
        std::vector<float> a, b2;
        capture (2.0, a, b2);
        std::printf ("      underruns after steady 2 s: %d\n", pc.underruns() - u0);
        proc.setParam ("pcLevel", 100.f); proc.setParam ("pcComp", 70.f);        // chain rebuilt and cross-faded
        capture (1.0, a, b2);
        std::printf ("      underruns after chain swap: %d\n", pc.underruns() - u0);
        proc.setParam ("pcTarget", -20.f);
        capture (3.0, a, b2);
        pc.setPlaying (false);
        double e2 = 0, mx = 0; size_t cnt = 0, at2 = 0;
        for (size_t k = 4096; k + 1 < a.size(); ++k) { const double d = (double) a[k + 1] - 2.0 * a[k] + a[k - 1]; e2 += d * d; if (std::abs (d) > mx) { mx = std::abs (d); at2 = k; } ++cnt; }
        const double rough = mx / std::max (1e-12, std::sqrt (e2 / (double) cnt));
        CHECK (rough < 12.0, "no click when the chain is swapped under a steady tone (peak/rms of the 2nd difference %.1f at %.2f s)", rough, (double) at2 / sr);
        CHECK (pc.underruns() == u0, "...and still no dropouts (%d)", pc.underruns() - u0);
        proc.setParam ("pcDenoise", 55.f); proc.setParam ("pcTone", 70.f); proc.setParam ("pcLevel", 100.f); proc.setParam ("pcComp", 70.f); proc.setParam ("pcTarget", -16.f);
    }

    // ---- live vs the rendered result: similar loudness
    {
        pc.loadFile (in);
        waitFor ([&] { return ! pc.busy() && pc.info().hasFile; }, 20000);
        proc.runPodcast();
        waitFor ([&] { return ! pc.busy(); }, 120000);
        std::vector<float> a1, b1, a2, b2;
        pc.abOriginal = false; pc.loop = true;
        pc.setLive (true);  pc.seek (0.2); pc.setPlaying (true); waitFor ([&] { return pc.bufferedMs() > 100; }, 5000); capture (4.0, a1, b1); pc.setPlaying (false);
        pc.setLive (false); pc.seek (0.2); pc.setPlaying (true); waitFor ([&] { return pc.bufferedMs() > 100; }, 5000); capture (4.0, a2, b2); pc.setPlaying (false);
        const double ll = lufsOf (a1, b1, (size_t) sr, a1.size()), lr = lufsOf (a2, b2, (size_t) sr, a2.size());
        CHECK (std::abs (ll - lr) < 3.0, "the live preview and the final render are within 3 dB of each other (%.1f vs %.1f LUFS)", ll, lr);
        pc.setLive (true);
    }
}

namespace
{
struct Turn { int voice; size_t a, b; };
struct Conversation { std::vector<float> x; std::vector<Turn> turns; };

struct VoiceSpec { double f0; std::vector<int> vowels; double level; double tiltHz; };     // tiltHz > 0: a one-pole low-pass (a duller recording of that person)

// people taking turns: each turn is a few words in that person's pitch / vowel colour, separated by a pause of room noise
Conversation makeConversation (double sr, const std::vector<VoiceSpec>& voices, const std::vector<int>& order, int wordsPerTurn = 4, unsigned seed = 11)
{
    Conversation c;
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (0.f, 1.f);
    for (int vi : order)
    {
        const auto& vs = voices[(size_t) vi];
        const size_t a = c.x.size();
        std::vector<float> turn;
        for (int w = 0; w < wordsPerTurn; ++w)
        {
            const double f0 = vs.f0 * (0.97 + 0.06 * u (rng));
            const int len = 3 + (int) (u (rng) * 2.f);
            std::vector<int> v;
            for (int k = 0; k < len; ++k) v.push_back (vs.vowels[(size_t) (u (rng) * (float) vs.vowels.size()) % vs.vowels.size()]);
            auto word = vowelVoice (sr, f0, v, 0.15, vs.level);
            const size_t fade = (size_t) (0.02 * sr);
            for (size_t i = 0; i < fade && i < word.size(); ++i) { const float g = (float) i / (float) fade; word[i] *= g; word[word.size() - 1 - i] *= g; }
            turn.insert (turn.end(), word.begin(), word.end());
            turn.insert (turn.end(), (size_t) ((0.12 + 0.08 * u (rng)) * sr), 0.f);
        }
        if (vs.tiltHz > 0.0)
        {
            const float a1 = (float) (1.0 - std::exp (-2.0 * 3.14159265 * vs.tiltHz / sr)); float y = 0.f;
            for (auto& s : turn) { y += a1 * (s - y); s = y; }
        }
        c.x.insert (c.x.end(), turn.begin(), turn.end());
        c.turns.push_back ({ vi, a, c.x.size() });
        c.x.insert (c.x.end(), (size_t) (0.9 * sr), 0.f);              // the pause before the next person
    }
    addNoise (c.x, 0.002, (unsigned) seed);
    return c;
}

// how well the found voices match who really spoke (best of all relabelings)
double voiceAccuracy (const aio::podcast::SpeakerMap& m, const Conversation& c, int nTrue)
{
    const int K = std::max (m.count, nTrue);
    std::vector<std::vector<double>> conf ((size_t) nTrue, std::vector<double> ((size_t) K, 0.0));
    double total = 0.0;
    for (size_t i = 0; i < m.chunks.size(); ++i)
    {
        const long long mid = (m.chunks[i].a + m.chunks[i].b) / 2;
        for (const auto& t : c.turns)
            if ((long long) t.a <= mid && mid < (long long) t.b) { conf[(size_t) t.voice][(size_t) m.label[i]] += m.chunks[i].seconds; total += m.chunks[i].seconds; }
    }
    // greedy matching is enough for up to 4 voices: try every assignment of found -> true
    std::vector<int> perm ((size_t) K);
    std::iota (perm.begin(), perm.end(), 0);
    double best = 0.0;
    do
    {
        double ok = 0.0;
        for (int t = 0; t < nTrue; ++t) ok += conf[(size_t) t][(size_t) perm[(size_t) t]];
        best = std::max (best, ok);
    } while (std::next_permutation (perm.begin(), perm.end()));
    return total > 0.0 ? best / total : 0.0;
}

aio::podcast::ReadFn readOf (const std::vector<float>& x)
{
    return [&x] (float* l, float* r, long long pos, int n)
    {
        for (int i = 0; i < n; ++i) { const size_t k = (size_t) pos + (size_t) i; l[i] = r[i] = k < x.size() ? x[k] : 0.f; }
        return true;
    };
}

double goertzelDb (const std::vector<float>& x, double sr, double f, size_t from, size_t to)
{
    double re = 0, im = 0;
    for (size_t i = from; i < to && i < x.size(); ++i) { const double ph = 2.0 * 3.14159265358979 * f * (double) i / sr; re += x[i] * std::cos (ph); im -= x[i] * std::sin (ph); }
    const double n = (double) (to - from);
    return 20.0 * std::log10 (std::sqrt (re * re + im * im) * 2.0 / n + 1.0e-12);
}

std::vector<float> channel0 (const juce::AudioBuffer<float>& b) { return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + b.getNumSamples()); }

double turnsRms (const std::vector<float>& y, const Conversation& c, int voice)
{
    std::vector<std::pair<size_t, size_t>> rs;
    for (const auto& t : c.turns) if (t.voice == voice) rs.push_back ({ t.a, t.b });
    return rmsRanges (y, rs, 1000);
}
} // namespace

static void testPodcastUI()
{
    std::printf ("\n== Podcast Ready: screen ==\n");
#if ! JucePlugin_IsSynth
    const double sr = 44100.0;
    AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
    AIOEditor ed (p);
    CHECK (ed.podcastTabIndex() > ed.morphTabIndex(), "the PODCAST tab exists (tab %d)", ed.podcastTabIndex());
    ed.showTab (ed.podcastTabIndex());
    juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
    CHECK (p.getPodcast().info().hasFile == false, "the screen starts empty");

    // a ceremony: three people on one track
    const auto talk = makeConversation (sr, { VoiceSpec { 105.0, { 2, 0 }, 0.10, 0.0 }, VoiceSpec { 190.0, { 0, 1 }, 0.10, 0.0 }, VoiceSpec { 270.0, { 3, 4 }, 0.05, 0.0 } },
                                        { 1, 0, 1, 2, 1, 2, 0, 1, 2, 1, 1, 0, 2, 1, 0, 1, 2, 1, 2, 1 }, 4, 8);
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_podcast_test").getChildFile ("ui.wav");
    f.getParentDirectory().createDirectory(); f.deleteFile();
    {
        auto b = toBuffer (talk.x, true);
        juce::WavAudioFormat wav; std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0)); os.release();
        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }
    p.getPodcast().loadFile (f);
    waitFor ([&] { juce::MessageManager::getInstance()->runDispatchLoopUntil (20); return ! p.getPodcast().busy() && p.getPodcast().info().hasFile; }, 20000);
    CHECK (p.getPodcast().info().voiceCount >= 2, "the voices show on the screen (%d found)", p.getPodcast().info().voiceCount);
    p.applyPodcastPreset (7);
    p.setParam ("pcSpkCount", 3.f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    CHECK (p.getPodcast().info().voiceCount == 3, "choosing three voices in the box regroups the recording (%d)", p.getPodcast().info().voiceCount);
    p.runPodcast();
    waitFor ([&] { juce::MessageManager::getInstance()->runDispatchLoopUntil (20); return ! p.getPodcast().busy(); }, 120000);
    p.getPodcast().seek (0.3);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    CHECK (p.getPodcast().info().hasResult, "a processed file is on the screen");

    auto img = ed.createComponentSnapshot (ed.getLocalBounds(), true, 1.0f);
    CHECK (img.getWidth() > 900 && img.getHeight() > 600, "the screen renders (%dx%d)", img.getWidth(), img.getHeight());
    auto dir = juce::File ("/home/claude/snaps"); dir.createDirectory();
    juce::PNGImageFormat png;
    auto out = dir.getChildFile ("podcast_ready.png"); out.deleteFile();
    if (auto os = out.createOutputStream()) png.writeImageToStream (img, *os);
    std::printf ("wrote %s\n", out.getFullPathName().toRawUTF8());
#endif
}

// -----------------------------------------------------------------------------------------------------------------
// Long-file streaming test (run with:  AIOTests --podcastlong [minutes])   -  not part of the normal suite because of its run time
static long peakRssMB()
{
    long kb = 0;
    juce::StringArray lines; lines.addLines (juce::File ("/proc/self/status").loadFileAsString());
    for (auto& l : lines) if (l.startsWith ("VmHWM:")) kb = l.fromFirstOccurrenceOf (":", false, false).trim().getIntValue();
    return kb / 1024;
}

static void testPodcastLong (double minutes)
{
    std::printf ("\n== Podcast Ready: %.0f minute file, streamed from disk ==\n", minutes);
    const double sr = 48000.0;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_podcast_long");
    dir.createDirectory();
    auto f = dir.getChildFile ("long.wav"); f.deleteFile();
    {
        auto sp = makeSpeech (sr, 40);
        addNoise (sp.x, 0.004);
        float pk = 0.f; for (float v : sp.x) pk = std::max (pk, std::abs (v));
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), sr, 2, 24, {}, 0));
        os.release();
        const long long total = (long long) (minutes * 60.0 * sr);
        long long done = 0; int piece = 0;
        juce::AudioBuffer<float> b (2, (int) sp.x.size());
        while (done < total)
        {
            // every piece is recorded at a different level (as a real recording is) but clips at the same digital ceiling
            const float lvl = 0.6f + 0.4f * (float) ((piece * 37) % 11) / 10.f;
            const auto clipped = hardClip (sp.x, 1.8f * lvl / pk, 0.8f);
            for (int c = 0; c < 2; ++c) for (size_t i = 0; i < clipped.size(); ++i) b.setSample (c, (int) i, clipped[i]);
            const int n = (int) std::min<long long> ((long long) sp.x.size(), total - done);
            w->writeFromAudioSampleBuffer (b, 0, n);
            done += n; ++piece;
        }
    }
    std::printf ("      test file: %.2f GB\n", (double) f.getSize() / 1.0e9);
    const long rssBefore = peakRssMB();

    AIOProcessor proc; proc.setPlayConfigDetails (2, 2, 44100.0, 512); proc.prepareToPlay (44100.0, 512);
    auto& pc = proc.getPodcast();
    const double t0 = juce::Time::getMillisecondCounterHiRes();
    pc.loadFile (f);
    CHECK (waitFor ([&] { return ! pc.busy(); }, 3600000) && pc.info().hasFile, "the file loads and is analysed (%.1f min, %.0f s)", pc.info().seconds / 60.0, (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0);
    const double t1 = juce::Time::getMillisecondCounterHiRes();
    proc.applyPodcastPreset (0);
    proc.runPodcast();
    CHECK (waitFor ([&] { return ! pc.busy(); }, 7200000) && pc.state() == aio::PodcastSession::Idle, "processing finishes in %.0f s (%.0fx real time)", (juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0, pc.info().seconds / ((juce::Time::getMillisecondCounterHiRes() - t1) / 1000.0));
    const auto i = pc.info();
    CHECK (i.hasResult && std::abs (i.report.after.lufs + 16.0) < 0.7 && i.report.after.truePeakDb <= -0.9, "whole file at the target: %.2f LUFS, %.2f dBTP", i.report.after.lufs, i.report.after.truePeakDb);
    CHECK (i.report.declipped > 1000 && i.report.after.clipped * 500 < i.report.declipped, "clipping repaired across the whole file (%lld rebuilt, %lld left flat)", i.report.declipped, i.report.after.clipped);
    const long rssAfter = peakRssMB();
    CHECK (rssAfter < 1500, "memory stays small: peak %ld MB (was %ld MB before)", rssAfter, rssBefore);

    // preview near the end, then play past the end without looping
    pc.abOriginal = false; pc.loop = false; pc.seek (0.9995); pc.setPlaying (true);
    juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
    double e = 0; int blocks = 0;
    for (; blocks < 4000 && pc.isPlaying(); ++blocks)
    {
        waitFor ([&] { return pc.bufferedMs() >= 30 || ! pc.isPlaying(); }, 3000);
        buf.clear(); proc.processBlock (buf, m);
        for (int k = 0; k < 512; ++k) e += std::abs (buf.getSample (0, k));
    }
    CHECK (e > 1.0 && ! pc.isPlaying(), "the last seconds play and it stops at the end (%d blocks)", blocks);

    auto out = dir.getChildFile ("long_out.wav"); out.deleteFile();
    const double t2 = juce::Time::getMillisecondCounterHiRes();
    proc.exportPodcast (out, 16);
    juce::String msg; bool done = false;
    waitFor ([&] { if (pc.busy()) return false; done = pc.takeExportDone (msg); return done; }, 3600000);
    CHECK (done && std::abs ((double) out.getSize() - i.seconds * sr * 4.0) < 1e6, "exported 16-bit WAV (%.2f GB, %.0f s)", (double) out.getSize() / 1.0e9, (juce::Time::getMillisecondCounterHiRes() - t2) / 1000.0);
    std::printf ("      peak memory over the whole run: %ld MB\n", peakRssMB());
    out.deleteFile(); f.deleteFile();
}


// debug aid: `AIOTests --speakers file.wav [count]` prints what the speaker detection finds in a real recording
static void speakerReport (const juce::File& f, int requested)
{
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (f));
    if (rd == nullptr) { std::printf ("can't read %s\n", f.getFullPathName().toRawUTF8()); return; }
    const long long len = rd->lengthInSamples;
    auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto a = aio::podcast::analyse ([&] (float* l, float* r, long long pos, int n)
    {
        juce::AudioBuffer<float> b (2, n);
        rd->read (&b, 0, n, pos, true, true);
        std::memcpy (l, b.getReadPointer (0), (size_t) n * sizeof (float));
        std::memcpy (r, b.getReadPointer (rd->numChannels > 1 ? 1 : 0), (size_t) n * sizeof (float));
        return true;
    }, len, rd->sampleRate, nullptr);
    std::printf ("analysis %.1f s for %.1f min of audio\n", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0, (double) len / rd->sampleRate / 60.0);
    auto m = a.speakers;
    if (requested > 0) aio::podcast::clusterSpeakers (m, requested);
    std::printf ("chunks %d, voices %d, separation %.2f\n", (int) m.chunks.size(), m.count, m.separation);
    for (int s = 0; s < m.count; ++s)
        std::printf ("  voice %d: %.0f s, f0 %.0f Hz, level %.1f dB, bands", s + 1, m.speaker[(size_t) s].seconds, m.speaker[(size_t) s].f0, m.speaker[(size_t) s].levelDb);
    std::printf ("\n");
    for (int s = 0; s < m.count; ++s)
    {
        std::printf ("  voice %d spectrum:", s + 1);
        for (int k = 0; k < 8; ++k) std::printf (" %.1f", m.speaker[(size_t) s].bandDb[(size_t) k]);
        std::printf ("\n");
    }
    const double secs = (double) len / rd->sampleRate;
    const int T = (int) (secs / 10.0) + 1;
    std::string line ((size_t) T, '.');
    std::vector<std::array<double, 4>> acc ((size_t) T);
    for (auto& v : acc) v.fill (0.0);
    for (size_t i = 0; i < m.chunks.size(); ++i)
        acc[(size_t) (m.chunks[i].a / rd->sampleRate / 10.0)][(size_t) m.label[i]] += m.chunks[i].seconds;
    for (int t = 0; t < T; ++t)
    {
        int best = -1; double bv = 0.0;
        for (int s = 0; s < 4; ++s) if (acc[(size_t) t][(size_t) s] > bv) { bv = acc[(size_t) t][(size_t) s]; best = s; }
        if (best >= 0) line[(size_t) t] = (char) ('1' + best);
    }
    for (int i = 0; i < T; i += 60) std::printf ("%5d s  %s\n", i * 10, line.substr ((size_t) i, 60).c_str());
    int changes = 0; for (size_t i = 1; i < m.label.size(); ++i) if (m.label[i] != m.label[i - 1]) ++changes;
    std::printf ("voice changes between neighbouring chunks: %d of %d\n", changes, (int) m.chunks.size());
    std::printf ("chunks under 165 Hz (start s / voice / f0):");
    for (size_t i = 0; i < m.chunks.size(); ++i) if (m.chunks[i].f0 < 165.f) std::printf (" %d/%d/%.0f", (int) (m.chunks[i].a / rd->sampleRate), m.label[i] + 1, m.chunks[i].f0);
    std::printf ("\n");
    for (int s = 0; s < m.count; ++s)
    {
        std::vector<float> f; for (size_t i = 0; i < m.chunks.size(); ++i) if (m.label[i] == s) f.push_back (m.chunks[i].f0);
        std::sort (f.begin(), f.end());
        if (! f.empty()) std::printf ("  voice %d f0 10/50/90 percentile: %.0f %.0f %.0f\n", s + 1, f[f.size() / 10], f[f.size() / 2], f[f.size() * 9 / 10]);
    }
}

// =================================================================================================================
// Voices, low cut, echo, warmth, video decode, batch
// =================================================================================================================
static void testPodcastVoices()
{
    std::printf ("\n== Podcast Ready: voices ==\n");
    using namespace aio::podcast;
    const double sr = 44100.0;
    const std::vector<int> order { 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0 };

    // ---- a man and a woman: found automatically and told apart
    const VoiceSpec man { 110.0, { 0, 1, 2 }, 0.10, 0.0 }, woman { 215.0, { 3, 4, 2 }, 0.10, 0.0 };
    const auto duo = makeConversation (sr, { man, woman }, order);
    SpeakerMap m;
    detectSpeakers (readOf (duo.x), (long long) duo.x.size(), sr, -60.0, NoiseProfile(), nullptr, m);
    CHECK (m.valid && m.chunks.size() >= 20, "speech is cut into chunks at the pauses (%d)", (int) m.chunks.size());
    CHECK (m.count == 2, "a man and a woman are found as two voices automatically (%d, separation %.2f)", m.count, m.separation);
    const double acc = voiceAccuracy (m, duo, 2);
    CHECK (acc >= 0.9, "...and each stretch of speech goes to the right person (%.0f %% right)", acc * 100.0);
    if (m.count == 2)
    {
        const int lowV = m.speaker[0].f0 < m.speaker[1].f0 ? 0 : 1;
        CHECK (m.speaker[(size_t) lowV].f0 > 95.0 && m.speaker[(size_t) lowV].f0 < 130.0 && m.speaker[(size_t) (1 - lowV)].f0 > 190.0 && m.speaker[(size_t) (1 - lowV)].f0 < 245.0,
               "...with their pitches measured (%.0f Hz and %.0f Hz)", m.speaker[0].f0, m.speaker[1].f0);
        CHECK (m.speakerAt (0) >= 0 && m.speakerAt ((long long) duo.x.size() - 1) < 2 && ! m.cutPos.empty() && m.cutPos.size() == m.cutSpk.size(), "the switch points between voices are listed");
    }

    // ---- three people, number asked for
    const VoiceSpec w1 { 190.0, { 0, 1 }, 0.10, 0.0 }, w2 { 270.0, { 3, 4 }, 0.10, 0.0 }, mm { 105.0, { 2, 0 }, 0.10, 0.0 };
    const std::vector<int> order3 { 0, 1, 2, 0, 2, 1, 1, 0, 2, 0, 1, 0, 2, 1, 0, 1, 2, 0, 1, 2, 0, 1, 0, 2, 1, 0, 2, 1 };
    const auto trio = makeConversation (sr, { mm, w1, w2 }, order3, 4, 5);
    SpeakerMap m3;
    detectSpeakers (readOf (trio.x), (long long) trio.x.size(), sr, -60.0, NoiseProfile(), nullptr, m3);
    clusterSpeakers (m3, 3);
    CHECK (m3.count == 3, "asking for three voices gives three (%d)", m3.count);
    const double acc3 = voiceAccuracy (m3, trio, 3);
    CHECK (acc3 >= 0.8, "...and they match the three people (%.0f %% right)", acc3 * 100.0);
    clusterSpeakers (m3, 1);
    CHECK (m3.count == 1 && m3.cutPos.size() == 1, "asking for one voice switches the per-voice processing off");
    clusterSpeakers (m3, 0);
    CHECK (m3.count >= 2, "automatic again finds the voices (%d)", m3.count);

    // ---- one voice is one voice
    {
        const auto solo = makeConversation (sr, { man }, std::vector<int> (20, 0));
        SpeakerMap ms;
        detectSpeakers (readOf (solo.x), (long long) solo.x.size(), sr, -60.0, NoiseProfile(), nullptr, ms);
        CHECK (ms.count == 1, "one person talking is not split into several (%d)", ms.count);
    }

    // ---- editing a label
    {
        SpeakerMap e = m;
        if (e.count == 2 && e.chunks.size() > 4)
        {
            const int before = e.speakerAt ((e.chunks[3].a + e.chunks[3].b) / 2);
            e.label[3] = 1 - e.label[3]; e.rebuild();
            CHECK (e.speakerAt ((e.chunks[3].a + e.chunks[3].b) / 2) == 1 - before, "re-assigning one chunk by hand changes who is heard there");
            CHECK (e.count == 2, "...and keeps the number of voices");
        }
    }

    // ---- per-voice EQ: each voice is brought toward a clear speech curve on its own (a dull voice and a bright one need opposite things)
    {
        const VoiceSpec dull { 110.0, { 0, 1, 2 }, 0.10, 700.0 }, bright { 215.0, { 3, 4, 2 }, 0.10, 0.0 };
        const auto cv = makeConversation (sr, { dull, bright }, order);
        auto deviation = [&] (bool speakersOn, double& d0, double& d1)
        {
            Options o; o.denoise = 0.f; o.deess = 0.f; o.comp = 0.f; o.level = 0.f; o.tone = 1.f; o.declip = false; o.speakers = speakersOn; o.spkBalance = 0.f;
            auto b = toBuffer (cv.x); Report rep;
            const bool ok = process (b, sr, o, rep, nullptr);
            const auto y = channel0 (b);
            const auto& shape = aio::AutoVoice::modeParams (aio::VoxSpeech).shape;
            auto dev = [&] (int voice)
            {
                std::array<double, 8> db {};
                for (int k = 0; k < 7; ++k)
                {
                    double e = 0; int n = 0;
                    for (const auto& t : cv.turns) if (t.voice == voice)
                        for (double m : { 0.8, 0.95, 1.1, 1.3 }) { e += std::pow (10.0, goertzelDb (y, sr, (double) aio::AutoVoice::kBandHz[(size_t) k] * m, t.a + 2000, t.b - 2000) / 10.0); ++n; }
                    db[(size_t) k] = 10.0 * std::log10 (e / n + 1e-18);
                }
                const double ref = 0.25 * (db[2] + db[3] + db[4] + db[5]), tref = 0.25 * (shape[2] + shape[3] + shape[4] + shape[5]);
                double s = 0; for (int k = 1; k <= 5; ++k) s += std::abs ((db[(size_t) k] - ref) - ((double) shape[(size_t) k] - tref));
                return s / 5.0;
            };
            d0 = dev (0); d1 = dev (1);
            return ok;
        };
        double on0 = 0, on1 = 0, off0 = 0, off1 = 0;
        const bool ok1 = deviation (true, on0, on1), ok2 = deviation (false, off0, off1);
        CHECK (ok1 && ok2, "both runs finish");
        CHECK (on0 + on1 < off0 + off1 - 0.5, "each voice gets its own EQ: both end up nearer a clear speech curve (off from it by %.1f / %.1f dB, was %.1f / %.1f with one shared EQ)", on0, on1, off0, off1);
    }

    // ---- per-voice level balance: a quiet voice and a loud one come together
    {
        const VoiceSpec loud { 110.0, { 0, 1, 2 }, 0.16, 0.0 }, quiet { 215.0, { 3, 4, 2 }, 0.04, 0.0 };
        const auto cv = makeConversation (sr, { loud, quiet }, order);
        auto gap = [&] (bool on, float bal)
        {
            Options o; o.denoise = 0.f; o.deess = 0.f; o.comp = 0.f; o.level = 0.f; o.tone = 0.f; o.declip = false; o.speakers = on; o.spkBalance = bal;
            auto b = toBuffer (cv.x); Report rep; process (b, sr, o, rep, nullptr);
            const auto y = channel0 (b);
            return std::abs (20.0 * std::log10 (turnsRms (y, cv, 0) / turnsRms (y, cv, 1)));
        };
        const double off = gap (false, 0.f), on = gap (true, 1.f), half = gap (true, 0.5f);
        CHECK (off > 10.0, "(the two voices start %.1f dB apart)", off);
        CHECK (on < off - 5.0 && on < 6.5, "balancing the voices brings them together (%.1f dB apart, was %.1f)", on, off);
        CHECK (half > on - 0.01 && half < off, "...and the balance knob is a proper amount (%.1f dB at half)", half);
    }

    // ---- the live preview treats the voices the same way as the final run
    {
        const VoiceSpec loud { 110.0, { 0, 1, 2 }, 0.16, 0.0 }, quiet { 215.0, { 3, 4, 2 }, 0.04, 0.0 };
        const auto cv = makeConversation (sr, { loud, quiet }, order);
        auto a = std::make_shared<Analysis> (analyse (readOf (cv.x), (long long) cv.x.size(), sr, nullptr));
        CHECK (a->valid && a->speakers.count == 2, "analysis finds the two voices on its own (%d)", a->speakers.count);
        Options o; o.denoise = 0.f; o.deess = 0.f; o.comp = 0.f; o.level = 0.f; o.tone = 0.f; o.declip = false; o.spkBalance = 1.f;
        LiveChain chain (a, o, sr, false, 0.0, 0);
        std::vector<float> oL, oR;
        for (size_t pos = 0; pos < cv.x.size(); pos += 2048)
        {
            std::vector<float> blk (2048, 0.f);
            for (size_t i = 0; i < 2048 && pos + i < cv.x.size(); ++i) blk[i] = cv.x[pos + i];
            chain.push (blk.data(), blk.data(), 2048, oL, oR);
        }
        const double d = std::abs (20.0 * std::log10 (turnsRms (oL, cv, 0) / turnsRms (oL, cv, 1)));
        CHECK (d < 7.0, "LIVE brings the loud and the quiet voice together too (%.1f dB apart)", d);
        // and starting in the middle of the file (a seek) finds the right voice there: same loud-vs-quiet difference as the full run
        {
            const size_t startAt = cv.turns[6].a - 3000, endAt = cv.turns[7].b + 3000;
            LiveChain mid (a, o, sr, false, chain.gainDb(), (long long) startAt);
            std::vector<float> mL, mR;
            for (size_t pos = startAt; pos < endAt; pos += 2048)
            {
                std::vector<float> blk (2048, 0.f);
                for (size_t i = 0; i < 2048 && pos + i < cv.x.size(); ++i) blk[i] = cv.x[pos + i];
                mid.push (blk.data(), blk.data(), 2048, mL, mR);
            }
            auto turnRms = [&] (const std::vector<float>& y, size_t off, const Turn& t) { double s = 0; size_t n = 0; for (size_t i = t.a + 2000; i + 2000 < t.b; ++i) { const double v = y[i - off]; s += v * v; ++n; } return std::sqrt (s / (double) n); };
            const double full = 20.0 * std::log10 (turnRms (oL, 0, cv.turns[6]) / turnRms (oL, 0, cv.turns[7]));
            const double part = 20.0 * std::log10 (turnRms (mL, startAt, cv.turns[6]) / turnRms (mL, startAt, cv.turns[7]));
            CHECK (std::abs (full - part) < 3.0, "a LIVE chain started mid-file knows who is speaking there (voice-to-voice difference %.1f dB, %.1f dB in the full run)", part, full);
        }
    }

    // ---- no clicks when the voice changes: look at the stretches around every switch (and nowhere else)
    {
        const VoiceSpec dull { 110.0, { 0, 1, 2 }, 0.10, 900.0 }, bright { 215.0, { 3, 4, 2 }, 0.10, 0.0 };
        const auto cv = makeConversation (sr, { dull, bright }, order);
        Options o; o.tone = 1.f;
        auto run = [&] (bool on, int& around)
        {
            o.speakers = on;
            auto b = toBuffer (cv.x); Report rep; process (b, sr, o, rep, nullptr);
            const auto y = channel0 (b);
            double e2 = 0; for (size_t i = 2; i < y.size(); ++i) { const double d = (double) y[i] - 2.0 * y[i - 1] + y[i - 2]; e2 += d * d; }
            const double rms2 = std::sqrt (e2 / (double) y.size());
            around = 0;
            for (size_t t = 1; t < cv.turns.size(); ++t)
            {
                if (cv.turns[t].voice == cv.turns[t - 1].voice) continue;
                const size_t from = cv.turns[t - 1].b, to = cv.turns[t].a + 1500;      // the pause between the two people and the first moments of the new one
                for (size_t i = from; i < to && i + 1 < y.size(); ++i) { const double d = (double) y[i] - 2.0 * y[i - 1] + y[i - 2]; if (std::abs (d) > 10.0 * rms2) ++around; }
            }
            double pk = 0; for (size_t i = 2; i < y.size(); ++i) pk = std::max (pk, std::abs ((double) y[i] - 2.0 * y[i - 1] + y[i - 2]));
            return pk / rms2;
        };
        int cOn = 0, cOff = 0;
        const double pkOn = run (true, cOn), pkOff = run (false, cOff);
        CHECK (cOn <= cOff + 2, "switching voices adds no clicks around the switch (%d vs %d with one voice; peak/rms of the 2nd difference %.1f vs %.1f)", cOn, cOff, pkOn, pkOff);
    }
}

static void testPodcastExtras()
{
    std::printf ("\n== Podcast Ready: low cut, echo, warmth, presets ==\n");
    using namespace aio::podcast;
    const double sr = 44100.0;

    // ---- low cut
    {
        std::vector<float> x ((size_t) (sr * 6.0));
        for (size_t i = 0; i < x.size(); ++i)
            x[i] = (float) (0.30 * std::sin (2.0 * 3.14159265 * 100.0 * (double) i / sr) + 0.10 * std::sin (2.0 * 3.14159265 * 1000.0 * (double) i / sr));
        auto run = [&] (float hz)
        {
            Options o; o.denoise = 0.f; o.deess = 0.f; o.comp = 0.f; o.level = 0.f; o.tone = 0.f; o.declip = false; o.lowCutHz = hz;
            auto b = toBuffer (x, false); Report rep; process (b, sr, o, rep, nullptr);
            const auto y = channel0 (b);
            return std::make_pair (goertzelDb (y, sr, 100.0, (size_t) sr, (size_t) (5 * sr)), goertzelDb (y, sr, 1000.0, (size_t) sr, (size_t) (5 * sr)));
        };
        const auto off = run (0.f), on = run (200.f);
        CHECK ((off.first - off.second) - (on.first - on.second) > 18.0, "low cut at 200 Hz takes the 100 Hz rumble down by %.0f dB against the voice band", (off.first - off.second) - (on.first - on.second));
        const auto light = run (60.f);
        CHECK (std::abs ((light.first - light.second) - (off.first - off.second)) < 3.0, "...and a low cut at 60 Hz leaves a 100 Hz tone alone (%.1f dB change)", (light.first - light.second) - (off.first - off.second));
    }

    // ---- echo reduction: a talker in a room, the tail between words comes down while the words stay
    {
        const double rsr = 44100.0;
        auto dry = makeConversation (rsr, { VoiceSpec { 130.0, { 0, 1, 2, 3 }, 0.10, 0.0 } }, std::vector<int> (10, 0), 2, 9);
        // a simple room: four comb filters + two all-pass diffusers, RT60 ~ 0.7 s
        std::vector<float> wet (dry.x.size(), 0.f);
        {
            const double dl[4] = { 0.0297, 0.0371, 0.0411, 0.0437 };
            for (int k = 0; k < 4; ++k)
            {
                const size_t d = (size_t) (dl[k] * rsr);
                const float g = (float) std::pow (10.0, -3.0 * dl[k] / 0.7);
                std::vector<float> buf (dry.x.size(), 0.f);
                for (size_t i = 0; i < dry.x.size(); ++i) { buf[i] = dry.x[i] + (i >= d ? g * buf[i - d] : 0.f); wet[i] += 0.25f * buf[i]; }
            }
        }
        std::vector<float> room (dry.x.size());
        for (size_t i = 0; i < room.size(); ++i) room[i] = 0.55f * dry.x[i] + 0.45f * wet[i];
        auto run = [&] (float echo)
        {
            Options o; o.denoise = 0.f; o.deess = 0.f; o.comp = 0.f; o.level = 0.f; o.tone = 0.f; o.declip = false; o.echo = echo; o.speakers = false;
            auto b = toBuffer (room, false); Report rep; process (b, rsr, o, rep, nullptr);
            const auto y = channel0 (b);
            // tail: 200-450 ms after the end of each turn; voice: inside the turns
            std::vector<std::pair<size_t, size_t>> tails, voice;
            for (const auto& t : dry.turns) { tails.push_back ({ t.b + (size_t) (0.20 * rsr), t.b + (size_t) (0.45 * rsr) }); voice.push_back ({ t.a, t.b }); }
            return std::make_pair (rmsRanges (y, tails), rmsRanges (y, voice, 1000));
        };
        const auto off = run (0.f), on = run (1.f);
        const double tailDrop = 20.0 * std::log10 (off.first / std::max (1e-9, on.first)), voiceChange = 20.0 * std::log10 (on.second / off.second);
        CHECK (tailDrop > 3.0, "echo reduction takes the room tail between words down (%.1f dB)", tailDrop);
        CHECK (voiceChange > -3.5, "...without sinking the voice (%.1f dB)", voiceChange);
        const auto mid = run (0.5f);
        const double midDrop = 20.0 * std::log10 (off.first / std::max (1e-9, mid.first));
        CHECK (midDrop > 1.0 && midDrop < tailDrop + 0.2, "...and the knob is a proper amount (%.1f dB at half)", midDrop);
        // off means off: identical to a run without the stage
        Options o0; o0.echo = 0.f; Options o1 = o0;
        auto b0 = toBuffer (room, false), b1 = toBuffer (room, false); Report r0, r1;
        process (b0, rsr, o0, r0, nullptr); process (b1, rsr, o1, r1, nullptr);
        double dmax = 0; for (int i = 0; i < b0.getNumSamples(); ++i) dmax = std::max (dmax, (double) std::abs (b0.getSample (0, i) - b1.getSample (0, i)));
        CHECK (dmax == 0.0, "the stage is deterministic and off at 0");
    }

    // ---- warmth
    {
        std::vector<float> x ((size_t) (sr * 5.0));
        for (size_t i = 0; i < x.size(); ++i) x[i] = (float) (0.35 * std::sin (2.0 * 3.14159265 * 220.0 * (double) i / sr));
        auto thd = [&] (float warmth)
        {
            Options o; o.denoise = 0.f; o.deess = 0.f; o.comp = 0.f; o.level = 0.f; o.tone = 0.f; o.declip = false; o.warmth = warmth; o.speakers = false; o.targetLufs = -16.f;
            auto b = toBuffer (x, false); Report rep; process (b, sr, o, rep, nullptr);
            const auto y = channel0 (b);
            const double f1 = goertzelDb (y, sr, 220.0, (size_t) sr, (size_t) (4 * sr));
            double h = 0; for (int k = 2; k <= 5; ++k) h += std::pow (10.0, goertzelDb (y, sr, 220.0 * k, (size_t) sr, (size_t) (4 * sr)) / 10.0);
            double pk = 0; for (float v : y) pk = std::max (pk, (double) std::abs (v));
            return std::make_tuple (10.0 * std::log10 (h) - f1, pk, rep.after.lufs);
        };
        const auto [d0, p0, l0] = thd (0.f);
        const auto [d1, p1, l1] = thd (1.f);
        CHECK (d0 < -55.0, "warmth 0 adds no distortion (%.0f dB)", d0);
        CHECK (d1 > d0 + 20.0 && d1 < -12.0, "full warmth adds gentle harmonics (%.0f dB)", d1);
        CHECK (std::abs (l1 - (-16.0)) < 0.7 && p1 <= 0.95f, "...and the loudness and peaks still land where they should (%.1f LUFS, peak %.2f)", l1, p1);
        (void) p0; (void) l0;
    }

    // ---- presets
    {
        CHECK (AIOProcessor::podcastPresetCount() == 9, "nine starting points, with three for weddings (%d)", AIOProcessor::podcastPresetCount());
        AIOProcessor p; p.setPlayConfigDetails (2, 2, 44100.0, 512); p.prepareToPlay (44100.0, 512);
        int found = 0;
        for (int i = 0; i < AIOProcessor::podcastPresetCount(); ++i)
        {
            const juce::String n (AIOProcessor::podcastPresetName (i));
            if (! n.startsWith ("Wedding")) continue;
            ++found;
            p.applyPodcastPreset (i);
            const auto o = p.readPodcastOptions();
            CHECK (o.lowCutHz >= 80.f && o.echo > 0.f && o.speakers && o.targetLufs == -16.f, "'%s': low cut %.0f Hz, echo %.0f %%, voices on, %s", n.toRawUTF8(), o.lowCutHz, o.echo * 100.f, o.mono ? "mono" : "stereo");
        }
        CHECK (found == 3, "three wedding presets (%d)", found);
        p.applyPodcastPreset (0);
        const auto o = p.readPodcastOptions();
        CHECK (o.lowCutHz == 0.f && o.echo == 0.f && o.warmth == 0.f, "the plain podcast preset leaves low cut, echo and warmth off");
        p.setParam ("pcSpkCount", 3.f);
        CHECK (p.readPodcastOptions().speakerRequest == 3, "the voice count is a setting");
    }
}

static void testPodcastFiles()
{
    std::printf ("\n== Podcast Ready: video / compressed files, voices in a session, batch ==\n");
    using namespace aio::podcast;
    const double sr = 44100.0;
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_podcast_files");
    dir.deleteRecursively(); dir.createDirectory();

    // ---- which files get decoded first
    CHECK (aio::media::needsDecode (juce::File ("a.MP4")) && aio::media::needsDecode (juce::File ("x/clip.mov")) && aio::media::needsDecode (juce::File ("v.m4a")) && aio::media::needsDecode (juce::File ("v.mkv")),
          "MP4, MOV, M4A, MKV files are decoded first");
    CHECK (! aio::media::needsDecode (juce::File ("a.wav")) && ! aio::media::needsDecode (juce::File ("a.flac")) && ! aio::media::needsDecode (juce::File ("a.mp3")), "WAV, FLAC and MP3 are read directly");
    CHECK (aio::media::chooserPattern().contains ("*.mp4") && aio::media::chooserPattern().contains ("*.mov") && aio::media::chooserPattern().contains ("*.wav"), "the file chooser offers video files too");

    // ---- decoding itself (any format JUCE can read here)
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    const auto duo = makeConversation (sr, { VoiceSpec { 110.0, { 0, 1, 2 }, 0.10, 0.0 }, VoiceSpec { 215.0, { 3, 4, 2 }, 0.10, 0.0 } },
                                       { 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 1, 0 });
    auto writeFile = [&] (const juce::File& f, juce::AudioFormat& fmt, int bits, const std::vector<float>& x)
    {
        f.deleteFile();
        juce::AudioBuffer<float> b = toBuffer (x, false);
        std::unique_ptr<juce::FileOutputStream> os (f.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (fmt.createWriterFor (os.get(), sr, 1, bits, {}, 0));
        if (! w) return false;
        os.release();
        return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    };
    juce::FlacAudioFormat flacF; juce::WavAudioFormat wavF;
    const auto flac = dir.getChildFile ("clip.flac");
    CHECK (writeFile (flac, flacF, 24, duo.x), "(test FLAC written)");
    {
        const auto out = dir.getChildFile ("decoded.wav");
        juce::String err;
        const bool ok = aio::media::decodeToWav (flac, out, fm, nullptr, err);
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (out));
        CHECK (ok && rd != nullptr && rd->lengthInSamples == (long long) duo.x.size() && rd->numChannels == 1 && std::abs (rd->sampleRate - sr) < 1.0, "a compressed file is decoded to a WAV of the same length (%s)", err.toRawUTF8());
        const auto junk = dir.getChildFile ("junk.mp4");
        junk.replaceWithText ("this is not a video");
        juce::String err2;
        CHECK (! aio::media::decodeToWav (junk, dir.getChildFile ("junk.wav"), fm, nullptr, err2) && err2.isNotEmpty(), "a broken video file fails with a message instead of crashing (\"%s\")", err2.toRawUTF8());
        CHECK (! aio::media::decodeToWav (dir.getChildFile ("nope.mov"), dir.getChildFile ("nope.wav"), fm, nullptr, err2) && err2.isNotEmpty(), "a missing file fails with a message too");
    }

    // ---- a session: load, voices, re-label, decode path
    const auto wav = dir.getChildFile ("ceremony.wav");
    CHECK (writeFile (wav, wavF, 24, duo.x), "(test WAV written)");
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        auto& pc = p.getPodcast();
        pc.loadFile (wav);
        waitFor ([&] { return ! pc.busy() && pc.info().hasFile; }, 60000);
        auto info = pc.info();
        CHECK (info.hasFile && info.voiceCount == 2 && info.voices.size() == 2 && info.segments.size() >= 20, "the session reports the two voices and when they speak (%d voices, %d blocks)", info.voiceCount, (int) info.segments.size());
        CHECK (info.voiceRequest == 0 && ! info.decoded, "(automatic; read directly)");
        pc.setSpeakerCount (3);
        info = pc.info();
        CHECK (info.voiceCount == 3 && info.voiceRequest == 3, "asking for three voices re-groups the same recording (%d)", info.voiceCount);
        pc.setSpeakerCount (1);
        CHECK (pc.info().voiceCount == 1, "one voice turns it off");
        pc.setSpeakerCount (0);
        info = pc.info();
        CHECK (info.voiceCount == 2, "automatic again finds two (%d)", info.voiceCount);
        const double mid = 0.5 * (info.segments[5].t0 + info.segments[5].t1);
        const int was = info.segments[5].voice;
        pc.cycleSpeakerAt (mid, 1);
        info = pc.info();
        CHECK (info.segments[5].voice != was, "clicking a block moves it to the other voice (%d -> %d)", was, info.segments[5].voice);
        pc.cycleSpeakerAt (mid, -1);
        CHECK (pc.info().segments[5].voice == was, "...and clicking the other way moves it back");
        // run it, then edit: the render is marked as out of date
        p.runPodcast();
        waitFor ([&] { return ! pc.busy(); }, 60000);
        CHECK (pc.info().hasResult && ! pc.info().resultStale && pc.info().used.speakers, "a run with the voices on works");
        pc.cycleSpeakerAt (mid, 1);
        CHECK (pc.info().resultStale, "editing the voices after a run says the render needs repeating");
        // the live preview keeps working through all of that
        pc.setPlaying (true);
        bool audible = false;
        for (int b = 0; b < 40 && ! audible; ++b)
        {
            waitFor ([&] { return pc.bufferedMs() >= 30; }, 3000);
            juce::AudioBuffer<float> buf (2, 512); buf.clear(); juce::MidiBuffer mb; p.processBlock (buf, mb);
            audible = buf.getMagnitude (0, 512) > 0.01f;
        }
        pc.setPlaying (false);
        CHECK (audible, "the live preview plays after the voices were edited");
    }
    {
        // the decode path through a session (every file is decoded when the test hook is on)
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        auto& pc = p.getPodcast();
        aio::media::forceDecodeForTests (true);
        pc.loadFile (flac);
        waitFor ([&] { return ! pc.busy() && pc.info().hasFile; }, 60000);
        aio::media::forceDecodeForTests (false);
        const auto info = pc.info();
        CHECK (info.hasFile && info.decoded && info.name == "clip.flac" && info.voiceCount == 2 && std::abs (info.seconds - (double) duo.x.size() / sr) < 0.01, "a decoded file loads like any other and keeps its own name (%s, %d voices)", info.name.toRawUTF8(), info.voiceCount);
        p.runPodcast();
        waitFor ([&] { return ! pc.busy(); }, 60000);
        CHECK (pc.info().hasResult, "...and can be processed");
        int srcDirs = 0;
        for (const auto& d : juce::File::getSpecialLocation (juce::File::tempDirectory).findChildFiles (juce::File::findDirectories, false, "AIOStudioPodcast_src_*")) { (void) d; ++srcDirs; }
        CHECK (srcDirs >= 1, "(the decoded audio sits in its own temporary folder)");
    }
    {
        int srcDirs = 0;
        for (const auto& d : juce::File::getSpecialLocation (juce::File::tempDirectory).findChildFiles (juce::File::findDirectories, false, "AIOStudioPodcast_src_*")) { (void) d; ++srcDirs; }
        CHECK (srcDirs == 0, "closing the plug-in removes the decoded audio (%d folders left)", srcDirs);
    }

    // ---- batch
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        auto& pc = p.getPodcast();
        const auto f1 = dir.getChildFile ("one.wav"), f2 = dir.getChildFile ("two.flac"), bad = dir.getChildFile ("broken.wav");
        std::vector<float> shortClip (duo.x.begin(), duo.x.begin() + (long) (sr * 25.0));
        CHECK (writeFile (f1, wavF, 24, shortClip) && writeFile (f2, flacF, 24, shortClip), "(batch inputs written)");
        bad.replaceWithText ("RIFF not really");
        const auto outDir = dir.getChildFile ("results");
        Options o; o.targetLufs = -16.f;
        pc.runBatch ({ f1, bad, f2 }, outDir, o, 24, false);
        CHECK (pc.state() == aio::PodcastSession::Batching || pc.busy() || pc.batchStatus().total == 3, "the batch starts");
        waitFor ([&] { return ! pc.busy(); }, 180000);
        auto bs = pc.batchStatus();
        CHECK (bs.total == 3 && bs.done == 2 && bs.failed == 1 && bs.finished, "two files done, the broken one skipped (%d done, %d skipped)", bs.done, bs.failed);
        const auto r1 = outDir.getChildFile ("one - podcast ready.wav"), r2 = outDir.getChildFile ("two - podcast ready.wav");
        std::unique_ptr<juce::AudioFormatReader> a (fm.createReaderFor (r1)), b (fm.createReaderFor (r2));
        CHECK (a != nullptr && b != nullptr && a->lengthInSamples == (long long) shortClip.size(), "the finished files are in the folder with the right length");
        if (a)
        {
            const auto t = juce::AudioBuffer<float> (1, (int) a->lengthInSamples);
            std::vector<float> y ((size_t) a->lengthInSamples);
            juce::AudioBuffer<float> tmp (1, (int) a->lengthInSamples); a->read (&tmp, 0, (int) a->lengthInSamples, 0, true, false);
            const auto st = measure (tmp.getReadPointer (0), tmp.getReadPointer (0), (size_t) tmp.getNumSamples(), sr);
            CHECK (std::abs (st.lufs - (-16.0)) < 0.7 && st.truePeakDb <= -0.8, "...each cleaned to the target (%.1f LUFS, %.1f dBTP)", st.lufs, st.truePeakDb);
            (void) t; (void) y;
        }
        bool mentionsBroken = false; for (auto& l : bs.log) if (l.contains ("broken.wav") && l.contains ("skipped")) mentionsBroken = true;
        CHECK (mentionsBroken, "the log says which file was skipped");
        // running it again does not overwrite what is there
        pc.runBatch ({ f1 }, outDir, o, 24, false);
        waitFor ([&] { return ! pc.busy(); }, 120000);
        CHECK (outDir.getChildFile ("one - podcast ready (2).wav").existsAsFile() && r1.existsAsFile(), "an earlier result is never overwritten (the new one is numbered)");
        int work = 0;
        for (const auto& d : juce::File::getSpecialLocation (juce::File::tempDirectory).findChildFiles (juce::File::findDirectories, false, "AIOStudioPodcast_batch_*")) { (void) d; ++work; }
        CHECK (work == 0, "the batch leaves no temporary folders behind (%d)", work);
        // cancel
        pc.runBatch ({ f1, f2, f1, f2 }, dir.getChildFile ("cancelled"), o, 24, false);
        juce::Thread::sleep (300);
        pc.cancel();
        waitFor ([&] { return ! pc.busy(); }, 120000);
        CHECK (pc.state() == aio::PodcastSession::Idle, "a batch can be cancelled");
    }
    dir.deleteRecursively();
}
