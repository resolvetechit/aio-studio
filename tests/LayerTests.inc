// =================================================================================================================
// Layer engine + voice changer tests
// =================================================================================================================
#include "../Source/dsp/LayerEngine.h"
#include "../Source/dsp/LayerPresets.h"
#include "../Source/dsp/PitchCorrector.h"
#include "../Source/ui/LayersUI.h"

namespace
{
struct LyEv { double t; int note; bool on; };
struct Stereo { std::vector<float> l, r, m; };

Stereo runLayers (aio::LayerEngine& e, const aio::LayersSettings& s, double sr, double seconds, const std::vector<LyEv>& ev)
{
    Stereo o;
    const size_t total = (size_t) std::lround (seconds * sr);
    o.l.assign (total, 0.f); o.r.assign (total, 0.f);
    size_t pos = 0, next = 0;
    std::vector<LyEv> sorted = ev;
    std::sort (sorted.begin(), sorted.end(), [] (const LyEv& a, const LyEv& b) { return a.t < b.t; });
    while (pos < total)
    {
        size_t end = std::min (total, pos + 256);
        while (next < sorted.size() && (size_t) std::lround (sorted[next].t * sr) <= pos)
        {
            if (sorted[next].on) e.noteOn (sorted[next].note, 100); else e.noteOff (sorted[next].note);
            ++next;
        }
        if (next < sorted.size()) end = std::min (end, std::max (pos + 1, (size_t) std::lround (sorted[next].t * sr)));
        e.render (o.l.data() + pos, o.r.data() + pos, (int) (end - pos), s);
        pos = end;
    }
    o.m.resize (total);
    for (size_t i = 0; i < total; ++i) o.m[i] = 0.5f * (o.l[i] + o.r[i]);
    return o;
}

aio::LayersSettings soloLayer (aio::LayerSrc src, float levelDb = 0.f)
{
    aio::LayersSettings s;
    s.on = true;
    auto& c = s.layer[0];
    c.on = true; c.src = (int) src; c.levelDb = levelDb; c.attackMs = 5.f; c.releaseMs = 400.f; c.cutoffHz = 16000.f; c.reso = 0.f; c.motion = 0.f;
    c.track = src != aio::LayerSrc::Impact;
    return s;
}

double hfRatio (const std::vector<float>& x, size_t a, size_t b)
{
    double e = 0, d = 0;
    for (size_t i = a + 1; i < b && i < x.size(); ++i) { e += (double) x[i] * x[i]; const double t = (double) x[i] - x[i - 1]; d += t * t; }
    return d / std::max (e, 1.0e-20);
}

double centroid (const std::array<double, 8>& db)
{
    double sw = 0, sp = 0;
    for (int i = 0; i < 8; ++i) { const double p = std::pow (10.0, db[(size_t) i] / 10.0); sw += p * (300.0 + 600.0 * i); sp += p; }
    return sw / std::max (sp, 1.0e-20);
}

std::vector<float> runVoiceChanger (const std::vector<float>& voice, double sr, float transposeSt, float formantSt, uint16_t mask = 0, float amount = 1.f)
{
    aio::PitchCorrector pc;
    pc.prepare (sr);
    aio::TunerSettings ts;
    ts.on = true; ts.mask = mask; ts.amount = amount; ts.retuneMs = 0.f; ts.transposeSt = transposeSt; ts.formantSt = formantSt;
    ts.minHz = 60.f; ts.maxHz = 600.f;
    std::vector<float> l (voice), r (voice);
    for (size_t pos = 0; pos < voice.size(); pos += 128)
    {
        const int m = (int) std::min<size_t> (128, voice.size() - pos);
        pc.process (l.data() + pos, r.data() + pos, m, ts);
    }
    return l;
}
} // namespace

