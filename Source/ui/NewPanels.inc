// Included once from PluginEditor.cpp: the MASTER, VOICE and SING panels (effect build only).

namespace
{
inline juce::String midiName (double midi)
{
    const int n = (int) std::lround (midi);
    return juce::String (aio::pcName (((n % 12) + 12) % 12)) + juce::String (n / 12 - 1);
}

inline juce::String mmss (double s)
{
    const int t = (int) std::lround (std::max (0.0, s));
    return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2);
}

inline juce::String dbText (double v, int dec = 1)
{
    if (v < -150.0) return "--";
    return dec <= 0 ? juce::String (juce::roundToInt (v)) : juce::String (v, dec);
}

// frequency (Hz) -> x in [0,1] on a 25 Hz .. 20 kHz log axis
inline float logX (float hz) { return std::log (std::max (hz, 25.f) / 25.f) / std::log (800.f); }
} // namespace

// =================================================================================================
// MASTER
// =================================================================================================
class AIOEditor::MasterPanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit MasterPanel (AIOProcessor& p)
        : proc (p),
          style (p.apvts, "mastStyle", "Style"),
          target (p.apvts, "mastTarget", "Loudness target", 1, col::accent2),
          amount (p.apvts, "mastAmount", "Tone amount", -1, col::accent2),
          glue (p.apvts, "mastGlue", "Glue", -1, col::accent2),
          warmth (p.apvts, "mastWarmth", "Warmth", -1, col::accent2),
          monoBass (p.apvts, "mastMonoBass", "Mono bass", col::accent2),
          follow (p.apvts, "mastFollow", "FOLLOW LOUDNESS ON THE MASTER", col::accent2)
    {
        loadBtn.setButtonText ("Load audio (WAV / MP3 / FLAC)...");
        runBtn.setButtonText ("AUTO MASTER");
        exportBtn.setButtonText ("Export...");
        playBtn.setButtonText ("PLAY");
        abBtn.setButtonText ("HEAR: MASTERED");
        learnBtn.setButtonText ("LEARN FROM PLAYBACK");
        loopBtn.setButtonText ("LOOP");
        loopBtn.setToggleState (true, juce::dontSendNotification);
        loopBtn.setColour (juce::ToggleButton::tickColourId, col::accent2);
        runBtn.setColour (juce::TextButton::buttonColourId, col::accent2.darker (0.2f));
        runBtn.setColour (juce::TextButton::textColourOffId, col::bg);
        bits.addItem ("16-bit", 1); bits.addItem ("24-bit", 2); bits.addItem ("32-bit float", 3);
        bits.setSelectedId (2, juce::dontSendNotification);

        loadBtn.onClick = [this] { chooseFile(); };
        runBtn.onClick = [this] { proc.analyseAndMaster(); statusNote.clear(); };
        exportBtn.onClick = [this] { chooseExport(); };
        playBtn.onClick = [this]
        {
            auto& m = proc.getMaster();
            m.setPlaying (! m.isPlaying());
        };
        loopBtn.onClick = [this] { proc.getMaster().loop = loopBtn.getToggleState(); };
        abBtn.onClick = [this]
        {
            auto& m = proc.getMaster();
            const bool orig = ! m.abOriginal.load();
            if (info.hasAfter && info.before.valid)
                m.matchGainLin = (float) std::pow (10.0, (info.after.lufs - info.before.lufs) / 20.0);
            m.abOriginal = orig;
            abBtn.setButtonText (orig ? "HEAR: ORIGINAL" : "HEAR: MASTERED");
        };
        learnBtn.onClick = [this]
        {
            if (proc.getLearn().isLearning())
            {
                statusNote = proc.finishLearning() ? "Learned from the live signal and applied to the master chain."
                                                   : "Not enough audio was heard (need at least a few seconds of music).";
            }
            else { proc.startLearning(); statusNote = "Listening - play the song through this plug-in..."; }
        };

        for (auto* c : std::initializer_list<juce::Component*> { &style, &target, &amount, &glue, &warmth, &monoBass, &follow,
                                                                  &loadBtn, &runBtn, &exportBtn, &bits, &playBtn, &abBtn, &learnBtn, &loopBtn })
            addAndMakeVisible (c);
        startTimerHz (20);
    }

    ~MasterPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, left, "Auto Master", col::accent2);
        drawSection (g, wave, "Track", col::accent2);
        drawSection (g, tonal, "Tonal balance", col::accent2);
        drawSection (g, readout, "Before / after", col::accent2);
        drawSection (g, bus, "Master bus", col::accent2);

        g.setFont (juce::Font (juce::FontOptions (12.f)));
        // file + status
        {
            auto r = fileInfoRect;
            g.setColour (col::text);
            g.drawFittedText (info.hasFile ? info.name + "   " + mmss (info.seconds) + "  " + juce::String ((int) info.sampleRate) + " Hz"
                                           : juce::String ("No file loaded"), r.removeFromTop (18), juce::Justification::centredLeft, 1);
            g.setColour (col::dim);
            auto m = statusNote.isNotEmpty() && ! proc.getMaster().busy() ? statusNote : proc.getMaster().message();
            g.drawFittedText (m, r, juce::Justification::topLeft, 3);
        }
        if (proc.getMaster().busy())
        {
            auto pb = progressRect.toFloat();
            g.setColour (col::edge); g.fillRoundedRectangle (pb, 3.f);
            g.setColour (col::accent2); g.fillRoundedRectangle (pb.withWidth (pb.getWidth() * proc.getMaster().progress()), 3.f);
        }

        paintWave (g);
        paintTonal (g);
        paintReadout (g);
        paintBus (g);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        left = b.removeFromLeft (310);
        b.removeFromLeft (10);

        auto l = left.reduced (14, 34);
        loadBtn.setBounds (l.removeFromTop (30)); l.removeFromTop (6);
        fileInfoRect = l.removeFromTop (52);
        progressRect = l.removeFromTop (6); l.removeFromTop (6);
        style.setBounds (l.removeFromTop (42)); l.removeFromTop (4);
        auto k1 = l.removeFromTop (104);
        target.setBounds (k1.removeFromLeft (k1.getWidth() / 2)); amount.setBounds (k1);
        auto k2 = l.removeFromTop (104);
        glue.setBounds (k2.removeFromLeft (k2.getWidth() / 2)); warmth.setBounds (k2);
        monoBass.setBounds (l.removeFromTop (28)); l.removeFromTop (8);
        runBtn.setBounds (l.removeFromTop (38)); l.removeFromTop (8);
        auto ex = l.removeFromTop (30);
        bits.setBounds (ex.removeFromRight (110).reduced (0, 1)); ex.removeFromRight (6);
        exportBtn.setBounds (ex);

        // right column
        const int busH = 118;
        auto busRow = b.removeFromBottom (busH);
        b.removeFromBottom (10);
        wave = b.removeFromTop (172);
        b.removeFromTop (10);
        tonal = b;

        readout = busRow.removeFromLeft (busRow.getWidth() * 55 / 100);
        busRow.removeFromLeft (10);
        bus = busRow;

        auto w = wave.reduced (14, 30);
        auto tr = w.removeFromBottom (30);
        waveRect = w.withTrimmedBottom (6);
        playBtn.setBounds (tr.removeFromLeft (90)); tr.removeFromLeft (8);
        loopBtn.setBounds (tr.removeFromLeft (70)); tr.removeFromLeft (8);
        abBtn.setBounds (tr.removeFromLeft (160));

        auto bs = bus.reduced (14, 30);
        learnBtn.setBounds (bs.removeFromTop (28)); bs.removeFromTop (6);
        follow.setBounds (bs.removeFromTop (28));
        busTextRect = bs;

        tonalPlot = tonal.reduced (16, 34).withTrimmedBottom (4);
    }

    void mouseDown (const juce::MouseEvent& e) override { seekFrom (e); }
    void mouseDrag (const juce::MouseEvent& e) override { seekFrom (e); }

