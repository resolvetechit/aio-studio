#include "PluginProcessor.h"
#include <cstring>
#include "PluginEditor.h"
#include "Presets.h"

using namespace aio;

// ------------------------------------------------------------------------------------------------
// Per-block transport / mode information handed to processChunk
struct AIOProcessor::Tx
{
    bool fileMode = false, fileOriginal = false;
    bool composeRun = false, singRun = false;
    bool tunerActive = false, assist = false, guideOn = false;
    float guideLevel = 0.5f;
    float singGateDb = -55.f, singMinHz = 100.f, singMaxHz = 800.f;
    double beat0 = 0.0, bps = 0.0, bpm = 120.0;
    const SingTrack* st = nullptr;
};

AIOProcessor::BusesProperties AIOProcessor::makeBuses()
{
    BusesProperties b;
#if JucePlugin_IsSynth
    return b.withOutput ("Output", juce::AudioChannelSet::stereo(), true);
#else
    return b.withInput ("Input", juce::AudioChannelSet::stereo(), true)
            .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
#endif
}

AIOProcessor::AIOProcessor()
    : AudioProcessor (makeBuses()),
      apvts (*this, nullptr, "AIO_STATE", createLayout())
{
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
        {
            const auto id = r->getParameterID();
            if ((id.startsWith ("cmp") && id != "cmpOn") || id == "singSource")
                apvts.addParameterListener (id, this);
        }

    for (int i = 0; i < 6; ++i)
    {
        static const char* what[5] = { "On", "Type", "Freq", "Gain", "Q" };
        for (int k = 0; k < 5; ++k) eqPtr[(size_t) i][(size_t) k] = apvts.getRawParameterValue (eqId (i, what[k]));
    }
    for (int i = 0; i < 12; ++i) notePtr[(size_t) i] = apvts.getRawParameterValue ("tuneNote" + juce::String (i));

    master.onWake = [this] { triggerAsyncUpdate(); };
    regeneratePattern();
}

AIOProcessor::~AIOProcessor()
{
    cancelPendingUpdate();
    master.onWake = nullptr;
    for (auto* p : getParameters())
        if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.removeParameterListener (r->getParameterID(), this);
}

bool AIOProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    if (l.getMainOutputChannelSet() != juce::AudioChannelSet::stereo()) return false;
#if JucePlugin_IsSynth
    return true;
#else
    return l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
#endif
}

void AIOProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    chunkSize = std::max (samplesPerBlock, 1024);

    chain.prepare (sampleRate, chunkSize);
    synth.prepare (sampleRate);
    synthBuf.setSize (2, chunkSize, false, true, false);
    singDet.prepare (sampleRate);
    guide.prepare (sampleRate);
    learn.prepare (sampleRate);
    master.prepare (sampleRate);

    tunerWasOn = false;
    wasRunning = wasComposeRun = false;
    medN = 0;
    std::memset (outActive, 0, sizeof (outActive));
    reportedLatency = 0;
    setLatencySamples (0);
}

void AIOProcessor::setParam (const juce::String& id, float v)
{
    if (auto* p = apvts.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v));
}

// ------------------------------------------------------------------------------------------------
uint16_t AIOProcessor::getEffectiveTunerMask() const
{
    int key = (int) f ("tuneKey");
    int sc  = (int) f ("tuneScale");
    if (f ("tuneLink") > 0.5f) { key = (int) f ("cmpKey"); sc = (int) f ("cmpScale") + 1; }

    if (sc <= 0) return 0x0FFF;                                         // chromatic
    if (sc >= 13)                                                       // custom
    {
        uint16_t m = 0;
        for (int i = 0; i < 12; ++i) if (notePtr[(size_t) i]->load() > 0.5f) m |= (uint16_t) (1u << i);
        return m;
    }
    const auto& sd = scaleDefs()[(size_t) (sc - 1)];
    return PitchCorrector::maskFromScale (key, sd.iv, sd.n);
}