static void testLayerEngine()
{
    std::printf ("\n-- layer engine --\n");
    const double sr = 44100.0;

    // every sound source: audible, bounded, silent before the note, released after it
    for (int src = 0; src < 6; ++src)
    {
        if (src == (int) aio::LayerSrc::Impact) continue;           // a one-shot: has its own test below
        aio::LayerEngine e; e.prepare (sr);
        auto s = soloLayer ((aio::LayerSrc) src);
        const auto o = runLayers (e, s, sr, 4.0, { { 0.5, 57, true }, { 2.0, 57, false } });
        const double before = rmsOf (o.m, sr, 0.0, 0.45), during = rmsOf (o.m, sr, 0.8, 1.9), after = rmsOf (o.m, sr, 3.6, 4.0);
        CHECK (before < 1.0e-6, "%s: silent before the note", aio::layerSrcName (src));
        std::printf ("    %-10s rms %.3f\n", aio::layerSrcName (src), during);
        CHECK (during > 0.02 && during < 0.5, "%s: audible and sane (rms %.3f)", aio::layerSrcName (src), during);
        CHECK (after < during * 0.03 + 1.0e-4, "%s: dies away after the key is released (%.4f -> %.5f)", aio::layerSrcName (src), during, after);
        CHECK (allFinite (o.l, 3.f) && allFinite (o.r, 3.f), "%s: finite and bounded", aio::layerSrcName (src));
        CHECK (! e.anyActive(), "%s: the voice is freed after the release", aio::layerSrcName (src));
    }

    // pitched sources play the key that was pressed
    for (int src : { 0, 4, 5 })
    {
        aio::LayerEngine e; e.prepare (sr);
        auto s = soloLayer ((aio::LayerSrc) src);
        const auto o = runLayers (e, s, sr, 3.0, { { 0.0, 57, true } });
        const double m = noteMedianMidi (o.m, sr, 0.8, 2.8);
        CHECK (std::abs (m - 57.0) < 0.35, "%s plays A3 (%.2f)", aio::layerSrcName (src), m);
        aio::LayerEngine e2; e2.prepare (sr);
        auto s2 = s; s2.layer[0].coarse = 12.f;
        const auto o2 = runLayers (e2, s2, sr, 3.0, { { 0.0, 57, true } });
        const double m2 = noteMedianMidi (o2.m, sr, 0.8, 2.8);
        CHECK (std::abs (m2 - 69.0) < 0.4, "%s: +12 semitones (%.2f)", aio::layerSrcName (src), m2);
    }

    // level and pan
    {
        aio::LayerEngine e; e.prepare (sr);
        auto s = soloLayer (aio::LayerSrc::Pad, 0.f);
        const auto a = runLayers (e, s, sr, 1.5, { { 0.0, 60, true } });
        aio::LayerEngine e2; e2.prepare (sr);
        s.layer[0].levelDb = -6.f;
        const auto b = runLayers (e2, s, sr, 1.5, { { 0.0, 60, true } });
        const double d = 20.0 * std::log10 (rmsOf (a.m, sr, 0.5, 1.4) / rmsOf (b.m, sr, 0.5, 1.4));
        CHECK (std::abs (d - 6.0) < 0.6, "level knob moves the layer by 6 dB (%.2f)", d);
        aio::LayerEngine e3; e3.prepare (sr);
        s.layer[0].levelDb = 0.f; s.layer[0].pan = -100.f;
        const auto c = runLayers (e3, s, sr, 1.5, { { 0.0, 60, true } });
        CHECK (rmsOf (c.r, sr, 0.5, 1.4) < rmsOf (c.l, sr, 0.5, 1.4) * 0.02, "pan hard left leaves the right channel silent");
        aio::LayerEngine e4; e4.prepare (sr);
        s.layer[0].pan = 0.f;
        const auto d0 = runLayers (e4, s, sr, 1.5, { { 0.0, 60, true } });
        const double l = rmsOf (d0.l, sr, 0.5, 1.4), r = rmsOf (d0.r, sr, 0.5, 1.4);
        CHECK (std::abs (20.0 * std::log10 (l / r)) < 2.5, "centred pad is balanced (L/R %.1f dB)", 20.0 * std::log10 (l / r));
    }

    // envelope: a slow attack, an exact-ish release
    {
        aio::LayerEngine e; e.prepare (sr);
        auto s = soloLayer (aio::LayerSrc::Pad);
        s.layer[0].attackMs = 800.f; s.layer[0].releaseMs = 500.f;
        const auto o = runLayers (e, s, sr, 4.0, { { 0.0, 60, true }, { 2.0, 60, false } });
        CHECK (rmsOf (o.m, sr, 0.0, 0.1) < rmsOf (o.m, sr, 1.2, 1.8) * 0.2, "slow attack: it fades in");
        CHECK (rmsOf (o.m, sr, 2.0, 2.05) > rmsOf (o.m, sr, 1.2, 1.8) * 0.7, "...and holds until the key goes up");
        CHECK (rmsOf (o.m, sr, 2.55, 2.65) < rmsOf (o.m, sr, 1.2, 1.8) * 0.03, "release time 500 ms: about -40 dB after it (%.4f)", rmsOf (o.m, sr, 2.55, 2.65) / rmsOf (o.m, sr, 1.2, 1.8));
    }

    // sustain pedal holds a released note
    {
        aio::LayerEngine e; e.prepare (sr);
        auto s = soloLayer (aio::LayerSrc::Pad);
        e.noteOn (60, 100);
        std::vector<float> l (44100), r (44100);
        e.render (l.data(), r.data(), 22050, s);
        e.setSustain (true); e.noteOff (60);
        e.render (l.data() + 22050, r.data() + 22050, 22050, s);
        const double held = rmsOf (std::vector<float> (l.begin() + 30000, l.end()), sr, 0.0, 1.0);
        e.setSustain (false);
        std::vector<float> l2 (44100), r2 (44100);
        e.render (l2.data(), r2.data(), 44100, s);
        CHECK (held > 0.01, "sustain pedal keeps the note sounding (%.3f)", held);
        CHECK (rmsOf (l2, sr, 0.6, 1.0) < held * 0.05, "...and lifting it releases it");
    }

    // polyphony: more keys than voices
    {
        aio::LayerEngine e; e.prepare (sr);
        aio::LayersSettings s; s.on = true;
        for (int i = 0; i < 9; ++i) { auto& c = s.layer[(size_t) i]; c.on = true; c.src = i % 6; c.levelDb = -14.f; c.releaseMs = 800.f; }
        std::vector<LyEv> ev;
        for (int k = 0; k < 14; ++k) ev.push_back ({ 0.05 * k, 48 + 3 * k, true });
        const auto o = runLayers (e, s, sr, 3.0, ev);
        CHECK (allFinite (o.l, 6.f) && allFinite (o.r, 6.f), "14 keys on 8 voices with 9 layers: finite and bounded");
        CHECK (e.nActive.load() <= aio::LayerEngine::kVoices, "never more than %d voices", aio::LayerEngine::kVoices);
    }

    // brightness macro and layer cutoff
    {
        auto run = [&] (float bright, float cutoff)
        {
            aio::LayerEngine e; e.prepare (sr);
            auto s = soloLayer (aio::LayerSrc::Pad); s.bright = bright; s.layer[0].cutoffHz = cutoff;
            const auto o = runLayers (e, s, sr, 2.0, { { 0.0, 48, true } });
            return hfRatio (o.m, 44100, 88000);
        };
        const double dark = run (-100.f, 3000.f), bright = run (100.f, 3000.f), open = run (0.f, 16000.f);
        CHECK (bright > dark * 4.0, "brightness macro opens / closes the filters (%.5f vs %.5f)", dark, bright);
        CHECK (open > run (0.f, 1000.f) * 4.0, "layer cutoff closes the sound");
    }

    // lo-fi changes the sound but not the level much; drive keeps it bounded
    {
        auto run = [&] (float lofi, float drive)
        {
            aio::LayerEngine e; e.prepare (sr);
            auto s = soloLayer (aio::LayerSrc::Bell); s.layer[0].lofi = lofi; s.layer[0].drive = drive;
            return runLayers (e, s, sr, 1.5, { { 0.0, 72, true } });
        };
        const auto clean = run (0.f, 0.f), crushed = run (100.f, 0.f), hot = run (0.f, 100.f);
        double diff = 0; for (size_t i = 8000; i < 60000; ++i) diff += std::abs (clean.m[i] - crushed.m[i]);
        CHECK (diff > 1.0, "lo-fi changes the waveform (%.2f)", diff);
        const double rc = rmsOf (clean.m, sr, 0.3, 1.0), rl = rmsOf (crushed.m, sr, 0.3, 1.0);
        CHECK (std::abs (20.0 * std::log10 (rl / rc)) < 6.0, "...without a big level jump (%.1f dB)", 20.0 * std::log10 (rl / rc));
        CHECK (allFinite (hot.l, 3.f), "drive stays bounded");
    }

    // the impact is a one-shot: it rings out even if the key is released straight away
    {
        aio::LayerEngine e; e.prepare (sr);
        auto s = soloLayer (aio::LayerSrc::Impact);
        s.layer[0].releaseMs = 2000.f;
        const auto o = runLayers (e, s, sr, 2.0, { { 0.0, 60, true }, { 0.05, 60, false } });
        CHECK (rmsOf (o.m, sr, 0.3, 0.5) > 0.01, "impact keeps ringing after a short key press (%.3f)", rmsOf (o.m, sr, 0.3, 0.5));
        CHECK (rmsOf (o.m, sr, 0.0, 0.1) > rmsOf (o.m, sr, 1.5, 2.0) * 10.0, "...and it decays");
    }

    // sample layer
    {
        auto sd = std::make_shared<aio::SampleData>();
        sd->sr = 44100.0; sd->rootNote = 57; sd->name = "sine";
        sd->l.resize (44100);
        for (size_t i = 0; i < sd->l.size(); ++i) sd->l[i] = 0.5f * (float) std::sin (2.0 * aio::kPi * 220.0 * (double) i / 44100.0);
        auto run = [&] (int note, bool loop, double secs)
        {
            aio::LayerEngine e; e.prepare (sr);
            e.setSample (0, sd);
            auto s = soloLayer (aio::LayerSrc::Sample); s.layer[0].loop = loop;
            return runLayers (e, s, sr, secs, { { 0.0, note, true } });
        };
        const auto a = run (57, true, 3.0);
        CHECK (std::abs (noteMedianMidi (a.m, sr, 0.5, 2.8) - 57.0) < 0.15, "sample at its root note plays unchanged (%.2f)", noteMedianMidi (a.m, sr, 0.5, 2.8));
        const auto b = run (69, true, 3.0);
        CHECK (std::abs (noteMedianMidi (b.m, sr, 0.5, 2.8) - 69.0) < 0.2, "an octave up on the next octave key (%.2f)", noteMedianMidi (b.m, sr, 0.5, 2.8));
        CHECK (rmsOf (a.m, sr, 2.0, 2.9) > 0.1, "looping sample keeps playing past its end");
        const auto c = run (57, false, 3.0);
        CHECK (rmsOf (c.m, sr, 0.2, 0.9) > 0.1 && rmsOf (c.m, sr, 1.5, 2.9) < 1.0e-3, "one-shot sample stops at its end");
        aio::LayerEngine e; e.prepare (sr);
        const auto d = runLayers (e, soloLayer (aio::LayerSrc::Sample), sr, 1.0, { { 0.0, 60, true } });
        CHECK (rmsOf (d.m, sr, 0.1, 0.9) < 1.0e-6, "sample layer with no sample loaded is silent");
    }

    // factory sounds: every one plays, none is silent or wildly loud
    {
        const auto& presets = aio::layerPresets();
        double minDb = 0, maxDb = -200;
        int bad = 0;
        for (const auto& p : presets)
        {
            aio::LayerEngine e; e.prepare (sr);
            aio::LayersSettings s; s.on = true; s.bright = p.bright; s.motion = p.motion; s.relScale = p.relScale;
            for (int i = 0; i < aio::kLayers; ++i) s.layer[(size_t) i] = p.layer[i];
            const auto o = runLayers (e, s, sr, 4.0, { { 0.0, 48, true }, { 0.0, 55, true }, { 0.0, 60, true }, { 2.5, 48, false }, { 2.5, 55, false }, { 2.5, 60, false } });
            const double db = 20.0 * std::log10 (rmsOf (o.m, sr, 0.8, 2.4) + 1.0e-9);
            minDb = std::min (minDb, db); maxDb = std::max (maxDb, db);
            std::printf ("    %-26s %6.1f dBFS rms\n", p.name.c_str(), db);
            if (! allFinite (o.l, 3.f) || ! allFinite (o.r, 3.f) || db < -42.0 || db > -6.0) { ++bad; std::printf ("      ^ out of range\n"); }
        }
        CHECK (bad == 0, "all %d factory sounds are audible, bounded and sensibly loud (%.1f .. %.1f dBFS)", (int) presets.size(), minDb, maxDb);
        CHECK (maxDb - minDb < 14.0, "...and the loudest is within 14 dB of the quietest (%.1f dB)", maxDb - minDb);
    }

    // cost: 8 voices x 9 layers, 10 seconds
    {
        aio::LayerEngine e; e.prepare (48000.0);
        aio::LayersSettings s; s.on = true;
        for (int i = 0; i < 9; ++i) { auto& c = s.layer[(size_t) i]; c.on = true; c.src = i % 6; c.levelDb = -16.f; c.releaseMs = 4000.f; c.motion = 50.f; c.reso = 30.f; }
        for (int k = 0; k < 8; ++k) e.noteOn (40 + 4 * k, 100);
        std::vector<float> l (480), r (480);
        const auto t0 = std::chrono::steady_clock::now();
        for (int b = 0; b < 1000; ++b) { std::fill (l.begin(), l.end(), 0.f); std::fill (r.begin(), r.end(), 0.f); e.render (l.data(), r.data(), 480, s); }
        const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("    8 voices x 9 layers: 10 s of audio in %.0f ms (%.1f%% of one core)\n", ms, ms / 100.0);
        CHECK (ms < 4000.0, "worst case (72 sources) stays under 40%% of a core (%.1f%%)", ms / 100.0);
    }
}