private:
    void seekFrom (const juce::MouseEvent& e)
    {
        auto p = e.getPosition();
        if (! waveRect.expanded (4, 4).contains (p) || ! info.hasFile) return;
        proc.getMaster().seek ((double) (p.x - waveRect.getX()) / (double) std::max (1, waveRect.getWidth()));
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a song or stem to master", juce::File(), "*.wav;*.mp3;*.flac;*.aif;*.aiff;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile())
            {
                proc.getMaster().setPlaying (false);
                statusNote.clear();
                proc.getMaster().loadFile (f);
            }
        });
    }

    void chooseExport()
    {
        if (! info.hasFile) { statusNote = "Load a file first."; return; }
        const auto base = juce::File (info.name).getFileNameWithoutExtension() + " - mastered.wav";
        chooser = std::make_unique<juce::FileChooser> ("Export the mastered file", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (base), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
            static const int depths[3] = { 16, 24, 32 };
            proc.exportMaster (f, depths[juce::jlimit (0, 2, bits.getSelectedId() - 1)]);
            statusNote.clear();
        });
    }

    void timerCallback() override
    {
        auto& m = proc.getMaster();
        if ((tick++ % 3) == 0 || m.state() != lastState)
        {
            info = m.info();
            lastState = m.state();
        }
        juce::String done;
        if (m.takeExportDone (done)) statusNote = done;

        const bool busy = m.busy();
        runBtn.setEnabled (info.hasFile && ! busy);
        exportBtn.setEnabled (info.hasFile && ! busy);
        playBtn.setEnabled (info.hasFile && ! busy);
        loadBtn.setEnabled (! busy);
        abBtn.setEnabled (info.hasFile);
        playBtn.setButtonText (m.isPlaying() ? "STOP" : "PLAY");
        learnBtn.setButtonText (proc.getLearn().isLearning() ? "STOP + APPLY (" + mmss (proc.getLearn().seconds()) + ")" : "LEARN FROM PLAYBACK");
        learnBtn.setToggleState (proc.getLearn().isLearning(), juce::dontSendNotification);
        if (isShowing()) repaint();
    }

    void paintWave (juce::Graphics& g)
    {
        auto r = waveRect.toFloat();
        g.setColour (col::bg.brighter (0.03f)); g.fillRoundedRectangle (r, 6.f);
        if (! info.hasFile || info.waveform.size() < 4)
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (13.f)));
            g.drawText ("Load a song to see it here. Click the waveform to jump around.", r.toNearestInt(), juce::Justification::centred);
            return;
        }
        const int cols = (int) info.waveform.size() / 2;
        const float midY = r.getCentreY(), half = r.getHeight() * 0.5f - 3.f;
        const double pos = proc.getMaster().position();
        for (int x = 0; x < (int) r.getWidth(); ++x)
        {
            const int c = juce::jlimit (0, cols - 1, (int) ((double) x / r.getWidth() * cols));
            const float mn = info.waveform[(size_t) c * 2], mx = info.waveform[(size_t) c * 2 + 1];
            const bool played = (double) x / r.getWidth() < pos;
            g.setColour (played ? col::accent2 : col::accent2.withAlpha (0.45f));
            g.drawVerticalLine ((int) r.getX() + x, midY - mx * half, midY - mn * half + 1.f);
        }
        const float px = r.getX() + (float) pos * r.getWidth();
        g.setColour (col::text);
        g.fillRect (px - 1.f, r.getY(), 2.f, r.getHeight());
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        g.drawText (mmss (pos * info.seconds) + " / " + mmss (info.seconds), r.toNearestInt().reduced (6, 2), juce::Justification::topRight);
    }

    void paintTonal (juce::Graphics& g)
    {
        auto r = tonalPlot.toFloat();
        g.setColour (col::bg.brighter (0.03f)); g.fillRoundedRectangle (r, 6.f);

        MasterAnalysis la; MasterSettings ls; const MasterSettings* s = nullptr; bool fromLearn = false;
        if (info.hasFile && info.hasSettings) s = &info.settings;
        else if (proc.getLearnResult (la, ls)) { s = &ls; fromLearn = true; }

        auto plot = r.reduced (34.f, 14.f);
        plot.removeFromBottom (14.f);
        const float range = 10.f;
        auto yOf = [&] (float db) { return plot.getCentreY() - juce::jlimit (-range, range, db) / range * plot.getHeight() * 0.5f; };
        auto xOf = [&] (float hz) { return plot.getX() + logX (hz) * plot.getWidth(); };

        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        for (float db : { -10.f, -5.f, 0.f, 5.f, 10.f })
        {
            g.setColour (std::abs (db) < 0.01f ? col::edge.brighter (0.4f) : col::edge);
            g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
            g.setColour (col::dim);
            g.drawText (juce::String (db > 0 ? "+" : "") + juce::String ((int) db), (int) r.getX() + 2, (int) yOf (db) - 7, 28, 14, juce::Justification::centredRight);
        }
        for (float hz : { 50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 5000.f, 10000.f })
        {
            g.setColour (col::edge);
            g.drawVerticalLine ((int) xOf (hz), plot.getY(), plot.getBottom());
            g.setColour (col::dim);
            g.drawText (hz >= 1000.f ? juce::String ((int) (hz / 1000.f)) + "k" : juce::String ((int) hz),
                        (int) xOf (hz) - 20, (int) plot.getBottom() + 1, 40, 14, juce::Justification::centred);
        }

        if (s == nullptr)
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (13.f)));
            g.drawText ("Run AUTO MASTER (or Learn from playback) to see how your mix compares to the target curve.", r.toNearestInt(), juce::Justification::centred);
            return;
        }

        juce::Path corr, expect;
        bool first = true;
        for (int j = 0; j < kMB; ++j)
        {
            const float hz = bandCentreHz (j);
            if (hz < 25.f || hz > 19000.f) continue;
            const float x = xOf (hz);
            const float bw = plot.getWidth() / (float) kMB * 0.7f;
            const float e = s->errorDb[(size_t) j], c = s->correctionDb[(size_t) j];
            g.setColour ((std::abs (e) > 3.f ? col::accent2 : col::dim).withAlpha (0.55f));
            const float y0 = yOf (0.f), y1 = yOf (e);
            g.fillRect (x - bw * 0.5f, std::min (y0, y1), bw, std::abs (y1 - y0));
            if (first) { corr.startNewSubPath (x, yOf (c)); expect.startNewSubPath (x, yOf (e + c)); first = false; }
            else       { corr.lineTo (x, yOf (c)); expect.lineTo (x, yOf (e + c)); }
        }
        g.setColour (col::accent);
        g.strokePath (corr, juce::PathStrokeType (2.2f));
        g.setColour (col::text.withAlpha (0.9f));
        g.strokePath (expect, juce::PathStrokeType (1.2f));

        // legend
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        auto lg = r.reduced (40.f, 2.f).removeFromTop (14.f);
        g.setColour (col::accent2); g.fillRect (lg.getX(), lg.getY() + 3.f, 10.f, 8.f);
        g.setColour (col::dim); g.drawText (fromLearn ? "your signal vs target (0 = on target)" : "your mix vs target (0 = on target)", (int) lg.getX() + 14, (int) lg.getY(), 230, 14, juce::Justification::centredLeft);
        g.setColour (col::accent); g.fillRect (lg.getX() + 250.f, lg.getY() + 5.f, 14.f, 3.f);
        g.setColour (col::dim); g.drawText ("EQ correction", (int) lg.getX() + 268, (int) lg.getY(), 100, 14, juce::Justification::centredLeft);
        g.setColour (col::text); g.fillRect (lg.getX() + 372.f, lg.getY() + 6.f, 14.f, 1.5f);
        g.setColour (col::dim); g.drawText ("expected result", (int) lg.getX() + 390, (int) lg.getY(), 110, 14, juce::Justification::centredLeft);
    }

    void paintReadout (juce::Graphics& g)
    {
        auto r = readout.reduced (16, 34);
        g.setFont (juce::Font (juce::FontOptions (13.f)));
        const int c1 = r.getX() + 150, c2 = r.getX() + 250, rowH = 20;
        int y = r.getY();
        g.setColour (col::dim);
        g.drawText ("Before", c1, y, 90, rowH, juce::Justification::centredRight);
        g.drawText ("After", c2, y, 90, rowH, juce::Justification::centredRight);
        y += rowH;
        auto row = [&] (const juce::String& name, const juce::String& a, const juce::String& b)
        {
            g.setColour (col::dim);  g.drawText (name, r.getX(), y, 150, rowH, juce::Justification::centredLeft);
            g.setColour (col::text); g.drawText (a, c1, y, 90, rowH, juce::Justification::centredRight);
            g.setColour (col::accent2); g.drawText (b, c2, y, 90, rowH, juce::Justification::centredRight);
            y += rowH;
        };
        const bool has = info.hasFile && info.before.valid;
        const bool aft = has && info.hasAfter;
        row ("Loudness (LUFS)", has ? dbText (info.before.lufs) : "--", aft ? dbText (info.after.lufs) : "--");
        row ("True peak (dBTP)", has ? dbText (info.before.truePeakDb) : "--", aft ? dbText (info.after.truePeakDb) : "--");
        row ("Peak-to-loudness (dB)", has ? dbText (info.before.truePeakDb - info.before.lufs) : "--", aft ? dbText (info.after.truePeakDb - info.after.lufs) : "--");
    }

    void paintBus (juce::Graphics& g)
    {
        auto r = busTextRect;
        const auto& ch = proc.getChain();
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.setColour (col::dim);
        g.drawText ("Live in / out:", r.getX(), r.getY() + 2, 90, 18, juce::Justification::centredLeft);
        g.setColour (col::text);
        g.drawText (dbText (ch.inLufs.load()) + "  /  " + dbText (ch.outLufs.load()) + " LUFS", r.getX() + 92, r.getY() + 2, 170, 18, juce::Justification::centredLeft);
        if (follow.getToggleState())
        {
            g.setColour (col::accent2);
            g.drawText ("riding " + juce::String (ch.followGainDb.load(), 1) + " dB", r.getRight() - 120, r.getY() + 2, 120, 18, juce::Justification::centredRight);
        }
    }

    AIOProcessor& proc;
    Combo style;
    Knob target, amount, glue, warmth;
    Toggle monoBass, follow;
    juce::TextButton loadBtn, runBtn, exportBtn, playBtn, abBtn, learnBtn;
    juce::ToggleButton loopBtn;
    juce::ComboBox bits;
    juce::Rectangle<int> left, wave, tonal, readout, bus, fileInfoRect, progressRect, waveRect, tonalPlot, busTextRect;
    MasterSession::Info info;
    MasterSession::State lastState = MasterSession::Idle;
    juce::String statusNote;
    int tick = 0;
    std::unique_ptr<juce::FileChooser> chooser;
};