ComposeSettings AIOProcessor::readComposeSettings() const
{
    ComposeSettings s;
    s.key = (int) f ("cmpKey");
    s.scale = (int) f ("cmpScale");
    s.progression = (int) f ("cmpProg");
    s.bars = (int) f ("cmpBars");
    s.chordLen = (int) f ("cmpChordLen");
    s.chordKind = (int) f ("cmpChordKind");
    s.voicing = (int) f ("cmpVoicing");
    s.smoothVoicing = f ("cmpSmooth") > 0.5f;
    s.chordRhythm = (int) f ("cmpRhythm");
    s.chordOct = (int) f ("cmpChordOct");
    s.chordVel = (int) f ("cmpChordVel");
    s.humanize = f ("cmpHuman") * 0.01f;
    s.swing = f ("cmpSwing") * 0.01f;
    s.melodyOn = f ("cmpMelOn") > 0.5f;
    s.melodyDensity = f ("cmpMelDensity") * 0.01f;
    s.melodyStyle = (int) f ("cmpMelStyle");
    s.melodyRange = (int) f ("cmpMelRange");
    s.melodyOct = (int) f ("cmpMelOct");
    s.melodyRests = f ("cmpMelRests") * 0.01f;
    s.melodyVel = (int) f ("cmpMelVel");
    s.bassOn = f ("cmpBassOn") > 0.5f;
    s.bassStyle = (int) f ("cmpBassStyle");
    s.chordSeed = chordSeed.load();
    s.melodySeed = melodySeed.load();
    return s;
}

void AIOProcessor::regeneratePattern()
{
    auto p = std::make_shared<const Pattern> (generatePattern (readComposeSettings()));
    ring[(size_t) ringPos] = p;
    ringPos = (ringPos + 1) % (int) ring.size();
    uiPattern = p;
    activePattern.store (p.get());
    rebuildSingTrack();
}

void AIOProcessor::rebuildSingTrack()
{
    std::shared_ptr<const SingTrack> t;
    if ((int) f ("singSource") == 1 && importedTrack && ! importedTrack->empty()) t = importedTrack;
    else if (uiPattern) t = std::make_shared<const SingTrack> (trackFromPattern (*uiPattern));
    if (! t) t = std::make_shared<const SingTrack>();
    singRing[(size_t) singRingPos] = t;
    singRingPos = (singRingPos + 1) % (int) singRing.size();
    uiSingTrack = t;
    activeSing.store (t.get());
}

void AIOProcessor::newChordSeed()  { chordSeed  = (uint32_t) juce::Random::getSystemRandom().nextInt (1000000) + 1; regeneratePattern(); }
void AIOProcessor::newMelodySeed() { melodySeed = (uint32_t) juce::Random::getSystemRandom().nextInt (1000000) + 1; regeneratePattern(); }

void AIOProcessor::parameterChanged (const juce::String& id, float)
{
    juce::ignoreUnused (id);
    patternDirty = true;
    triggerAsyncUpdate();
}

void AIOProcessor::handleAsyncUpdate()
{
    if (patternDirty.exchange (false)) regeneratePattern();
    const int lat = pendingLatency.exchange (-1);
    if (lat >= 0 && lat != getLatencySamples()) setLatencySamples (lat);

    MasterSettings s; RenderStats st;
    if (master.takeResult (s, st)) applyMasterSettings (s);
}

int AIOProcessor::readAnalyzer (float* dest, int maxSamples)
{
    const int n = std::min (maxSamples, anaFifo.getNumReady());
    if (n <= 0) return 0;
    int s1, n1, s2, n2;
    anaFifo.prepareToRead (n, s1, n1, s2, n2);
    if (n1 > 0) std::memcpy (dest, anaData.data() + s1, (size_t) n1 * sizeof (float));
    if (n2 > 0) std::memcpy (dest + n1, anaData.data() + s2, (size_t) n2 * sizeof (float));
    anaFifo.finishedRead (n1 + n2);
    return n1 + n2;
}