static void testVoiceChanger()
{
    std::printf ("\n-- voice changer --\n");
    const double sr = 44100.0;
    const auto voice = vowelVoice (sr, 150.0, { 0, 0, 0, 0 }, 1.0);
    const double in = noteMedianMidi (voice, sr, 1.0, 3.5);

    for (float st : { 5.f, -4.f, 7.f, -6.f })
    {
        const auto out = runVoiceChanger (voice, sr, st, 0.f);
        const double m = noteMedianMidi (out, sr, 1.0, 3.5);
        CHECK (std::abs (m - (in + (double) st)) < 0.5, "voice pitch %+.0f semitones (%.2f -> %.2f)", (double) st, in, m);
        CHECK (allFinite (out, 2.f), "...bounded");
        CHECK (rmsOf (out, sr, 1.0, 3.5) > rmsOf (voice, sr, 1.0, 3.5) * 0.4, "...(the shifter alone) keeps most of its level at %+.0f (%.3f vs %.3f)", (double) st, rmsOf (out, sr, 1.0, 3.5), rmsOf (voice, sr, 1.0, 3.5));
    }

    // formants move on their own, the pitch stays
    {
        const auto base = runVoiceChanger (voice, sr, 0.f, 0.f);
        const auto up = runVoiceChanger (voice, sr, 0.f, 4.f);
        const auto down = runVoiceChanger (voice, sr, 0.f, -4.f);
        const double c0 = centroid (bandDb (base, sr, 1.0, 3.5)), cu = centroid (bandDb (up, sr, 1.0, 3.5)), cd = centroid (bandDb (down, sr, 1.0, 3.5));
        CHECK (cu > c0 * 1.06, "formants +4: the spectrum moves up (%.0f -> %.0f Hz)", c0, cu);
        CHECK (cd < c0 * 0.94, "formants -4: the spectrum moves down (%.0f -> %.0f Hz)", c0, cd);
        CHECK (std::abs (noteMedianMidi (up, sr, 1.0, 3.5) - in) < 0.4 && std::abs (noteMedianMidi (down, sr, 1.0, 3.5) - in) < 0.4,
               "...while the pitch stays where it was (%.2f / %.2f vs %.2f)", noteMedianMidi (up, sr, 1.0, 3.5), noteMedianMidi (down, sr, 1.0, 3.5), in);
        CHECK (allFinite (up, 2.f) && allFinite (down, 2.f), "formant shifts stay bounded");
    }

    // pitch + formants together (a deeper, bigger voice) and a changing voice
    {
        const auto seq = vowelVoice (sr, 130.0, { 0, 3, 1, 4, 2 }, 0.8);
        const auto out = runVoiceChanger (seq, sr, -5.f, -3.f);
        CHECK (allFinite (out, 2.f) && rmsOf (out, sr, 0.5, 3.5) > 0.03, "deeper-and-bigger on a changing vowel sequence: audible and bounded");
    }

    // transposing and then snapping to a scale
    {
        const uint16_t cMajor = (uint16_t) ((1u << 0) | (1u << 2) | (1u << 4) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 11));
        const auto v = vowelVoice (sr, 164.8, { 0, 0, 0, 0 }, 1.0);        // about E3
        const auto out = runVoiceChanger (v, sr, 3.f, 0.f, cMajor, 1.f);    // E3 + 3 = G3, which is in C major
        const double m = noteMedianMidi (out, sr, 1.0, 3.5);
        CHECK (std::abs (m - std::round (m)) < 0.3 && cMajor & (1u << (((int) std::lround (m)) % 12)), "transpose then snap lands on a scale note (%.2f)", m);
    }

    // in the plug-in
    {
        AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
        p.setParam ("vcOn", 1.f); p.setParam ("vcPitch", 5.f); p.setParam ("vcFormant", 2.f);
        std::vector<float> out (voice.size());
        const int B = 512;
        for (size_t pos = 0; pos + B <= voice.size(); pos += B)
        {
            juce::AudioBuffer<float> b (2, B);
            for (int i = 0; i < B; ++i) { b.setSample (0, i, voice[pos + (size_t) i]); b.setSample (1, i, voice[pos + (size_t) i]); }
            juce::MidiBuffer m; p.processBlock (b, m);
            for (int i = 0; i < B; ++i) out[pos + (size_t) i] = 0.5f * (b.getSample (0, i) + b.getSample (1, i));
        }
        const double m = noteMedianMidi (out, sr, 1.0, 3.5);
        CHECK (std::abs (m - (in + 5.0)) < 0.6, "in the plug-in: +5 semitones (%.2f -> %.2f)", in, m);
        {
            const double dbDiff = 20.0 * std::log10 (rmsOf (out, sr, 1.5, 3.5) / rmsOf (voice, sr, 1.5, 3.5));
            CHECK (std::abs (dbDiff) < 3.0, "...and the voice stays as loud as it was (%+.1f dB)", dbDiff);
        }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        CHECK (p.getLatencySamples() >= 1412, "the plug-in reports latency while the voice changer is on (%d)", p.getLatencySamples());
        p.setParam ("vcOn", 0.f);
        std::vector<float> dummy (512, 0.f);
        juce::AudioBuffer<float> b (2, 512); b.clear(); juce::MidiBuffer mm;
        for (int k = 0; k < 4; ++k) p.processBlock (b, mm);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
        CHECK (p.getLatencySamples() < 400, "...and drops it again when it is off (%d)", p.getLatencySamples());
        for (int i = 0; i < AIOProcessor::voicePresetCount(); ++i)
        {
            AIOProcessor q; q.setPlayConfigDetails (2, 2, sr, 512); q.prepareToPlay (sr, 512);
            q.applyVoicePreset (i);
            std::vector<float> o2 (voice.size());
            for (size_t pos = 0; pos + B <= voice.size(); pos += B)
            {
                juce::AudioBuffer<float> bb (2, B);
                for (int j = 0; j < B; ++j) { bb.setSample (0, j, voice[pos + (size_t) j]); bb.setSample (1, j, voice[pos + (size_t) j]); }
                juce::MidiBuffer m2; q.processBlock (bb, m2);
                for (int j = 0; j < B; ++j) o2[pos + (size_t) j] = 0.5f * (bb.getSample (0, j) + bb.getSample (1, j));
            }
            CHECK (allFinite (o2, 2.f) && rmsOf (o2, sr, 1.0, 3.5) > 0.02, "voice preset '%s': audible and bounded", AIOProcessor::voicePresetName (i));
        }
    }

    // tone controls
    {
        juce::Random rnd (7);
        std::vector<float> noise (44100 * 3);
        for (auto& v : noise) v = (rnd.nextFloat() * 2.f - 1.f) * 0.1f;
        auto run = [&] (float warmth, float air)
        {
            AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
            p.setParam ("vcOn", 1.f); p.setParam ("vcWarmth", warmth); p.setParam ("vcAir", air);
            std::vector<float> out (noise.size());
            for (size_t pos = 0; pos + 512 <= noise.size(); pos += 512)
            {
                juce::AudioBuffer<float> b (2, 512);
                for (int i = 0; i < 512; ++i) { b.setSample (0, i, noise[pos + (size_t) i]); b.setSample (1, i, noise[pos + (size_t) i]); }
                juce::MidiBuffer m; p.processBlock (b, m);
                for (int i = 0; i < 512; ++i) out[pos + (size_t) i] = b.getSample (0, i);
            }
            return out;
        };
        const auto flat = run (0.f, 0.f), warm = run (100.f, 0.f), air = run (0.f, 100.f);
        auto lowShare = [&] (const std::vector<float>& x) { double e = 0, a = 0; for (size_t i = 50000; i + 128 < x.size(); i += 128) { double s = 0; for (int k = 0; k < 128; ++k) s += x[i + (size_t) k]; a += s * s / 128.0; for (int k = 0; k < 128; ++k) e += (double) x[i + (size_t) k] * x[i + (size_t) k]; } return a / e; };
        CHECK (lowShare (warm) > lowShare (flat) * 1.5, "warmth adds low end (%.3f -> %.3f)", lowShare (flat), lowShare (warm));
        CHECK (hfRatio (air, 50000, 130000) > hfRatio (flat, 50000, 130000) * 1.3, "air adds high end (%.3f -> %.3f)", hfRatio (flat, 50000, 130000), hfRatio (air, 50000, 130000));
    }
}