// =================================================================================================
// VOICE
// =================================================================================================
class AIOEditor::VoicePanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit VoicePanel (AIOProcessor& p)
        : proc (p),
          on (p.apvts, "voxOn", "AUTO VOICE ON", col::chords),
          mode (p.apvts, "voxMode", "Source type"),
          targetK (p.apvts, "voxTarget", "Target level", 1, col::chords),
          level (p.apvts, "voxLevel", "Leveler", -1, col::chords),
          tone (p.apvts, "voxTone", "Auto tone", -1, col::chords),
          deess (p.apvts, "voxDeEss", "De-esser", -1, col::chords),
          comp (p.apvts, "voxComp", "Compressor", -1, col::chords),
          clean (p.apvts, "voxClean", "Gap clean-up", -1, col::chords)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &on, &mode, &targetK, &level, &tone, &deess, &comp, &clean })
            addAndMakeVisible (c);
        startTimerHz (24);
    }
    ~VoicePanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, left, "Auto Voice", col::chords);
        drawSection (g, right, "What it is doing right now", col::chords);

        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (12.f)));
        g.drawFittedText ("Put this on a vocal, instrument or podcast channel. It rides the level, tames harsh S sounds, "
                          "nudges the tone toward a healthy balance and compresses - all by listening, so it keeps working "
                          "when the performance changes. No latency. Louder-than-ideal sections come down, quiet sections come up.",
                          noteRect, juce::Justification::topLeft, 8);

        const auto& v = proc.getChain().vox;
        auto r = right.reduced (18, 36);
        const int labW = 140, barH = 14, gap = 12;
        auto bar = [&] (const juce::String& name, float val, float lo, float hi, bool centred, const juce::String& text, juce::Colour c, bool fromRight = false)
        {
            auto row = r.removeFromTop (barH + gap);
            g.setColour (col::dim);
            g.drawText (name, row.getX(), row.getY(), labW, barH, juce::Justification::centredLeft);
            auto b = juce::Rectangle<float> ((float) row.getX() + labW, (float) row.getY(), (float) row.getWidth() - labW - 90.f, (float) barH);
            g.setColour (col::bg); g.fillRoundedRectangle (b, 4.f);
            const float t = juce::jlimit (0.f, 1.f, (val - lo) / (hi - lo));
            g.setColour (c);
            if (centred)
            {
                const float z = (0.f - lo) / (hi - lo);
                const float x0 = b.getX() + b.getWidth() * std::min (z, t), x1 = b.getX() + b.getWidth() * std::max (z, t);
                g.fillRoundedRectangle (x0, b.getY(), std::max (2.f, x1 - x0), b.getHeight(), 4.f);
                g.setColour (col::dim); g.fillRect (b.getX() + b.getWidth() * z - 0.5f, b.getY() - 2.f, 1.f, b.getHeight() + 4.f);
            }
            else if (fromRight) { const float w = std::max (2.f, b.getWidth() * (1.f - t)); g.fillRoundedRectangle (b.getRight() - w, b.getY(), w, b.getHeight(), 4.f); }
            else g.fillRoundedRectangle (b.getX(), b.getY(), std::max (2.f, b.getWidth() * t), b.getHeight(), 4.f);
            g.setColour (col::text);
            g.drawText (text, (int) b.getRight() + 6, row.getY(), 84, barH, juce::Justification::centredLeft);
        };

        const float inDb = v.inRmsDb.load(), nfDb = v.noiseFloorDb.load();
        {
            auto row = r;
            bar ("Input level", inDb, -80.f, 0.f, false, dbText (inDb, 0) + " dB", v.voicedFlag.load() ? col::chords : col::dim);
            // noise-floor marker on the same bar
            const float t = juce::jlimit (0.f, 1.f, (nfDb + 80.f) / 80.f);
            const float bx = (float) row.getX() + labW + t * ((float) row.getWidth() - labW - 90.f);
            g.setColour (col::accent2); g.fillRect (bx - 1.f, (float) row.getY() - 2.f, 2.f, (float) barH + 4.f);
        }
        bar ("Noise floor (orange tick)", nfDb, -80.f, 0.f, false, dbText (nfDb, 0) + " dB", col::edge.brighter (0.3f));
        bar ("Leveler gain", v.levelerDb.load(), -15.f, 15.f, true, juce::String (v.levelerDb.load(), 1) + " dB", col::chords);
        bar ("De-esser", v.deEssDb.load(), -12.f, 0.f, false, juce::String (v.deEssDb.load(), 1) + " dB", col::melody, true);
        bar ("Compressor", proc.getChain().vox.comp.grMeter.load(), -18.f, 0.f, false, juce::String (v.comp.grMeter.load(), 1) + " dB", col::accent2, true);

        g.setColour (v.voicedFlag.load() ? col::accent : col::dim);
        g.fillEllipse ((float) right.getRight() - 150.f, (float) right.getY() + 12.f, 10.f, 10.f);
        g.setFont (juce::Font (juce::FontOptions (12.f, juce::Font::bold)));
        g.drawText (v.voicedFlag.load() ? "VOICE DETECTED" : "waiting for signal", right.getRight() - 136, right.getY() + 8, 130, 18, juce::Justification::centredLeft);

        // tone: measured shape vs target shape vs applied correction
        r.removeFromTop (6);
        g.setColour (col::text);
        g.setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
        g.drawText ("TONE BALANCE (8 octave bands)", r.getX(), r.getY(), 300, 16, juce::Justification::centredLeft);
        r.removeFromTop (20);
        auto plot = r.withTrimmedBottom (22).toFloat();
        g.setColour (col::bg.brighter (0.03f)); g.fillRoundedRectangle (plot, 6.f);
        const auto& mp = aio::AutoVoice::modeParams ((int) proc.apvts.getRawParameterValue ("voxMode")->load());
        const float tref = 0.25f * (mp.shape[2] + mp.shape[3] + mp.shape[4] + mp.shape[5]);
        const float range = 14.f;
        auto yOf = [&] (float db) { return plot.getCentreY() - juce::jlimit (-range, range, db) / range * plot.getHeight() * 0.5f; };
        static const char* names[8] = { "100", "200", "400", "800", "1.6k", "3.2k", "6.4k", "12.8k" };
        const float cw = plot.getWidth() / 8.f;
        g.setColour (col::edge); g.drawHorizontalLine ((int) yOf (0.f), plot.getX(), plot.getRight());
        juce::Path tgt;
        for (int k = 0; k < 8; ++k)
        {
            const float cx = plot.getX() + cw * ((float) k + 0.5f);
            const float meas = v.bandShape[(size_t) k].load(), app = v.bandApplied[(size_t) k].load();
            g.setColour (col::chords.withAlpha (0.55f));
            const float y0 = yOf (0.f), y1 = yOf (meas - (mp.shape[(size_t) k] - tref) * 0.f);
            g.fillRect (cx - cw * 0.22f, std::min (y0, y1), cw * 0.3f, std::abs (y1 - y0));
            g.setColour (col::accent);
            const float ya = yOf (app);
            g.fillRect (cx + cw * 0.10f, std::min (y0, ya), cw * 0.16f, std::abs (ya - y0));
            const float ty = yOf (mp.shape[(size_t) k] - tref);
            if (k == 0) tgt.startNewSubPath (cx, ty); else tgt.lineTo (cx, ty);
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (11.f)));
            g.drawText (names[k], (int) (cx - cw * 0.5f), (int) plot.getBottom() + 3, (int) cw, 14, juce::Justification::centred);
        }
        g.setColour (col::text.withAlpha (0.85f));
        g.strokePath (tgt, juce::PathStrokeType (1.6f));
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        auto lg = plot.reduced (8.f, 4.f).removeFromTop (14.f);
        g.setColour (col::chords); g.fillRect (lg.getX(), lg.getY() + 3.f, 9.f, 8.f);
        g.setColour (col::dim); g.drawText ("measured", (int) lg.getX() + 13, (int) lg.getY(), 70, 14, juce::Justification::centredLeft);
        g.setColour (col::text); g.fillRect (lg.getX() + 90.f, lg.getY() + 6.f, 14.f, 1.5f);
        g.setColour (col::dim); g.drawText ("target shape", (int) lg.getX() + 108, (int) lg.getY(), 80, 14, juce::Justification::centredLeft);
        g.setColour (col::accent); g.fillRect (lg.getX() + 196.f, lg.getY() + 3.f, 9.f, 8.f);
        g.setColour (col::dim); g.drawText ("EQ being applied", (int) lg.getX() + 210, (int) lg.getY(), 110, 14, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        left = b.removeFromLeft (330);
        b.removeFromLeft (10);
        right = b;

        auto l = left.reduced (16, 36);
        on.setBounds (l.removeFromTop (32)); l.removeFromTop (8);
        mode.setBounds (l.removeFromTop (42)); l.removeFromTop (6);
        Knob* rows[3][2] = { { &targetK, &level }, { &tone, &deess }, { &comp, &clean } };
        for (auto& row : rows)
        {
            auto k = l.removeFromTop (100);
            const int w = k.getWidth() / 2;
            for (int i = 0; i < 2; ++i) row[i]->setBounds (k.getX() + i * w, k.getY(), w, k.getHeight());
        }
        l.removeFromTop (4);
        noteRect = l;
    }