// ------------------------------------------------------------------------------------------------
int AIOProcessor::gatherEvents (const Pattern& p, double b0, double b1, int n, Ev* out, int maxEv) const
{
    const double L = p.lengthBeats;
    if (L <= 0 || b1 <= b0) return 0;
    const int c0 = (int) std::floor (b0 / L) - 1, c1 = (int) std::floor (b1 / L);
    int count = 0;
    const double inv = (double) n / (b1 - b0);

    auto push = [&] (double t, bool on, const NoteEvent& ne)
    {
        if (count >= maxEv) return;
        int s = (int) ((t - b0) * inv);
        s = std::clamp (s, 0, n - 1);
        out[count++] = { s, on, ne.part, ne.pitch, ne.vel };
    };

    for (int c = c0; c <= c1; ++c)
        for (const auto& ne : p.notes)
        {
            const double ts = c * L + ne.start, te = ts + ne.len;
            if (ts >= b0 && ts < b1) push (ts, true, ne);
            if (te >= b0 && te < b1) push (te, false, ne);
        }

    // order: by sample, note-offs before note-ons at the same sample (insertion sort: stable, no allocation)
    auto before = [] (const Ev& a, const Ev& b)
    {
        if (a.sample != b.sample) return a.sample < b.sample;
        return (! a.on) && b.on;
    };
    for (int i = 1; i < count; ++i)
    {
        const Ev key = out[i];
        int j = i - 1;
        while (j >= 0 && before (key, out[j])) { out[j + 1] = out[j]; --j; }
        out[j + 1] = key;
    }
    return count;
}

void AIOProcessor::releaseAllNotes (juce::MidiBuffer& midi, int sample)
{
    synth.allNotesOff();
#if JucePlugin_ProducesMidiOutput
    for (int part = 0; part < 3; ++part)
        for (int n = 0; n < 128; ++n)
            if (outActive[part][n])
            {
                midi.addEvent (juce::MidiMessage::noteOff (part + 1, n), sample);
                outActive[part][n] = false;
            }
#else
    juce::ignoreUnused (midi, sample);
#endif
}

// ------------------------------------------------------------------------------------------------
ChainParams AIOProcessor::readChainParams (double bpm) const
{
    ChainParams cp;
    cp.inGain  = dbToGain (f ("inGain"));
    cp.outGain = dbToGain (f ("outGain"));
    cp.width   = f ("width") * 0.01f;
    cp.monoBassHz = f ("monoBass");
    cp.limOn   = f ("limOn") > 0.5f;
    cp.limCeil = f ("limCeil");

    cp.voxOn = f ("voxOn") > 0.5f;
    cp.vox.on = cp.voxOn;
    cp.vox.mode = (int) f ("voxMode");
    cp.vox.level = f ("voxLevel") * 0.01f;
    cp.vox.tone = f ("voxTone") * 0.01f;
    cp.vox.deess = f ("voxDeEss") * 0.01f;
    cp.vox.comp = f ("voxComp") * 0.01f;
    cp.vox.clean = f ("voxClean") * 0.01f;
    cp.vox.targetDb = f ("voxTarget");

    cp.tuneOn = f ("tuneOn") > 0.5f;
    {
        static const float lo[4] = { 65.f, 100.f, 150.f, 60.f }, hi[4] = { 500.f, 800.f, 1200.f, 1500.f };
        const int v = std::clamp ((int) f ("tuneVoice"), 0, 3);
        cp.tune.on = cp.tuneOn;
        cp.tune.mask = getEffectiveTunerMask();
        cp.tune.retuneMs = f ("tuneSpeed");
        cp.tune.amount = f ("tuneAmount") * 0.01f;
        cp.tune.fineCents = f ("tuneFine");
        cp.tune.minHz = lo[v]; cp.tune.maxHz = hi[v];
        cp.tune.gateDb = f ("tuneGate");
    }

    cp.eqOn = f ("eqOn") > 0.5f;
    for (int i = 0; i < ParametricEQ::kBands; ++i)
    {
        auto& b = cp.eq[(size_t) i];
        const auto& pp = eqPtr[(size_t) i];
        b.on = cp.eqOn && pp[0]->load() > 0.5f;
        b.type = (FilterType) (int) pp[1]->load();
        b.freq = pp[2]->load();
        b.gainDb = pp[3]->load();
        b.q = pp[4]->load();
    }

    cp.compOn = f ("compOn") > 0.5f;
    cp.comp = { f ("compThresh"), f ("compRatio"), f ("compAttack"), f ("compRelease"), f ("compKnee"), f ("compMakeup"), f ("compMix") * 0.01f };

    cp.satOn = f ("satOn") > 0.5f;
    cp.sat = { (SatType) (int) f ("satType"), f ("satDrive"), f ("satTone"), f ("satMix") * 0.01f, f ("satOut") };

    cp.choOn = f ("choOn") > 0.5f;
    cp.choRate = f ("choRate"); cp.choDepth = f ("choDepth") * 0.01f; cp.choMix = f ("choMix") * 0.01f;

    cp.dly = { 0.f, f ("dlyFeedback") * 0.01f, f ("dlyPing") > 0.5f, f ("dlyTone"), f ("dlyMix") * 0.01f };
    cp.dly.timeMs = (f ("dlySync") > 0.5f) ? (float) (delayDivBeats ((int) f ("dlyDiv")) * 60000.0 / bpm) : f ("dlyTime");
    cp.dlyOn = f ("dlyOn") > 0.5f;

    cp.revOn = f ("revOn") > 0.5f;
    cp.rev = { f ("revSize") * 0.01f, f ("revDamp") * 0.01f, f ("revWidth") * 0.01f, f ("revPre"), f ("revLow"), f ("revHigh"), f ("revMix") * 0.01f };

    cp.followOn = f ("mastFollow") > 0.5f;
    cp.followTargetLufs = f ("mastTarget");
    return cp;
}