static void testLayerProcessor()
{
    std::printf ("\n-- layers in the plug-in --\n");
    const double sr = 44100.0;
    auto make = [&] (std::unique_ptr<AIOProcessor>& p) { p = std::make_unique<AIOProcessor>(); p->setPlayConfigDetails (2, 2, sr, 512); p->prepareToPlay (sr, 512); };
    auto render = [&] (AIOProcessor& p, double secs, const std::vector<std::pair<double, juce::MidiMessage>>& midi)
    {
        const int B = 512;
        const size_t total = (size_t) (secs * sr) / B * B;
        std::vector<float> out (total);
        for (size_t pos = 0; pos < total; pos += B)
        {
            juce::AudioBuffer<float> b (2, B); b.clear();
            juce::MidiBuffer m;
            for (const auto& e : midi)
            {
                const int at = (int) std::lround (e.first * sr);
                if (at >= (int) pos && at < (int) pos + B) m.addEvent (e.second, at - (int) pos);
            }
            p.processBlock (b, m);
            for (int i = 0; i < B; ++i) out[pos + (size_t) i] = 0.5f * (b.getSample (0, i) + b.getSample (1, i));
        }
        return out;
    };

    std::unique_ptr<AIOProcessor> p;
    make (p);
    p->setParam ("lyOn", 1.f);
    {
        const auto out = render (*p, 3.0, { { 0.5, juce::MidiMessage::noteOn (1, 57, (juce::uint8) 100) }, { 2.0, juce::MidiMessage::noteOff (1, 57) } });
        CHECK (rmsOf (out, sr, 0.0, 0.45) < 1.0e-4, "layers: silent before the first key");
        CHECK (rmsOf (out, sr, 1.0, 1.9) > 0.01, "layers: MIDI note-on plays (rms %.3f)", rmsOf (out, sr, 1.0, 1.9));
        CHECK (allFinite (out, 2.f), "layers: bounded through the whole chain");
    }
    {
        make (p); p->setParam ("lyOn", 1.f); p->setParam ("lyRelease", 25.f);
        const auto out = render (*p, 4.0, { { 0.2, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) }, { 1.0, juce::MidiMessage::noteOff (1, 60) } });
        CHECK (rmsOf (out, sr, 0.5, 0.95) > 0.01 && rmsOf (out, sr, 3.2, 3.9) < 0.003, "layers: a MIDI note-off releases the sound (%.4f)", rmsOf (out, sr, 3.2, 3.9));
    }
    {
        make (p); p->setParam ("lyOn", 1.f);
        p->setLayerKey (64);
        const auto a = render (*p, 1.5, {});
        p->setLayerKey (-1);
        p->setParam ("lyRelease", 25.f);
        const auto b = render (*p, 4.0, {});
        CHECK (rmsOf (a, sr, 0.6, 1.4) > 0.01, "layers: the on-screen key plays");
        CHECK (rmsOf (b, sr, 3.0, 3.9) < 0.003, "layers: letting go of the on-screen key releases it");
    }
    {
        make (p); p->setParam ("lyOn", 0.f);
        const auto out = render (*p, 1.5, { { 0.1, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) } });
        CHECK (rmsOf (out, sr, 0.5, 1.4) < 1.0e-4, "layers off: MIDI makes no sound");
    }
    {
        make (p); p->setParam ("lyOn", 1.f); p->setParam ("lySource", 1.f);       // Compose chords
        p->regeneratePattern();
        p->previewPlaying.store (true);
        const auto out = render (*p, 4.0, {});
        p->previewPlaying.store (false);
        CHECK (rmsOf (out, sr, 0.5, 3.8) > 0.01, "layers play the Compose chords (rms %.3f)", rmsOf (out, sr, 0.5, 3.8));
        CHECK (allFinite (out, 2.f), "...and stay bounded");
    }

    // sounds, locking, randomise
    {
        make (p);
        p->setParam ("ly0Lock", 1.f); p->setParam ("ly0Src", 5.f); p->setParam ("ly0Level", -3.f);
        const int before = (int) p->apvts.getRawParameterValue ("ly0Src")->load();
        p->applyLayerPreset (3);
        CHECK ((int) p->apvts.getRawParameterValue ("ly0Src")->load() == before && std::abs (p->apvts.getRawParameterValue ("ly0Level")->load() + 3.f) < 0.05, "a locked layer survives loading a sound");
        p->setParam ("ly0Lock", 0.f);
        p->applyLayerPreset (0);
        const auto s = p->readLayerSettings();
        const auto& pr = aio::layerPresets()[0];
        bool same = s.layer[0].on == pr.layer[0].on && s.layer[0].src == pr.layer[0].src && std::abs (s.layer[0].levelDb - pr.layer[0].levelDb) < 0.1f
                    && std::abs (s.layer[1].cutoffHz - pr.layer[1].cutoffHz) < 5.f && s.on;
        CHECK (same, "loading a sound sets every layer's settings and switches the layers on");
        p->setParam ("ly2Lock", 1.f); p->setParam ("ly2Src", 3.f); p->setParam ("ly2On", 1.f);
        for (int k = 0; k < 20; ++k)
        {
            p->randomizeLayers();
            const auto r = p->readLayerSettings();
            int on = 0; for (auto& c : r.layer) on += c.on ? 1 : 0;
            if (on < 3 || on > 7 || r.layer[2].src != 3) { CHECK (false, "randomise: %d layers on, locked layer kept (src %d)", on, r.layer[2].src); break; }
            if (k == 19) CHECK (true, "randomise: 20 times, always 3..7 layers sound and the locked layer is untouched");
        }
        // each randomised sound plays something sane
        int silent = 0;
        for (int k = 0; k < 6; ++k)
        {
            p->randomizeLayers(); p->setParam ("lyOn", 1.f);
            const auto o = render (*p, 2.0, { { 0.0, juce::MidiMessage::noteOn (1, 52, (juce::uint8) 100) } });
            if (! allFinite (o, 2.f) || rmsOf (o, sr, 0.6, 1.9) < 0.003) ++silent;
        }
        CHECK (silent == 0, "randomised sounds are all audible and bounded");
    }

    // loading a sample from disk and saving it inside the state
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("aio_layer_test");
        dir.createDirectory();
        const auto wav = dir.getChildFile ("sine220.wav");
        wav.deleteFile();
        {
            juce::WavAudioFormat wf;
            auto* fos = new juce::FileOutputStream (wav);
            std::unique_ptr<juce::AudioFormatWriter> w (wf.createWriterFor (fos, 44100.0, 2, 16, {}, 0));
            juce::AudioBuffer<float> b (2, 44100);
            for (int i = 0; i < 44100; ++i) { const float v = 0.4f * (float) std::sin (2.0 * aio::kPi * 220.0 * i / 44100.0); b.setSample (0, i, v); b.setSample (1, i, v * 0.5f); }
            w->writeFromAudioSampleBuffer (b, 0, 44100);
        }
        make (p);
        juce::String err;
        CHECK (p->loadLayerSample (4, wav, err), "load a WAV into layer 5 (%s)", err.toRawUTF8());
        CHECK (p->layerSampleName (4) == "sine220" && (int) p->apvts.getRawParameterValue ("ly4Src")->load() == 6 && p->apvts.getRawParameterValue ("ly4On")->load() > 0.5f,
              "...it names the layer and switches it to Sample");
        CHECK (! p->loadLayerSample (4, dir.getChildFile ("missing.wav"), err) && err.isNotEmpty(), "a missing file is refused with a message");
        for (int i = 0; i < 9; ++i) p->setParam ("ly" + juce::String (i) + "On", i == 4 ? 1.f : 0.f);
        p->setParam ("ly4Loop", 1.f); p->setParam ("ly4Cutoff", 18000.f); p->setParam ("ly4Attack", 5.f);
        p->setParam ("lyOn", 1.f);
        const auto o = render (*p, 2.0, { { 0.0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) } });
        CHECK (rmsOf (o, sr, 0.5, 1.9) > 0.01, "the loaded sample plays from the keys (rms %.3f)", rmsOf (o, sr, 0.5, 1.9));

        juce::MemoryBlock state; p->getStateInformation (state);
        CHECK (state.getSize() > 10000, "the sample travels inside the project state (%d bytes)", (int) state.getSize());
        std::unique_ptr<AIOProcessor> q; make (q);
        q->setStateInformation (state.getData(), (int) state.getSize());
        CHECK (q->layerSampleName (4) == "sine220", "...and comes back when the project is reopened");
        const auto o2 = render (*q, 2.0, { { 0.0, juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100) } });
        CHECK (rmsOf (o2, sr, 0.5, 1.9) > 0.01 && std::abs (rmsOf (o2, sr, 0.5, 1.9) - rmsOf (o, sr, 0.5, 1.9)) < rmsOf (o, sr, 0.5, 1.9) * 0.2, "...sounding the same (%.3f vs %.3f)", rmsOf (o2, sr, 0.5, 1.9), rmsOf (o, sr, 0.5, 1.9));
        q->clearLayerSample (4);
        CHECK (q->layerSampleName (4).isEmpty(), "a sample can be removed");
        dir.deleteRecursively();
    }

    // a factory preset (the host program menu) leaves the layer and voice-changer settings alone
    {
        make (p);
        p->applyLayerPreset (4); p->setParam ("vcPitch", 4.f);
        const auto before = p->readLayerSettings();
        p->setCurrentProgram (2);
        const auto after = p->readLayerSettings();
        CHECK (before.layer[0].src == after.layer[0].src && std::abs (before.bright - after.bright) < 0.01f && std::abs (p->apvts.getRawParameterValue ("vcPitch")->load() - 4.f) < 0.01f,
              "the host's preset menu leaves the layers and the voice changer as they were");
    }
}