private:
    void timerCallback() override { if (isShowing()) repaint(); }

    AIOProcessor& proc;
    juce::Rectangle<int> left, right, noteRect;
    Toggle on;
    Combo mode;
    Knob targetK, level, tone, deess, comp, clean;
};

// =================================================================================================
// SING
// =================================================================================================
class AIOEditor::SingPanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit SingPanel (AIOProcessor& p)
        : proc (p),
          on (p.apvts, "singOn", "SING MODE", col::melody),
          source (p.apvts, "singSource", "Melody"),
          level (p.apvts, "singLevel", "Difficulty"),
          octave (p.apvts, "singOct", "Any octave", col::melody),
          guide (p.apvts, "singGuide", "Guide tone", col::melody),
          assist (p.apvts, "singAssist", "Assist", col::melody),
          guideLvl (p.apvts, "singGuideLvl", "Guide vol", 0, col::melody)
    {
        playBtn.setButtonText ("PLAY");
        playBtn.setColour (juce::TextButton::buttonColourId, col::melody.darker (0.25f));
        playBtn.setColour (juce::TextButton::textColourOffId, col::bg);
        loadBtn.setButtonText ("Load MIDI...");
        playBtn.onClick = [this]
        {
            const bool now = ! proc.singPlaying.load();
            proc.singPlaying = now;
            if (now) { sk.resetPass(); trail.clear(); pops.clear(); }
        };
        loadBtn.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Pick a MIDI file with a melody to sing", juce::File(), "*.mid;*.midi");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f.existsAsFile()) message = proc.importSingMidi (f);
                if (message.isEmpty() && f.existsAsFile()) message = "Loaded " + f.getFileName();
            });
        };
        for (auto* c : std::initializer_list<juce::Component*> { &on, &source, &level, &octave, &guide, &assist, &guideLvl, &playBtn, &loadBtn })
            addAndMakeVisible (c);
        startTimerHz (30);
    }
    ~SingPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (col::panel); g.fillRoundedRectangle (strip.toFloat(), 8.f);
        g.setColour (col::edge); g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), 8.f, 1.f);
        drawSection (g, scoreBox, "Score", col::melody);
        paintLanes (g);
        paintScore (g);

        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawFittedText (message.isNotEmpty() ? message : trackLabel(), infoRect, juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        strip = b.removeFromTop (74);
        b.removeFromTop (10);
        scoreBox = b.removeFromRight (240);
        b.removeFromRight (10);
        lane = b;

        auto s = strip.reduced (12, 8);
        on.setBounds (s.removeFromLeft (110).withHeight (30).translated (0, 4)); s.removeFromLeft (8);
        playBtn.setBounds (s.removeFromLeft (84).withHeight (30).translated (0, 4)); s.removeFromLeft (14);
        source.setBounds (s.removeFromLeft (150)); s.removeFromLeft (6);
        loadBtn.setBounds (s.removeFromLeft (100).withHeight (26).translated (0, 17)); s.removeFromLeft (14);
        level.setBounds (s.removeFromLeft (110)); s.removeFromLeft (14);
        octave.setBounds (s.removeFromLeft (96).withHeight (26).translated (0, 17)); s.removeFromLeft (6);
        guide.setBounds (s.removeFromLeft (96).withHeight (26).translated (0, 17)); s.removeFromLeft (6);
        assist.setBounds (s.removeFromLeft (80).withHeight (26).translated (0, 17)); s.removeFromLeft (6);
        guideLvl.setBounds (s.removeFromLeft (70).withHeight (58).translated (0, -2)); s.removeFromLeft (8);
        infoRect = s;
    }

