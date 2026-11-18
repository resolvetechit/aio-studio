// =================================================================================================================
// Podcast Ready: declip, denoise, loudness + the session / processor / UI around it
// =================================================================================================================
#include "../Source/podcast/Podcast.h"

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

    // ---- audition through processBlock
    auto play = [&] (bool original, bool match, double& rms, float& peak)
    {
        pc.abOriginal = original; pc.matchVolume = match; pc.loop = true;
        pc.seek (0.1); pc.setPlaying (true);
        juce::AudioBuffer<float> buf (2, 512); juce::MidiBuffer m;
        double e = 0; size_t cnt = 0; peak = 0.f; bool fin = true;
        for (int b = 0; b < 300; ++b)
        {
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
        for (int b = 0; b < 100; ++b) { buf.clear(); p2.processBlock (buf, m); for (int k = 0; k < 512; ++k) { e += buf.getSample (0, k) * buf.getSample (0, k); d += std::abs (buf.getSample (0, k) - buf.getSample (1, k)); } ++secs; }
        q.setPlaying (false);
        CHECK (e > 1.0 && d < 1e-3, "preview resamples 48 kHz -> 44.1 kHz and plays on both channels (energy %.1f)", e);
        const double pos0 = q.position();
        q.seek (0.5);
        CHECK (std::abs (q.position() - 0.5) < 0.01 && pos0 >= 0.0, "seeking works (%.2f)", q.position());
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

    const auto f = writeTestVoice ("ui.wav", sr, 14, true);
    p.getPodcast().loadFile (f);
    waitFor ([&] { juce::MessageManager::getInstance()->runDispatchLoopUntil (20); return ! p.getPodcast().busy() && p.getPodcast().info().hasFile; }, 20000);
    p.applyPodcastPreset (3);
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