// ------------------------------------------------------------------------------------------------
void AIOProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int total = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || total == 0) return;

#if JucePlugin_IsSynth
    buffer.clear();
    midi.clear();
#endif

    // ---------------------------------------------------------------- transport
    double bpm = 120.0, hostBeat = 0.0;
    bool hostPlaying = false;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm()) bpm = std::max (20.0, *b);
            hostPlaying = pos->getIsPlaying() && pos->getPpqPosition().hasValue();
            if (hostPlaying) hostBeat = *pos->getPpqPosition();
        }
    lastBpm.store (bpm);

    // ---------------------------------------------------------------- parameters, once per block
    const ChainParams cpFull = readChainParams (bpm);

    const bool fileMode = master.isPlaying();
    const ChainParams cp = fileMode ? cpFull.masterOnly() : cpFull;

    const bool composeOnParam = f ("cmpOn") > 0.5f;
    const bool singWanted = f ("singOn") > 0.5f && JucePlugin_IsSynth == 0;
    const bool assistWanted = singWanted && f ("singAssist") > 0.5f;
    const bool tunerLatencyOn = cpFull.tuneOn || assistWanted;

    Tx tx;
    tx.fileMode = fileMode;
    tx.fileOriginal = fileMode && master.abOriginal.load();
    tx.tunerActive = tunerLatencyOn && ! fileMode;
    tx.assist = assistWanted;
    tx.guideOn = singWanted && f ("singGuide") > 0.5f;
    tx.guideLevel = f ("singGuideLvl") * 0.01f;
    tx.bpm = bpm;
    tx.bps = bpm / (60.0 * currentSampleRate);
    tx.st = activeSing.load();
    {
        static const float lo[4] = { 65.f, 100.f, 150.f, 60.f }, hi[4] = { 500.f, 800.f, 1200.f, 1500.f };
        const int v = std::clamp ((int) f ("tuneVoice"), 0, 3);
        tx.singMinHz = lo[v]; tx.singMaxHz = hi[v]; tx.singGateDb = f ("tuneGate");
    }

    // latency bookkeeping: pitch corrector (while active) + limiter look-ahead (while on)
    if (tx.tunerActive != tunerWasOn)
    {
        if (tx.tunerActive) chain.tuner.reset();
        tunerWasOn = tx.tunerActive;
    }
    {
        const int want = chain.latencySamples (cpFull, tunerLatencyOn);
        if (want != reportedLatency) { reportedLatency = want; pendingLatency = want; triggerAsyncUpdate(); }
    }

    // ---------------------------------------------------------------- composer / sing clock
    const bool preview = previewPlaying.load();
    const bool internalPlay = preview || singPlaying.load();
    const bool composeWanted = composeOnParam || preview;
    const bool clockWanted = composeWanted || singWanted;
    const bool running = clockWanted && (hostPlaying || internalPlay) && ! fileMode;
    const bool composeRun = running && composeWanted;
    tx.composeRun = composeRun;
    tx.singRun = running && singWanted;

    double beat0 = hostPlaying ? hostBeat : previewBeat;
    if (running)
    {
        // continuity check: a jump (loop, seek, start) silences hanging notes
        if (! wasRunning || std::abs (beat0 - expectedBeat) > 0.02) releaseAllNotes (midi, 0);
        else beat0 = expectedBeat;
        if (wasComposeRun && ! composeRun) releaseAllNotes (midi, 0);
    }
    else if (wasRunning)
        releaseAllNotes (midi, 0);

    SynthParams sp;
    sp.wave[0] = (int) f ("synWaveChord"); sp.wave[1] = (int) f ("synWaveMel"); sp.wave[2] = (int) f ("synWaveBass");
    sp.level[0] = f ("synLevelChord") * 0.01f; sp.level[1] = f ("synLevelMel") * 0.01f; sp.level[2] = f ("synLevelBass") * 0.01f;
    if (tx.singRun) sp.level[1] = 0.f;                       // the guide tone replaces the synth melody while singing
    sp.cutoffHz = f ("synCutoff"); sp.attackMs = f ("synAttack"); sp.releaseMs = f ("synRelease");
    synth.setParams (sp);

    float* l = buffer.getWritePointer (0);
    float* r = buffer.getWritePointer (1);

    int pos = 0;
    while (pos < total)
    {
        const int n = std::min (chunkSize, total - pos);
        tx.beat0 = beat0 + pos * tx.bps;
        processChunk (l + pos, r + pos, n, pos, cp, tx, midi);
        pos += n;
    }

    if (running)
    {
        expectedBeat = beat0 + total * tx.bps;
        if (! hostPlaying) previewBeat = expectedBeat;
        if (auto p = activePattern.load()) uiBeat.store (std::fmod (beat0, p->lengthBeats));
        singBeat.store (beat0);
    }
    wasRunning = running;
    wasComposeRun = composeRun;
    composeRunning.store (composeRun);
    singRunning.store (tx.singRun);
    if (! running && ! hostPlaying && ! internalPlay) previewBeat = 0.0;
}