private:
    struct Pop { int rating; int pitch; double born; };

    juce::String trackLabel() const
    {
        if (! track) return "No melody yet.";
        const bool imported = (int) proc.apvts.getRawParameterValue ("singSource")->load() == 1 && proc.getSingTrackLabel().isNotEmpty();
        return (imported ? proc.getSingTrackLabel() : juce::String ("Compose melody")) + " - " + juce::String ((int) track->notes.size()) + " notes";
    }

    double toleranceFor (int lvl) const { return lvl == 0 ? 1.5 : (lvl == 2 ? 0.5 : 1.0); }

    void timerCallback() override
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        auto t = proc.getSingTrackForUI();
        if (t.get() != track.get())
        {
            track = t; sk.setTrack (t); trail.clear(); pops.clear();
            rating.assign (track ? track->notes.size() : 0, -1);
            lastTarget = -1;
        }
        cfg.toleranceSemis = (float) toleranceFor ((int) proc.apvts.getRawParameterValue ("singLevel")->load());
        cfg.octaveAgnostic = proc.apvts.getRawParameterValue ("singOct")->load() > 0.5f;

        const bool running = proc.singRunning.load();
        AIOProcessor::PitchFrame buf[256];
        int n;
        while ((n = proc.popPitchFrames (buf, 256)) > 0)
        {
            if (! running || ! isVisible() || ! track) continue;
            for (int i = 0; i < n; ++i)
            {
                const auto& f = buf[i];
                if (! trail.empty() && f.beat < trail.back().beat - 0.5) trail.clear();    // loop wrapped / seek
                sk.addFrame (f.beat, f.midi, cfg);
                float folded = f.midi;
                if (f.midi >= 0.f)
                {
                    const int idx = track->noteAt (f.beat);
                    double ref = idx >= 0 ? track->notes[(size_t) idx].pitch : (lastTarget >= 0 ? lastTarget : 0.5 * (track->lowestPitch() + track->highestPitch()));
                    if (idx >= 0) lastTarget = track->notes[(size_t) idx].pitch;
                    folded = (float) foldNear (f.midi, ref);
                    sungMidi = f.midi; sungFolded = folded; lastVoicedMs = now;
                }
                trail.push_back ({ f.beat, folded });
            }
        }
        while (! trail.empty() && trail.front().beat < dispBeat - 3.0) trail.pop_front();
        while (trail.size() > 1200) trail.pop_front();

        for (auto& e : sk.takeEvents())
        {
            if ((size_t) e.noteIndex < rating.size()) rating[(size_t) e.noteIndex] = e.rating;
            if (track && (size_t) e.noteIndex < track->notes.size()) pops.push_back ({ e.rating, track->notes[(size_t) e.noteIndex].pitch, now });
        }
        while (! pops.empty() && now - pops.front().born > 1400.0) pops.erase (pops.begin());

        if (sk.takePassFinished())
        {
            overlayUntil = now + 3200.0; overlayScore = sk.lastScore();
            best = std::max (best, overlayScore);
        }

        // smooth beat for drawing
        const double sb = proc.singBeat.load();
        const double bps = proc.lastBpm.load() / 60.0;
        if (std::abs (sb - lastSeenBeat) > 1.0e-9) { lastSeenBeat = sb; lastSeenMs = now; }
        const double extra = running ? std::min (0.12, (now - lastSeenMs) * 0.001) * bps : 0.0;
        dispBeat = running ? lastSeenBeat + extra : lastSeenBeat;

        playBtn.setButtonText (proc.singPlaying.load() ? "STOP" : "PLAY");
        if (isShowing()) repaint();
    }

    static double foldNear (double midi, double ref)
    {
        double d = std::fmod (midi - ref, 12.0);
        if (d > 6.0) d -= 12.0;
        if (d < -6.0) d += 12.0;
        return ref + d;
    }

    static juce::Colour ratingColour (int r)
    {
        switch (r) { case RatingPerfect: return juce::Colour (0xffffd24a); case RatingGreat: return col::accent;
                     case RatingGood: return col::chords; default: return juce::Colour (0xff7a4252); }
    }
    static const char* ratingText (int r)
    {
        switch (r) { case RatingPerfect: return "PERFECT!"; case RatingGreat: return "GREAT"; case RatingGood: return "GOOD"; default: return "miss"; }
    }

    void paintLanes (juce::Graphics& g)
    {
        auto full = lane.toFloat();
        g.setColour (col::panel); g.fillRoundedRectangle (full, 8.f);
        g.setColour (col::edge); g.drawRoundedRectangle (full.reduced (0.5f), 8.f, 1.f);

        const bool running = proc.singRunning.load();
        const bool on_ = proc.apvts.getRawParameterValue ("singOn")->load() > 0.5f;
        auto area = full.reduced (10.f, 12.f);
        const float gutter = 38.f;
        auto gut = area.removeFromLeft (gutter);
        juce::ignoreUnused (gut);

        if (! track || track->empty())
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (14.f)));
            g.drawText ("No melody to sing yet. Turn the Compose melody on, or load a MIDI file.", full.toNearestInt(), juce::Justification::centred);
            return;
        }

        int lo = track->lowestPitch() - 2, hi = track->highestPitch() + 2;
        while (hi - lo < 12) { --lo; ++hi; }
        const int rows = hi - lo + 1;
        const float rowH = area.getHeight() / (float) rows;
        auto yOf = [&] (double p) { return area.getBottom() - ((float) p - (float) lo + 0.5f) * rowH; };
        const double window = 8.0, behind = 2.0;
        auto xOf = [&] (double beat) { return area.getX() + (float) ((beat - (dispBeat - behind)) / window) * area.getWidth(); };

        // lane stripes + key labels
        for (int p = lo; p <= hi; ++p)
        {
            const int pc = ((p % 12) + 12) % 12;
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            g.setColour (black ? col::bg.withAlpha (0.55f) : col::bg.brighter (0.02f).withAlpha (0.35f));
            g.fillRect (area.getX(), yOf (p) - rowH * 0.5f, area.getWidth(), rowH);
            if (pc == 0 || rowH > 17.f)
            {
                g.setColour (pc == 0 ? col::text : col::dim);
                g.setFont (juce::Font (juce::FontOptions (10.5f)));
                g.drawText (midiName (p), (int) full.getX() + 8, (int) (yOf (p) - 7.f), 34, 14, juce::Justification::centredRight);
            }
        }
        // beat lines
        for (double b = std::floor (dispBeat - behind); b < dispBeat + window; b += 1.0)
        {
            const float x = xOf (b);
            if (x < area.getX() || x > area.getRight()) continue;
            const bool bar = std::fmod (std::fmod (b, track->lengthBeats) + track->lengthBeats, 4.0) < 1.0e-6;
            g.setColour (bar ? col::edge.brighter (0.35f) : col::edge.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
        }

        const double L = track->lengthBeats;
        const int curLoop = (int) std::floor (dispBeat / L);
        const float capH = juce::jlimit (9.f, 24.f, rowH * 0.82f);
        const int c0 = (int) std::floor ((dispBeat - behind) / L), c1 = (int) std::floor ((dispBeat + window) / L);
        g.saveState();
        g.reduceClipRegion (area.toNearestInt());
        for (int c = c0; c <= c1; ++c)
            for (size_t i = 0; i < track->notes.size(); ++i)
            {
                const auto& nt = track->notes[i];
                const double s = c * L + nt.start, e = s + nt.len;
                if (e < dispBeat - behind || s > dispBeat + window) continue;
                const float x0 = xOf (s), x1 = xOf (e), y = yOf (nt.pitch);
                juce::Rectangle<float> cap (x0 + 1.f, y - capH * 0.5f, std::max (4.f, x1 - x0 - 2.f), capH);

                const bool inCurPass = c == curLoop;
                const bool upcoming = (c > curLoop) || (inCurPass && s > dispBeat);
                const bool done = (c < curLoop) || (inCurPass && e <= dispBeat);
                const bool active = inCurPass && s <= dispBeat && dispBeat < e;
                int r = (done && i < rating.size()) ? rating[i] : -1;

                g.setColour (upcoming ? col::melody.withAlpha (0.20f) : col::panel.brighter (0.15f));
                g.fillRoundedRectangle (cap, capH * 0.5f);
                // lit cells = the parts of the note you actually hit
                if (inCurPass && ! upcoming)
                {
                    const uint16_t cells = sk.hitCells (i);
                    const float cw = cap.getWidth() / 16.f;
                    g.saveState();
                    juce::Path clip; clip.addRoundedRectangle (cap, capH * 0.5f);
                    g.reduceClipRegion (clip);
                    for (int k = 0; k < 16; ++k)
                        if ((cells >> k) & 1)
                        {
                            g.setColour (done && r >= 0 ? ratingColour (r).withAlpha (0.9f) : col::accent.withAlpha (0.95f));
                            g.fillRect (cap.getX() + (float) k * cw, cap.getY(), cw + 0.6f, cap.getHeight());
                        }
                    g.restoreState();
                }
                else if (done && r >= 0)
                {
                    g.setColour (ratingColour (r).withAlpha (0.45f));
                    g.fillRoundedRectangle (cap, capH * 0.5f);
                }
                g.setColour (active ? col::text : (upcoming ? col::melody.withAlpha (0.8f) : (r >= 0 ? ratingColour (r) : col::edge.brighter (0.3f))));
                g.drawRoundedRectangle (cap, capH * 0.5f, active ? 2.2f : 1.4f);
            }

        // trail of what you sang
        {
            juce::Path path;
            bool pen = false; double lastB = 0;
            for (const auto& tp : trail)
            {
                if (tp.beat < dispBeat - behind || tp.beat > dispBeat + 0.05) continue;
                if (tp.midi < 0.f) { pen = false; continue; }
                const float x = xOf (tp.beat), y = yOf (tp.midi);
                if (! pen || tp.beat - lastB > 0.12) path.startNewSubPath (x, y); else path.lineTo (x, y);
                pen = true; lastB = tp.beat;
            }
            g.setColour (col::melody.withAlpha (0.55f));
            g.strokePath (path, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // now line + your voice
        const float nx = xOf (dispBeat);
        g.setColour (col::text.withAlpha (0.85f));
        g.fillRect (nx - 1.f, area.getY(), 2.f, area.getHeight());
        const double nowMs = juce::Time::getMillisecondCounterHiRes();
        if (running && nowMs - lastVoicedMs < 140.0)
        {
            const bool hit = sk.hitting();
            const float y = yOf (sungFolded);
            juce::Rectangle<float> pill (nx - 14.f, y - capH * 0.4f, 28.f, capH * 0.8f);
            g.setColour ((hit ? col::accent : col::melody).withAlpha (0.35f));
            g.fillRoundedRectangle (pill.expanded (5.f), capH * 0.8f);
            g.setColour (hit ? col::accent : col::melody);
            g.fillRoundedRectangle (pill, pill.getHeight() * 0.5f);
        }

        // ratings floating up from the now-line
        for (const auto& p : pops)
        {
            const double age = (nowMs - p.born) / 1400.0;
            g.setColour (ratingColour (p.rating).withAlpha ((float) (1.0 - age)));
            g.setFont (juce::Font (juce::FontOptions (p.rating == RatingPerfect ? 20.f : 16.f, juce::Font::bold)));
            g.drawText (ratingText (p.rating), (int) nx + 14, (int) (yOf (p.pitch) - 30.f - 26.f * (float) age), 140, 24, juce::Justification::centredLeft);
        }
        g.restoreState();

        // hints / overlays
        if (! on_ || ! running)
        {
            g.setColour (col::bg.withAlpha (0.55f));
            g.fillRoundedRectangle (full.reduced (1.f), 8.f);
            g.setColour (col::text);
            g.setFont (juce::Font (juce::FontOptions (17.f, juce::Font::bold)));
            g.drawText (! on_ ? "Switch SING MODE on, then press PLAY" : "Press PLAY (or play in your DAW) and sing into this track's input",
                        full.toNearestInt().withTrimmedBottom (40), juce::Justification::centred);
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.drawText ("The notes scroll in from the right. Match the pitch as each note crosses the white line - the pink bar is your voice.",
                        full.toNearestInt().withTrimmedTop (30), juce::Justification::centred);
        }
        if (nowMs < overlayUntil)
        {
            auto c = full.toNearestInt().withSizeKeepingCentre (440, 150);
            g.setColour (col::bg.withAlpha (0.88f)); g.fillRoundedRectangle (c.toFloat(), 14.f);
            g.setColour (col::melody); g.drawRoundedRectangle (c.toFloat().reduced (1.f), 14.f, 2.f);
            g.setColour (col::text);
            g.setFont (juce::Font (juce::FontOptions (14.f)));
            g.drawText ("PASS COMPLETE", c.removeFromTop (34), juce::Justification::centred);
            g.setFont (juce::Font (juce::FontOptions (44.f, juce::Font::bold)));
            g.setColour (col::melody);
            g.drawText (juce::String ((int) std::lround (overlayScore)), c.removeFromTop (60), juce::Justification::centred);
            g.setColour (col::text);
            g.setFont (juce::Font (juce::FontOptions (18.f, juce::Font::bold)));
            g.drawText (juce::String (ScoreKeeper::rankName (overlayScore)) + "   " + stars (ScoreKeeper::stars (overlayScore)), c, juce::Justification::centred);
        }
    }

    static juce::String stars (int n)
    {
        juce::String s;
        for (int i = 0; i < 5; ++i) s += i < n ? juce::String (juce::CharPointer_UTF8 ("\xe2\x98\x85")) : juce::String (juce::CharPointer_UTF8 ("\xe2\x98\x86"));
        return s;
    }

    void paintScore (juce::Graphics& g)
    {
        auto r = scoreBox.reduced (16, 34);
        const double sc = sk.score();
        g.setColour (col::melody);
        g.setFont (juce::Font (juce::FontOptions (54.f, juce::Font::bold)));
        g.drawText (juce::String ((int) std::lround (sc)), r.removeFromTop (62), juce::Justification::centred);
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (12.f)));
        g.drawText ("out of 10000", r.removeFromTop (16), juce::Justification::centred);
        r.removeFromTop (6);
        g.setColour (juce::Colour (0xffffd24a));
        g.setFont (juce::Font (juce::FontOptions (26.f)));
        g.drawText (stars (ScoreKeeper::stars (sc)), r.removeFromTop (34), juce::Justification::centred);
        g.setColour (col::text);
        g.setFont (juce::Font (juce::FontOptions (17.f, juce::Font::bold)));
        g.drawText (ScoreKeeper::rankName (sc), r.removeFromTop (26), juce::Justification::centred);
        r.removeFromTop (14);

        auto line = [&] (const juce::String& k, const juce::String& v)
        {
            auto row = r.removeFromTop (22);
            g.setColour (col::dim); g.setFont (juce::Font (juce::FontOptions (12.5f)));
            g.drawText (k, row, juce::Justification::centredLeft);
            g.setColour (col::text); g.drawText (v, row, juce::Justification::centredRight);
        };
        const bool voiced = juce::Time::getMillisecondCounterHiRes() - lastVoicedMs < 200.0 && proc.singRunning.load();
        const int idx = track ? track->noteAt (dispBeat) : -1;
        line ("Target note", idx >= 0 ? midiName (track->notes[(size_t) idx].pitch) : juce::String ("--"));
        line ("You are singing", voiced ? midiName (sungMidi) : juce::String ("--"));
        line ("Last pass", sk.passCount() > 0 ? juce::String ((int) std::lround (sk.lastScore())) : juce::String ("--"));
        line ("Best pass", best > 0 ? juce::String ((int) std::lround (best)) : juce::String ("--"));
        line ("Passes", juce::String (sk.passCount()));

        r.removeFromTop (12);
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawFittedText ("Assist gently pulls your voice toward the current note (adds pitch-correction latency). "
                          "Put the plug-in on a mixer track that carries your microphone.", r, juce::Justification::topLeft, 6);
    }

    AIOProcessor& proc;
    Toggle on;
    Combo source, level;
    Toggle octave, guide, assist;
    Knob guideLvl;
    juce::TextButton playBtn, loadBtn;
    juce::Rectangle<int> strip, scoreBox, lane, infoRect;

    std::shared_ptr<const SingTrack> track;
    ScoreKeeper sk;
    SingConfig cfg;
    std::vector<int> rating;
    struct TrailPt { double beat; float midi; };
    std::deque<TrailPt> trail;
    std::vector<Pop> pops;
    double dispBeat = 0.0, lastSeenBeat = 0.0, lastSeenMs = 0.0, lastVoicedMs = -1.0e9, lastTarget = -1.0;
    double sungMidi = 0.0, sungFolded = 0.0;
    double overlayUntil = 0.0, overlayScore = 0.0, best = 0.0;
    juce::String message;
    std::unique_ptr<juce::FileChooser> chooser;
};