static void testLayerUI()
{
    std::printf ("\n-- layers and voice morph screens (synthetic mouse) --\n");
    const double sr = 44100.0;
    AIOProcessor p; p.setPlayConfigDetails (2, 2, sr, 512); p.prepareToPlay (sr, 512);
    p.setParam ("lyOn", 1.f); p.setParam ("lyRelease", 25.f);
    auto run = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b) { juce::AudioBuffer<float> buf (2, 512); buf.clear(); juce::MidiBuffer m; p.processBlock (buf, m); }
    };
    {
        aio::ui::LayerKeys keys (p);
        keys.setBounds (0, 0, 900, 120);
        p.setParam ("lySource", 2.f);
        keys.mouseDown (mouseEv (keys, keys.pointFor (60)));
        CHECK ((int) p.apvts.getRawParameterValue ("lySource")->load() == 0, "pressing a layer key switches the notes to 'MIDI / keys'");
        CHECK (keys.heldNote() == 60, "C4 is held");
        run (20);
        CHECK (p.getLayers().nActive.load() == 1, "one voice sounds while the key is down (%d)", p.getLayers().nActive.load());
        keys.mouseDrag (mouseEv (keys, keys.pointFor (64), 0, true, keys.pointFor (60)));
        run (20);
        CHECK (keys.heldNote() == 64, "dragging moves to the next key");
        keys.mouseUp (mouseEv (keys, keys.pointFor (64), 0, true, keys.pointFor (60)));
        run (60);
        CHECK (p.getLayers().nActive.load() == 0, "letting go ends the sound (%d)", p.getLayers().nActive.load());
        p.setParam ("lyHold", 1.f);
        keys.mouseDown (mouseEv (keys, keys.pointFor (67))); keys.mouseUp (mouseEv (keys, keys.pointFor (67)));
        run (20);
        CHECK (p.getLayers().nActive.load() == 1, "latch keeps the note after the mouse is released");
        keys.mouseDown (mouseEv (keys, keys.pointFor (67))); keys.mouseUp (mouseEv (keys, keys.pointFor (67)));
        run (60);
        CHECK (p.getLayers().nActive.load() == 0, "a second click un-latches it");
        p.setParam ("lyHold", 0.f);
    }