void AIOProcessor::detectSing (const float* l, const float* r, int n, const Tx& tx)
{
    const double latBeats = 0.035 * tx.bps * currentSampleRate;   // the estimate describes the last ~35 ms
    for (int i = 0; i < n; ++i)
    {
        if (! singDet.push (0.5 * ((double) l[i] + (double) r[i]), tx.singGateDb, tx.singMinHz, tx.singMaxHz)) continue;
        float midi = -1.f;
        if (singDet.voiced())
        {
            medBuf[2] = medBuf[1]; medBuf[1] = medBuf[0]; medBuf[0] = singDet.midi();
            if (medN < 3) ++medN;
            double m = medBuf[0];
            if (medN == 3) m = std::max (std::min (medBuf[0], medBuf[1]), std::min (std::max (medBuf[0], medBuf[1]), medBuf[2]));
            midi = (float) m;
        }
        else medN = 0;
        pushPitchFrame (tx.beat0 + (double) i * tx.bps - latBeats, midi);
    }
}

void AIOProcessor::pushPitchFrame (double beat, float midi)
{
    if (pfFifo.getFreeSpace() < 1) return;
    int s1, n1, s2, n2;
    pfFifo.prepareToWrite (1, s1, n1, s2, n2);
    pfData[(size_t) (n1 > 0 ? s1 : s2)] = { beat, midi };
    pfFifo.finishedWrite (1);
}