#if ! JucePlugin_IsSynth
#include "VocalUI.h"

// =================================================================================================
// VOCAL EDIT (Melodyne style note editor)
// =================================================================================================
class DragVocalButton : public juce::TextButton
{
public:
    explicit DragVocalButton (AIOProcessor& p) : proc (p) { setButtonText ("Drag audio to FL"); }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! started && e.getDistanceFromDragStart() > 6)
        {
            started = true;
            const auto file = proc.getVocal().dragFile();
            if (file.existsAsFile())
                if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
                    c->performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this, nullptr);
        }
        else if (! started)
            juce::TextButton::mouseDrag (e);
    }
    void mouseUp (const juce::MouseEvent& e) override { started = false; juce::TextButton::mouseUp (e); }
private:
    AIOProcessor& proc;
    bool started = false;
};

class AIOEditor::VocalPanel : public AIOEditor::PanelBase, private juce::Timer
{
public:
    explicit VocalPanel (AIOProcessor& p)
        : proc (p), roll (p), dragBtn (p),
          loopT (p.apvts, "vedLoop", "LOOP", col::bass),
          followT (p.apvts, "vedFollow", "PLAY WITH PROJECT", col::bass),
          origT (p.apvts, "vedOrig", "ORIGINAL", col::bass),
          linkT (p.apvts, "tuneLink", "follow Compose key", col::bass),
          key (p.apvts, "tuneKey", "Key"), scale (p.apvts, "tuneScale", "Scale"),
          grid (p.apvts, "vedGrid", "Timing grid"), snap (p.apvts, "vedSnap", "Drag snaps to"),
          kCorrect (p.apvts, "vedCorrect", "Pitch correct", 0, col::bass),
          kFlatten (p.apvts, "vedFlatten", "Flatten drift", 0, col::bass),
          kVibrato (p.apvts, "vedVibrato", "Vibrato", 0, col::bass),
          kGlide (p.apvts, "vedGlide", "Glide", 0, col::bass),
          kTiming (p.apvts, "vedTiming", "Timing", 0, col::bass),
          kLevel (p.apvts, "vedLevel", "Even out level", 0, col::bass),
          kAnchor (p.apvts, "vedAnchor", "Clip starts at beat", 2, col::bass)
    {
        loadBtn.setButtonText ("LOAD...");
        recBtn.setButtonText ("RECORD");
        recNowBtn.setButtonText ("REC NOW");
        stopRecBtn.setButtonText ("STOP REC");
        playBtn.setButtonText ("PLAY");
        exportBtn.setButtonText ("EXPORT WAV");
        autoBtn.setButtonText ("AUTO");
        recBtn.setTooltip ("Arms the recorder: recording starts when your DAW starts playing and the clip is placed at that beat.");
        recNowBtn.setTooltip ("Starts recording from the plug-in input right now.");
        autoBtn.setColour (juce::TextButton::buttonColourId, col::bass.darker (0.25f));
        autoBtn.setColour (juce::TextButton::textColourOffId, col::bg);
        playBtn.setColour (juce::TextButton::buttonColourId, col::accent.darker (0.4f));

        for (auto* b : std::initializer_list<juce::Button*> { &snapScale, &snapSemi, &flatBtn, &noVibBtn, &splitBtn, &joinBtn, &muteBtn, &resetBtn, &undoBtn, &redoBtn, &redetBtn })
            addAndMakeVisible (b);
        snapScale.setButtonText ("Snap to scale");  snapSemi.setButtonText ("Snap semitone");
        flatBtn.setButtonText ("Flatten drift");    noVibBtn.setButtonText ("Remove vibrato");
        splitBtn.setButtonText ("Split at playhead"); joinBtn.setButtonText ("Join with next");
        muteBtn.setButtonText ("Mute");  resetBtn.setButtonText ("Reset");
        undoBtn.setButtonText ("Undo");  redoBtn.setButtonText ("Redo");
        redetBtn.setButtonText ("Detect again");

        for (auto* c : std::initializer_list<juce::Component*> { &roll, &dragBtn, &loadBtn, &recBtn, &recNowBtn, &stopRecBtn, &playBtn, &exportBtn, &autoBtn,
                                                                  &loopT, &followT, &origT, &linkT, &key, &scale, &grid, &snap,
                                                                  &kCorrect, &kFlatten, &kVibrato, &kGlide, &kTiming, &kLevel, &kAnchor })
            addAndMakeVisible (c);

        auto setup = [this] (Insp& s, const juce::String& name, double lo, double hi, double step, const juce::String& suffix)
        {
            s.label.setText (name, juce::dontSendNotification);
            s.label.setFont (juce::Font (juce::FontOptions (11.5f)));
            s.label.setColour (juce::Label::textColourId, col::dim);
            s.slider.setSliderStyle (juce::Slider::LinearHorizontal);
            s.slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 18);
            s.slider.setRange (lo, hi, step);
            s.slider.setTextValueSuffix (suffix);
            s.slider.setColour (juce::Slider::trackColourId, col::bass);
            s.slider.setColour (juce::Slider::textBoxTextColourId, col::text);
            s.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            s.slider.onDragStart = [this] { sliderDragging = true; roll.beginEdit(); autoLive = false; };
            s.slider.onDragEnd = [this] { sliderDragging = false; };
            addAndMakeVisible (s.label); addAndMakeVisible (s.slider);
        };
        setup (iPitch, "Fine pitch", -50, 50, 1, " c");
        setup (iDrift, "Pitch drift", 0, 100, 1, " %");
        setup (iVib, "Vibrato depth", 0, 200, 1, " %");
        setup (iForm, "Formant", -6, 6, 0.1, " st");
        setup (iGain, "Note volume", -24, 12, 0.5, " dB");
        iPitch.slider.onValueChange = [this] { inspectorEdit (0); };
        iDrift.slider.onValueChange = [this] { inspectorEdit (1); };
        iVib.slider.onValueChange   = [this] { inspectorEdit (2); };
        iForm.slider.onValueChange  = [this] { inspectorEdit (3); };
        iGain.slider.onValueChange  = [this] { inspectorEdit (4); };