#if ! JucePlugin_IsSynth
    {
        aio::ui::CharacterPad pad (p);
        pad.setBounds (0, 0, 500, 480);
        pad.mouseDown (mouseEv (pad, { 250.f, 12.f }));
        CHECK (p.apvts.getRawParameterValue ("vcPitch")->load() > 11.5f && std::abs (p.apvts.getRawParameterValue ("vcFormant")->load()) < 0.3f, "top centre of the map: pitch up, throat unchanged (%.1f / %.1f)", p.apvts.getRawParameterValue ("vcPitch")->load(), p.apvts.getRawParameterValue ("vcFormant")->load());
        CHECK (p.apvts.getRawParameterValue ("vcOn")->load() > 0.5f, "touching the map switches the voice morph on");
        pad.mouseDrag (mouseEv (pad, { 488.f, 468.f }, 0, true, { 250.f, 12.f }));
        CHECK (p.apvts.getRawParameterValue ("vcPitch")->load() < -8.5f && p.apvts.getRawParameterValue ("vcFormant")->load() < -7.5f, "bottom right: lower voice, smaller throat (%.1f / %.1f)", p.apvts.getRawParameterValue ("vcPitch")->load(), p.apvts.getRawParameterValue ("vcFormant")->load());
        pad.mouseDoubleClick (mouseEv (pad, { 100.f, 100.f }));
        CHECK (std::abs (p.apvts.getRawParameterValue ("vcPitch")->load()) < 0.01f && std::abs (p.apvts.getRawParameterValue ("vcFormant")->load()) < 0.01f, "double-click resets it");
    }
#endif
    {
        AIOEditor ed (p);
        CHECK (ed.layerKeys() != nullptr && ed.layersTabIndex() >= 5, "the LAYERS tab exists (tab %d)", ed.layersTabIndex());
#if ! JucePlugin_IsSynth
        CHECK (ed.characterPad() != nullptr && ed.morphTabIndex() > ed.layersTabIndex(), "the VOICE MORPH tab exists (tab %d)", ed.morphTabIndex());
#endif
        ed.showTab (ed.layersTabIndex());
        CHECK (ed.layerKeys()->isShowing() || ed.layerKeys()->isVisible(), "the layers keyboard is on the screen");
    }
}