int AIOProcessor::popPitchFrames (PitchFrame* dest, int maxFrames)
{
    const int n = std::min (maxFrames, pfFifo.getNumReady());
    if (n <= 0) return 0;
    int s1, n1, s2, n2;
    pfFifo.prepareToRead (n, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) dest[i] = pfData[(size_t) (s1 + i)];
    for (int i = 0; i < n2; ++i) dest[n1 + i] = pfData[(size_t) (s2 + i)];
    pfFifo.finishedRead (n1 + n2);
    return n1 + n2;
}

void AIOProcessor::processChunk (float* l, float* r, int n, int chunkOffset, const ChainParams& cp, const Tx& tx, juce::MidiBuffer& midi)
{
    float op = 0.f;

    if (tx.fileMode)
    {
        // ---- file audition: the loaded file replaces the input and goes through the master chain only
        master.readPlayback (l, r, n);
        float pk = 0.f;
        for (int i = 0; i < n; ++i) pk = std::max ({ pk, std::abs (l[i]), std::abs (r[i]) });
        inPeak.store (pk);
        if (tx.fileOriginal) op = chain.processBypass (l, r, n, cp, master.matchGainLin.load());
        else { chain.stageInput (l, r, n, cp); op = chain.stagePost (l, r, n, cp); }
    }
    else
    {
        inPeak.store (chain.stageInput (l, r, n, cp));
        learn.feed (l, r, n);

        if (tx.singRun) detectSing (l, r, n, tx);

        // ---- pitch correction (acts on the incoming audio only); short sub-blocks so "assist" can follow the melody
        if (tx.tunerActive)
        {
            constexpr int kSub = 128;
            for (int off = 0; off < n; off += kSub)
            {
                const int m = std::min (kSub, n - off);
                TunerSettings ts = cp.tune;
                ts.on = true;
                if (tx.assist && tx.singRun && tx.st != nullptr)
                {
                    const int idx = tx.st->noteAt (tx.beat0 + (double) off * tx.bps);
                    ts.forcePc = idx >= 0 ? tx.st->notes[(size_t) idx].pitch % 12 : -1;
                    if (! cp.tuneOn) ts.mask = 0;           // assist only: nothing is corrected outside the melody notes
                }
                else if (! cp.tuneOn) ts.mask = 0;
                chain.tuner.process (l + off, r + off, m, ts);
            }
        }

        // ---- composer -> internal synth (+ MIDI out for the instrument build)
        if (tx.composeRun)
            if (auto* pat = activePattern.load())
            {
                const int cnt = gatherEvents (*pat, tx.beat0, tx.beat0 + n * tx.bps, n, evBuf.data(), (int) evBuf.size());
                for (int i = 0; i < cnt; ++i)
                {
                    const auto& e = evBuf[(size_t) i];
                    const int part = std::clamp (e.part, 0, 2);
                    if (e.on)
                    {
                        synth.noteOn (part, e.note, e.vel);
#if JucePlugin_ProducesMidiOutput
                        midi.addEvent (juce::MidiMessage::noteOn (part + 1, e.note, (juce::uint8) e.vel), chunkOffset + e.sample);
                        outActive[part][e.note] = true;
#endif
                    }
                    else
                    {
                        synth.noteOff (part, e.note);
#if JucePlugin_ProducesMidiOutput
                        midi.addEvent (juce::MidiMessage::noteOff (part + 1, e.note), chunkOffset + e.sample);
                        outActive[part][e.note] = false;
#endif
                    }
                }
            }
        juce::ignoreUnused (midi, chunkOffset);

        {
            float* sl = synthBuf.getWritePointer (0);
            float* sr = synthBuf.getWritePointer (1);
            synth.render (sl, sr, n);
            for (int i = 0; i < n; ++i) { l[i] += sl[i]; r[i] += sr[i]; }
        }

        // ---- guide tone for the Sing tab
        if (tx.singRun && tx.guideOn && tx.st != nullptr)
        {
            constexpr int kSub = 64;
            for (int off = 0; off < n; off += kSub)
            {
                const int m = std::min (kSub, n - off);
                const int idx = tx.st->noteAt (tx.beat0 + (double) off * tx.bps);
                guide.setNote (idx >= 0 ? tx.st->notes[(size_t) idx].pitch : -1);
                guide.render (l + off, r + off, m, tx.guideLevel);
            }
        }

        op = chain.stagePost (l, r, n, cp);
    }
    outPeak.store (op);

    {
        const int toWrite = std::min (n, anaFifo.getFreeSpace());
        if (toWrite > 0)
        {
            int s1, n1, s2, n2;
            anaFifo.prepareToWrite (toWrite, s1, n1, s2, n2);
            for (int i = 0; i < n1; ++i) anaData[(size_t) (s1 + i)] = 0.5f * (l[i] + r[i]);
            for (int i = 0; i < n2; ++i) anaData[(size_t) (s2 + i)] = 0.5f * (l[n1 + i] + r[n1 + i]);
            anaFifo.finishedWrite (n1 + n2);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Auto Master glue (message thread)
MasterOptions AIOProcessor::readMasterOptions() const
{
    MasterOptions o;
    o.style = (int) f ("mastStyle");
    o.targetLufs = f ("mastTarget");
    o.ceilingDb = f ("limCeil");
    o.amount = f ("mastAmount") * 0.01f;
    o.glue = f ("mastGlue") * 0.01f;
    o.warmth = f ("mastWarmth") * 0.01f;
    o.monoBass = f ("mastMonoBass") > 0.5f;
    return o;
}

void AIOProcessor::applyMasterSettings (const MasterSettings& s)
{
    setParam ("eqOn", s.eqOn ? 1.f : 0.f);
    for (int i = 0; i < ParametricEQ::kBands; ++i)
    {
        const auto& b = s.eq[(size_t) i];
        setParam (eqId (i, "On"), b.on ? 1.f : 0.f);
        setParam (eqId (i, "Type"), (float) (int) b.type);
        setParam (eqId (i, "Freq"), b.freq);
        setParam (eqId (i, "Gain"), b.gainDb);
        setParam (eqId (i, "Q"), b.q);
    }
    setParam ("compOn", s.compOn ? 1.f : 0.f);
    setParam ("compThresh", s.comp.threshDb); setParam ("compRatio", s.comp.ratio);
    setParam ("compAttack", s.comp.attackMs); setParam ("compRelease", s.comp.releaseMs);
    setParam ("compKnee", s.comp.kneeDb); setParam ("compMakeup", s.comp.makeupDb); setParam ("compMix", s.comp.mix * 100.f);
    setParam ("satOn", s.satOn ? 1.f : 0.f);
    setParam ("satType", (float) (int) s.sat.type); setParam ("satDrive", s.sat.driveDb); setParam ("satTone", s.sat.toneHz);
    setParam ("satMix", s.sat.mix * 100.f); setParam ("satOut", s.sat.outDb);
    setParam ("monoBass", s.monoBassHz);
    setParam ("limOn", 1.f);
    setParam ("limCeil", s.ceilingDb);
    setParam ("outGain", s.outGainDb);
}

void AIOProcessor::analyseAndMaster() { master.runAutoMaster (readMasterOptions()); }

void AIOProcessor::exportMaster (const juce::File& out, int bitDepth)
{
    master.exportFile (out, readChainParams (lastBpm.load()).masterOnly(), bitDepth);
}

void AIOProcessor::startLearning()
{
    learn.prepare (currentSampleRate);
    learn.start();
}

bool AIOProcessor::finishLearning()
{
    const auto a = learn.stop();
    if (! a.valid || a.seconds < 1.0) return false;
    auto s = deriveMaster (a, readMasterOptions());
    {
        const juce::ScopedLock sl (learnLock);
        learnAnalysis = a; learnSettings = s; haveLearn = true;
    }
    applyMasterSettings (s);
    return true;
}

bool AIOProcessor::getLearnResult (MasterAnalysis& a, MasterSettings& s) const
{
    const juce::ScopedLock sl (learnLock);
    if (! haveLearn) return false;
    a = learnAnalysis; s = learnSettings;
    return true;
}

// ------------------------------------------------------------------------------------------------
juce::String AIOProcessor::importSingMidi (const juce::File& file)
{
    juce::String err;
    auto t = trackFromMidiFile (file, err);
    if (t.empty()) return err.isEmpty() ? juce::String ("No usable notes found") : err;
    importedTrack = std::make_shared<const SingTrack> (std::move (t));
    singLabel = file.getFileName();
    setParam ("singSource", 1.f);
    rebuildSingTrack();
    return {};
}

juce::File AIOProcessor::makeTempMidiFile() const
{
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("AIO Studio Chords + Melody.mid");
    writeMidiFile (f);
    return f;
}

juce::Result AIOProcessor::writeMidiFile (const juce::File& file) const
{
    if (! uiPattern) return juce::Result::fail ("No pattern");
    const int tpq = 480;
    const double bpm = lastBpm.load();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (tpq);

    static const char* names[3] = { "Chords", "Melody", "Bass" };
    for (int part = 0; part < 3; ++part)
    {
        juce::MidiMessageSequence seq;
        if (part == 0)
        {
            auto tempo = juce::MidiMessage::tempoMetaEvent ((int) std::lround (60000000.0 / bpm));
            tempo.setTimeStamp (0);
            seq.addEvent (tempo);
            auto ts = juce::MidiMessage::timeSignatureMetaEvent (4, 4);
            ts.setTimeStamp (0);
            seq.addEvent (ts);
        }
        auto nm = juce::MidiMessage::textMetaEvent (3, names[part]);
        nm.setTimeStamp (0);
        seq.addEvent (nm);

        for (const auto& n : uiPattern->notes)
        {
            if (n.part != part) continue;
            auto on = juce::MidiMessage::noteOn (part + 1, n.pitch, (juce::uint8) n.vel);
            on.setTimeStamp (n.start * tpq);
            auto off = juce::MidiMessage::noteOff (part + 1, n.pitch);
            off.setTimeStamp ((n.start + n.len) * tpq);
            seq.addEvent (on);
            seq.addEvent (off);
        }
        seq.sort();
        seq.updateMatchedPairs();
        mf.addTrack (seq);
    }

    file.deleteFile();
    if (auto os = file.createOutputStream())
    {
        if (mf.writeTo (*os)) return juce::Result::ok();
        return juce::Result::fail ("Could not write MIDI data");
    }
    return juce::Result::fail ("Could not create " + file.getFullPathName());
}

// ------------------------------------------------------------------------------------------------
int AIOProcessor::getNumPrograms() { return numPresets(); }
const juce::String AIOProcessor::getProgramName (int i) { return presetName (i); }

void AIOProcessor::setCurrentProgram (int index)
{
    currentProgram = std::clamp (index, 0, numPresets() - 1);
    applyPreset (apvts, currentProgram);
}

void AIOProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("chordSeed", (int) chordSeed.load(), nullptr);
    state.setProperty ("melodySeed", (int) melodySeed.load(), nullptr);
    state.setProperty ("program", currentProgram, nullptr);
    if (importedTrack && ! importedTrack->empty())
    {
        state.setProperty ("singNotes", juce::String (serializeTrack (*importedTrack)), nullptr);
        state.setProperty ("singLabel", singLabel, nullptr);
    }
    if (auto xml = state.createXml()) copyXmlToBinary (*xml, dest);
}

void AIOProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto vt = juce::ValueTree::fromXml (*xml);
            chordSeed  = (uint32_t) (int) vt.getProperty ("chordSeed", (int) chordSeed.load());
            melodySeed = (uint32_t) (int) vt.getProperty ("melodySeed", (int) melodySeed.load());
            currentProgram = (int) vt.getProperty ("program", 0);
            const auto notes = vt.getProperty ("singNotes", juce::String()).toString();
            if (notes.isNotEmpty())
            {
                auto t = deserializeTrack (notes.toStdString());
                if (! t.empty()) { importedTrack = std::make_shared<const SingTrack> (std::move (t)); singLabel = vt.getProperty ("singLabel", "Imported MIDI").toString(); }
            }
            apvts.replaceState (vt);
            patternDirty = true;
            triggerAsyncUpdate();
        }
}

juce::AudioProcessorEditor* AIOProcessor::createEditor() { return new AIOEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new AIOProcessor(); }