        roll.onSelectionChanged = [this] { refreshInspector(); };
        roll.onEdited = [this] { autoLive = false; if (! sliderDragging) refreshInspector(); };

        loadBtn.onClick = [this] { chooseFile(); };
        recBtn.onClick = [this] { proc.getVocal().startRecording (true); };
        recNowBtn.onClick = [this] { proc.getVocal().startRecording (false); };
        stopRecBtn.onClick = [this] { proc.getVocal().stopRecording(); };
        playBtn.onClick = [this]
        {
            auto& v = proc.getVocal();
            if (! v.hasClip()) return;
            if (v.isPlaying()) v.play (false);
            else { if (v.playheadSeconds() >= v.renderedSeconds() - 0.02) v.seekSeconds (0.0); v.play (true); }
        };
        exportBtn.onClick = [this] { chooseExport(); };
        autoBtn.onClick = [this] { runAuto (true); };

        snapScale.onClick = [this] { roll.snapSelected (2); };
        snapSemi.onClick  = [this] { roll.snapSelected (1); };
        flatBtn.onClick   = [this] { roll.editSelected ([] (aio::VocalNote& n) { n.drift = 0.f; }); };
        noVibBtn.onClick  = [this] { roll.editSelected ([] (aio::VocalNote& n) { n.vib = 0.f; }); };
        splitBtn.onClick  = [this] { roll.splitAtPlayhead(); };
        joinBtn.onClick   = [this] { roll.joinSelected(); };
        muteBtn.onClick   = [this] { roll.toggleMute(); };
        resetBtn.onClick  = [this] { roll.resetSelected(); };
        undoBtn.onClick   = [this] { roll.undo(); };
        redoBtn.onClick   = [this] { roll.redo(); };
        redetBtn.onClick  = [this]
        {
            if (! proc.getVocal().hasClip()) return;
            proc.getVocal().doc().checkpoint();      // keeps the old notes reachable with Undo
            proc.getVocal().redetect();
            autoLive = false;
        };

        // the auto settings stay live after AUTO has been pressed: moving a knob re-applies them
        for (auto* k : { &kCorrect, &kFlatten, &kVibrato, &kGlide, &kTiming, &kLevel })
            k->slider.onValueChange = [this] { markAutoDirty(); };
        grid.box.onChange = [this] { markAutoDirty(); };
        key.box.onChange = [this] { markAutoDirty(); };
        scale.box.onChange = [this] { markAutoDirty(); };
        linkT.onClick = [this] { markAutoDirty(); };

        refreshInspector();
        startTimerHz (30);
    }
    ~VocalPanel() override { stopTimer(); }

    VocalRoll& getRoll() { return roll; }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, autoRect, "Auto edit", col::bass);
        drawSection (g, inspRect, selText, col::bass);

        auto& v = proc.getVocal();
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (12.f)));
        g.drawFittedText (statusText, statusRect, juce::Justification::centredLeft, 2);

        g.setFont (juce::Font (juce::FontOptions (11.f)));
        g.drawFittedText (roll.selectedCount() > 0 ? "AUTO changes only the selected notes" : "AUTO changes every note", autoNoteRect, juce::Justification::centred, 2);
        juce::ignoreUnused (v);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        auto bar = b.removeFromTop (32);
        auto put = [&bar] (juce::Component& c, int w) { c.setBounds (bar.removeFromLeft (w).withTrimmedRight (6)); };
        put (loadBtn, 84); put (recBtn, 84); put (recNowBtn, 84); put (stopRecBtn, 84);
        bar.removeFromLeft (10);
        put (playBtn, 70); put (loopT, 66); put (followT, 166); put (origT, 100);
        bar.removeFromLeft (4);
        put (exportBtn, 104); put (dragBtn, 124);
        statusRect = bar.withTrimmedLeft (6);

        b.removeFromTop (6);
        autoRect = b.removeFromTop (136);
        b.removeFromTop (6);
        inspRect = b.removeFromBottom (108);
        b.removeFromBottom (6);
        roll.setBounds (b);

        // auto section
        auto a = autoRect.reduced (14, 32);
        auto left = a.removeFromLeft (128);
        autoBtn.setBounds (left.removeFromTop (46));
        autoNoteRect = left.withTrimmedTop (2);
        a.removeFromLeft (10);
        auto kc = a.removeFromLeft (150);
        key.setBounds (kc.removeFromTop (28)); kc.removeFromTop (2);
        scale.setBounds (kc.removeFromTop (28)); kc.removeFromTop (2);
        linkT.setBounds (kc.removeFromTop (22));
        a.removeFromLeft (8);
        auto rightCol = a.removeFromRight (150);
        grid.setBounds (rightCol.removeFromTop (28)); rightCol.removeFromTop (2);
        snap.setBounds (rightCol.removeFromTop (28));
        a.removeFromRight (6);
        const int kw = a.getWidth() / 7;
        for (auto* k : { &kCorrect, &kFlatten, &kVibrato, &kGlide, &kTiming, &kLevel, &kAnchor })
            k->setBounds (a.removeFromLeft (kw));

        // inspector
        auto r = inspRect.reduced (14, 30);
        auto btns = r.removeFromRight (500);
        r.removeFromRight (14);
        const int colW = r.getWidth() / 2;
        Insp* order[5] = { &iPitch, &iDrift, &iVib, &iForm, &iGain };
        for (int i = 0; i < 5; ++i)
        {
            auto cell = juce::Rectangle<int> (r.getX() + (i / 3) * colW, r.getY() + (i % 3) * 24, colW - 14, 24);
            order[i]->label.setBounds (cell.removeFromLeft (96));
            order[i]->slider.setBounds (cell);
        }
        juce::Button* bb[11] = { &snapScale, &snapSemi, &flatBtn, &noVibBtn, &muteBtn, &splitBtn, &joinBtn, &resetBtn, &undoBtn, &redoBtn, &redetBtn };
        const int bw = btns.getWidth() / 4;
        for (int i = 0; i < 11; ++i)
            bb[i]->setBounds (btns.getX() + (i % 4) * bw, btns.getY() + (i / 4) * 24, bw - 6, 22);
    }

    // ---- driven by tests too
    void runAuto (bool fresh)
    {
        auto& v = proc.getVocal();
        if (! v.hasClip()) return;
        const bool hasSel = roll.selectedCount() > 0;
        if (hasSel) liveScope = roll.selection(); else liveScope.clear();
        if (! fresh && ! v.doc().undoStack.empty())
        {                                           // live re-apply: replace the previous auto step instead of stacking another one
            v.doc().notes = v.doc().undoStack.back();
            v.doc().undoStack.pop_back();
        }
        proc.autoEditVocal (hasSel ? &liveScope : nullptr);
        autoLive = true;
        roll.repaint();
    }
    bool isAutoLive() const { return autoLive; }
    void inspectorEdit (int which)
    {
        if (refreshing || roll.selectedCount() == 0) return;
        if (! sliderDragging) roll.beginEdit();
        autoLive = false;
        auto& notes = proc.getVocal().doc().notes;
        const int first = roll.firstSelected();
        const double curCents = (notes[(size_t) first].target - std::round (notes[(size_t) first].target)) * 100.0;
        const float v = (float) (which == 0 ? iPitch.slider.getValue() : which == 1 ? iDrift.slider.getValue() : which == 2 ? iVib.slider.getValue()
                              : which == 3 ? iForm.slider.getValue() : iGain.slider.getValue());
        const float delta = (float) ((v - curCents) * 0.01);
        roll.applyToSelected ([which, v, delta] (aio::VocalNote& n)
        {
            switch (which)
            {
                case 0: n.target += delta; break;
                case 1: n.drift = v * 0.01f; break;
                case 2: n.vib = v * 0.01f; break;
                case 3: n.formant = v; break;
                default: n.gainDb = v; break;
            }
        });
        roll.commit();
    }

private:
    struct Insp { juce::Label label; juce::Slider slider; };

    void markAutoDirty() { if (autoLive) autoDirtyAt = juce::Time::getMillisecondCounter(); }

    void refreshInspector()
    {
        auto& v = proc.getVocal();
        const int n = roll.selectedCount();
        const int first = roll.firstSelected();
        refreshing = true;
        const bool on = n > 0 && first >= 0 && (size_t) first < v.doc().notes.size();
        for (auto* s : { &iPitch, &iDrift, &iVib, &iForm, &iGain }) s->slider.setEnabled (on);
        for (auto* b : std::initializer_list<juce::Button*> { &snapScale, &snapSemi, &flatBtn, &noVibBtn, &muteBtn, &splitBtn, &joinBtn, &resetBtn })
            b->setEnabled (b == &splitBtn ? v.hasClip() : on);
        if (on)
        {
            const auto& nt = v.doc().notes[(size_t) first];
            iPitch.slider.setValue ((nt.target - std::round (nt.target)) * 100.0, juce::dontSendNotification);
            iDrift.slider.setValue (nt.drift * 100.0, juce::dontSendNotification);
            iVib.slider.setValue (nt.vib * 100.0, juce::dontSendNotification);
            iForm.slider.setValue (nt.formant, juce::dontSendNotification);
            iGain.slider.setValue (nt.gainDb, juce::dontSendNotification);
            selText = n == 1 ? "Selected note:  " + aio::ui::noteLabel (nt.target, true) + "   (" + juce::String (nt.newLen(), 2) + " s)"
                             : juce::String (n) + " notes selected  (sliders change all of them)";
        }
        else selText = v.hasClip() ? "Select a note to fine tune it  (click, or drag a box around several)" : "Notes";
        refreshing = false;
        undoBtn.setEnabled (! v.doc().undoStack.empty());
        redoBtn.setEnabled (! v.doc().redoStack.empty());
        repaint (inspRect);
    }

    void timerCallback() override
    {
        auto& v = proc.getVocal();
        if (v.poll())
        {
            if (v.takeNewClip()) { roll.clipChanged(); autoLive = false; }
            roll.repaint();
            refreshInspector();
        }
        double beat = 0.0;
        if (v.takeAnchor (beat)) proc.setParam ("vedAnchor", (float) beat);

        if (autoLive && autoDirtyAt != 0 && juce::Time::getMillisecondCounter() - autoDirtyAt > 150)
        {
            autoDirtyAt = 0;
            runAuto (false);
        }

        const int rm = v.recMode();
        juce::String s;
        if (rm == aio::VocalSession::RecArmed) s = "Armed: press play in your DAW to start recording";
        else if (rm == aio::VocalSession::RecRecording) s = "Recording...  " + juce::String (v.recSeconds(), 1) + " s  (press STOP REC or stop the DAW)";
        else if (v.busy()) s = v.status();
        else if (! exportNote.isEmpty()) s = exportNote;
        else if (v.hasClip()) s = v.status().isNotEmpty() ? v.status() : juce::String ("Clip: ") + juce::String (v.clipSeconds(), 1) + " s,  " + juce::String ((int) v.doc().notes.size()) + " notes";
        else s = "Load a vocal or record one";
        if (s != statusText) { statusText = s; repaint (statusRect); }

        playBtn.setButtonText (v.isPlaying() ? "STOP" : "PLAY");
        const bool idle = rm == aio::VocalSession::RecIdle;
        recBtn.setEnabled (idle); recNowBtn.setEnabled (idle); stopRecBtn.setEnabled (! idle);
        loadBtn.setEnabled (idle);
        exportBtn.setEnabled (v.hasClip());
        dragBtn.setEnabled (v.hasClip());
        autoBtn.setEnabled (v.hasClip() && ! v.busy());
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a vocal to edit", juce::File(), "*.wav;*.mp3;*.flac;*.aif;*.aiff;*.ogg");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile()) { exportNote.clear(); proc.getVocal().play (false); proc.getVocal().loadFile (f); }
        });
    }

    void chooseExport()
    {
        if (! proc.getVocal().hasClip()) return;
        chooser = std::make_unique<juce::FileChooser> ("Export the edited vocal", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Vocal edit.wav"), "*.wav");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (! f.hasFileExtension ("wav")) f = f.withFileExtension ("wav");
            const auto r = proc.getVocal().exportWav (f);
            exportNote = r.wasOk() ? "Saved " + f.getFileName() : "Could not save: " + r.getErrorMessage();
        });
    }

    AIOProcessor& proc;
    aio::ui::VocalRoll roll;
    DragVocalButton dragBtn;
    juce::TextButton loadBtn, recBtn, recNowBtn, stopRecBtn, playBtn, exportBtn, autoBtn;
    juce::TextButton snapScale, snapSemi, flatBtn, noVibBtn, splitBtn, joinBtn, muteBtn, resetBtn, undoBtn, redoBtn, redetBtn;
    Toggle loopT, followT, origT, linkT;
    Combo key, scale, grid, snap;
    Knob kCorrect, kFlatten, kVibrato, kGlide, kTiming, kLevel, kAnchor;
    Insp iPitch, iDrift, iVib, iForm, iGain;
    juce::Rectangle<int> autoRect, inspRect, statusRect, autoNoteRect;
    juce::String statusText, selText { "Notes" }, exportNote;
    std::vector<char> liveScope;
    bool autoLive = false, sliderDragging = false, refreshing = false;
    juce::uint32 autoDirtyAt = 0;
    std::unique_ptr<juce::FileChooser> chooser;
};
#endif
