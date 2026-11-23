// Included once from PluginEditor.cpp, after LayersUI.inc: the PODCAST READY panel (effect build only).

#if ! JucePlugin_IsSynth
#include "../podcast/MediaDecode.h"
namespace { const juce::Colour podCol { 0xff5ec8ff }; }      // sky blue

class AIOEditor::PodcastPanel : public AIOEditor::PanelBase, public juce::FileDragAndDropTarget, private juce::Timer
{
public:
    explicit PodcastPanel (AIOProcessor& p)
        : proc (p),
          kDenoise (p.apvts, "pcDenoise", "Noise", -1, podCol), kLevel (p.apvts, "pcLevel", "Leveler", -1, podCol),
          kTone (p.apvts, "pcTone", "Tone", -1, podCol), kDeEss (p.apvts, "pcDeEss", "De-ess", -1, podCol),
          kComp (p.apvts, "pcComp", "Compress", -1, podCol), kTarget (p.apvts, "pcTarget", "Loudness", 1, podCol),
          kCeil (p.apvts, "pcCeil", "Ceiling", 1, podCol),
          kLowCut (p.apvts, "pcLowCut", "Low cut", 0, podCol), kEcho (p.apvts, "pcEcho", "Echo", -1, podCol),
          kWarmth (p.apvts, "pcWarmth", "Warmth", -1, podCol), kSpkBal (p.apvts, "pcSpkBal", "Balance", -1, podCol),
          tDeclip (p.apvts, "pcDeclip", "Repair clipping", podCol), tMono (p.apvts, "pcMono", "Mono output", podCol),
          tSpk (p.apvts, "pcSpkOn", "Per voice", podCol)
    {
        loadBtn.setButtonText ("Load audio or video (or drop a file here)");
        batchBtn.setButtonText ("Batch...");
        batchBtn.setTooltip ("Pick several recordings or video clips: each one is cleaned with the current settings and saved into a folder you choose.");
        voiceBox.addItem ("Voices: auto", 1); voiceBox.addItem ("1 voice", 2); voiceBox.addItem ("2 voices", 3); voiceBox.addItem ("3 voices", 4); voiceBox.addItem ("4 voices", 5);
        voiceBox.setSelectedId (1, juce::dontSendNotification);
        voiceBox.setTooltip ("How many people speak on this recording. Auto works it out; set it if it guesses wrong. Then click a coloured block under the waveform to move a stretch of speech to another voice.");
        voiceBox.onChange = [this] { proc.setParam ("pcSpkCount", (float) (voiceBox.getSelectedId() - 1)); };
        kLowCut.slider.setTooltip ("Cuts wind and handling rumble below this pitch (0 = off). Voices sit above about 80 Hz; 100-150 Hz is typical for wind.");
        kEcho.slider.setTooltip ("Reduces the room's echo / reverb tail between words (helps a speech picked up by a mic across the room).");
        kWarmth.slider.setTooltip ("A touch of soft saturation for body and smoothness. 0 = off.");
        kSpkBal.slider.setTooltip ("With two or more voices: how far each is brought to the same loudness.");
        tSpk.setTooltip ("Give each voice its own EQ and level, using the voices found in the recording.");
        runBtn.setButtonText ("MAKE PODCAST READY");
        exportBtn.setButtonText ("Export...");
        playBtn.setButtonText ("PLAY");
        abBtn.setButtonText ("HEAR: PODCAST READY");
        loopBtn.setButtonText ("LOOP");
        loopBtn.setToggleState (true, juce::dontSendNotification);
        loopBtn.setColour (juce::ToggleButton::tickColourId, podCol);
        liveBtn.setButtonText ("LIVE");
        liveBtn.setToggleState (true, juce::dontSendNotification);
        liveBtn.setColour (juce::ToggleButton::tickColourId, podCol);
        liveBtn.setTooltip ("Hear the knobs working while it plays (about half a second after you move one). Turn off to hear the finished render instead.");
        matchBtn.setButtonText ("MATCH VOLUME");
        matchBtn.setToggleState (true, juce::dontSendNotification);
        matchBtn.setColour (juce::ToggleButton::tickColourId, podCol);
        runBtn.setColour (juce::TextButton::buttonColourId, podCol.darker (0.25f));
        runBtn.setColour (juce::TextButton::textColourOffId, col::bg);
        fmt.addItem ("WAV 16-bit", 1); fmt.addItem ("WAV 24-bit", 2); fmt.addItem ("FLAC 24-bit", 3); fmt.addItem ("WAV 32-bit float", 4);
        fmt.setSelectedId (2, juce::dontSendNotification);
        for (int i = 0; i < AIOProcessor::podcastPresetCount(); ++i) preset.addItem (AIOProcessor::podcastPresetName (i), i + 1);
        preset.setTextWhenNothingSelected ("Starting point...");
        preset.onChange = [this] { if (preset.getSelectedId() > 0) proc.applyPodcastPreset (preset.getSelectedId() - 1); };

        hint.setText ("Load a recording (audio, or an MP4 / MOV video) and press PLAY: with LIVE on you hear the clean-up straight away and every knob changes the sound while it plays. One file with several people is fine: the coloured strip shows who is speaking, each voice gets its own EQ and level, and you can click a block to move it to another voice. Pick a Wedding starting point, then MAKE PODCAST READY (exact pass) and Export. Batch does many files at once.", juce::dontSendNotification);
        hint.setFont (juce::Font (juce::FontOptions (12.f)));
        hint.setColour (juce::Label::textColourId, col::dim);
        hint.setJustificationType (juce::Justification::topLeft);

        loadBtn.onClick = [this] { chooseFile(); };
        runBtn.onClick = [this] { proc.getPodcast().abOriginal = false; abBtn.setButtonText ("HEAR: PODCAST READY"); statusNote.clear(); proc.runPodcast(); };
        exportBtn.onClick = [this] { chooseExport(); };
        playBtn.onClick = [this]
        {
            auto& pc = proc.getPodcast();
            if (! pc.isPlaying()) proc.getMaster().setPlaying (false);        // one file at a time
            pc.setPlaying (! pc.isPlaying());
        };
        loopBtn.onClick = [this] { proc.getPodcast().loop = loopBtn.getToggleState(); };
        liveBtn.onClick = [this]
        {
            auto& pc = proc.getPodcast();
            pc.setLive (liveBtn.getToggleState());
            if (! liveBtn.getToggleState() && ! info.hasResult) statusNote = "LIVE is off: press MAKE PODCAST READY to render, then PLAY to hear the result.";
            else statusNote.clear();
        };
        matchBtn.onClick = [this] { proc.getPodcast().matchVolume = matchBtn.getToggleState(); };
        batchBtn.onClick = [this] { chooseBatch(); };
        abBtn.onClick = [this]
        {
            auto& pc = proc.getPodcast();
            const bool orig = ! pc.abOriginal.load();
            pc.abOriginal = orig;
            abBtn.setButtonText (orig ? "HEAR: ORIGINAL" : "HEAR: PODCAST READY");
        };

        for (auto* c : std::initializer_list<juce::Component*> { &kDenoise, &kLevel, &kTone, &kDeEss, &kComp, &kTarget, &kCeil, &kLowCut, &kEcho, &kWarmth, &kSpkBal, &tDeclip, &tMono, &tSpk,
                                                                  &batchBtn, &voiceBox, &loadBtn, &runBtn, &exportBtn, &fmt, &preset, &playBtn, &abBtn, &loopBtn, &liveBtn, &matchBtn, &hint })
            addAndMakeVisible (c);
        startTimerHz (20);
    }

    ~PodcastPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        drawSection (g, left, "Podcast Ready", podCol);
        drawSection (g, wave, "Your recording", podCol);
        drawSection (g, table, "Before / after", podCol);
        drawSection (g, notes, "What it did", podCol);

        auto& pc = proc.getPodcast();
        g.setFont (juce::Font (juce::FontOptions (12.f)));
        {
            auto r = fileInfoRect;
            g.setColour (col::text);
            g.drawFittedText (info.hasFile ? info.name + "   " + mmss (info.seconds) + "  " + juce::String ((int) info.sampleRate) + " Hz  " + (info.channels > 1 ? "stereo" : "mono")
                                           : juce::String ("No file loaded"), r.removeFromTop (18), juce::Justification::centredLeft, 1);
            g.setColour (col::dim);
            const auto m = statusNote.isNotEmpty() && ! pc.busy() ? statusNote : pc.message();
            g.drawFittedText (m, r, juce::Justification::topLeft, 3);
        }
        if (pc.busy())
        {
            auto pb = progressRect.toFloat();
            g.setColour (col::edge); g.fillRoundedRectangle (pb, 3.f);
            g.setColour (podCol); g.fillRoundedRectangle (pb.withWidth (pb.getWidth() * pc.progress()), 3.f);
        }
        paintWave (g);
        paintVoices (g);
        paintTable (g);
        paintNotes (g);
    }

    void resized() override
    {
        auto b = getLocalBounds();
        left = b.removeFromLeft (392);
        b.removeFromLeft (10);

        auto l = left.reduced (14, 34);
        loadBtn.setBounds (l.removeFromTop (30)); l.removeFromTop (6);
        fileInfoRect = l.removeFromTop (50);
        progressRect = l.removeFromTop (6); l.removeFromTop (8);
        preset.setBounds (l.removeFromTop (26)); l.removeFromTop (6);
        const int cw = l.getWidth() / 4;
        auto row = [&] (std::initializer_list<juce::Component*> ks)
        {
            auto r = l.removeFromTop (82);
            for (auto* k : ks) k->setBounds (r.removeFromLeft (cw));
        };
        row ({ &kDenoise, &kLevel, &kTone, &kDeEss });
        row ({ &kComp, &kTarget, &kCeil, &kLowCut });
        row ({ &kEcho, &kWarmth, &kSpkBal });
        {
            auto t = l.removeFromTop (30).reduced (0, 2);
            const int w3 = t.getWidth() / 3;
            tSpk.setBounds (t.removeFromLeft (w3));
            tDeclip.setBounds (t.removeFromLeft (w3));
            tMono.setBounds (t);
        }
        l.removeFromTop (8);
        runBtn.setBounds (l.removeFromTop (40)); l.removeFromTop (8);
        auto ex = l.removeFromTop (28);
        fmt.setBounds (ex.removeFromRight (120)); ex.removeFromRight (6);
        batchBtn.setBounds (ex.removeFromRight (ex.getWidth() / 2 - 3)); ex.removeFromRight (6);
        exportBtn.setBounds (ex);

        wave = b.removeFromTop (268);
        b.removeFromTop (10);
        auto w = wave.reduced (14, 30);
        auto tr = w.removeFromBottom (30);
        legendRect = w.removeFromBottom (16);
        tlRect = w.removeFromBottom (22);
        waveRect = w.withTrimmedBottom (6);
        voiceBox.setBounds (tr.removeFromRight (120)); tr.removeFromRight (6);
        playBtn.setBounds (tr.removeFromLeft (84)); tr.removeFromLeft (6);
        loopBtn.setBounds (tr.removeFromLeft (64)); tr.removeFromLeft (6);
        liveBtn.setBounds (tr.removeFromLeft (64)); tr.removeFromLeft (6);
        abBtn.setBounds (tr.removeFromLeft (190)); tr.removeFromLeft (6);
        matchBtn.setBounds (tr.removeFromLeft (130));

        table = b.removeFromLeft (b.getWidth() * 58 / 100);
        b.removeFromLeft (10);
        notes = b;
        hint.setBounds (notes.reduced (16, 30).removeFromBottom (110));
    }

    // drop a recording or a video clip onto the window to load it
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && ! proc.getPodcast().busy();
    }
    void filesDropped (const juce::StringArray& files, int, int) override
    {
        const juce::File f (files[0]);
        if (f.existsAsFile() && ! proc.getPodcast().busy()) loadDropped (f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (tlRect.expanded (0, 3).contains (e.getPosition()) && info.hasFile && info.voiceCount >= 2 && ! proc.getPodcast().busy())
        {
            proc.getPodcast().cycleSpeakerAt ((double) (e.x - tlRect.getX()) / (double) std::max (1, tlRect.getWidth()), e.mods.isRightButtonDown() || e.mods.isShiftDown() ? -1 : 1);
            info = proc.getPodcast().info();
            return;
        }
        seekFrom (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { seekFrom (e); }

private:
    void seekFrom (const juce::MouseEvent& e)
    {
        auto p = e.getPosition();
        if (! waveRect.expanded (4, 4).contains (p) || ! info.hasFile) return;
        proc.getPodcast().seek ((double) (p.x - waveRect.getX()) / (double) std::max (1, waveRect.getWidth()));
    }

    void loadDropped (const juce::File& f)
    {
        proc.getPodcast().setPlaying (false);
        statusNote.clear();
        proc.getPodcast().loadFile (f);
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose a recording or a video clip", juce::File(), media::chooserPattern());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f.existsAsFile())
            {
                proc.getPodcast().setPlaying (false);
                statusNote.clear();
                proc.getPodcast().loadFile (f);
            }
        });
    }

    void chooseBatch()
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose the recordings or video clips to clean (select several)", juce::File(), media::chooserPattern());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc)
        {
            const auto res = fc.getResults();
            if (res.isEmpty()) return;
            std::vector<juce::File> files;
            for (const auto& f : res) if (f.existsAsFile()) files.push_back (f);
            if (files.empty()) return;
            const auto def = files.front().getParentDirectory().getChildFile ("Podcast ready");
            chooser = std::make_unique<juce::FileChooser> ("Where should the finished files go? (pick or create a folder)", def, "*");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this, files] (const juce::FileChooser& fc2)
            {
                const auto dir = fc2.getResult();
                if (dir == juce::File()) return;
                const int f = fmt.getSelectedId();
                static const int depths[4] = { 16, 24, 24, 32 };
                proc.getPodcast().setPlaying (false);
                statusNote.clear();
                proc.getPodcast().runBatch (files, dir, proc.readPodcastOptions(), depths[juce::jlimit (0, 3, f - 1)], f == 3);
            });
        });
    }

    void chooseExport()
    {
        if (! info.hasResult) { statusNote = "Press MAKE PODCAST READY first, then export."; return; }
        const int f = fmt.getSelectedId();
        const juce::String ext = f == 3 ? "flac" : "wav";
        const auto base = juce::File (info.name).getFileNameWithoutExtension() + " - podcast ready." + ext;
        chooser = std::make_unique<juce::FileChooser> ("Export the podcast-ready file", juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (base), "*." + ext);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this, ext, f] (const juce::FileChooser& fc)
        {
            auto out = fc.getResult();
            if (out == juce::File()) return;
            if (! out.hasFileExtension (ext)) out = out.withFileExtension (ext);
            static const int depths[4] = { 16, 24, 24, 32 };
            proc.exportPodcast (out, depths[juce::jlimit (0, 3, f - 1)]);
            statusNote.clear();
        });
    }

    void timerCallback() override
    {
        auto& pc = proc.getPodcast();
        if ((tick++ % 3) == 0 || pc.state() != lastState) { info = pc.info(); lastState = pc.state(); }
        juce::String done;
        if (pc.takeExportDone (done)) statusNote = done;
        const bool busy = pc.busy();
        runBtn.setEnabled (info.hasFile && ! busy);
        exportBtn.setEnabled (info.hasResult && ! busy);
        playBtn.setEnabled (info.hasFile && ! busy);
        loadBtn.setEnabled (! busy);
        batchBtn.setEnabled (! busy);
        voiceBox.setEnabled (info.hasFile && ! busy);
        {
            const int want = (int) std::lround (proc.apvts.getRawParameterValue ("pcSpkCount")->load());
            if (voiceBox.getSelectedId() != want + 1) voiceBox.setSelectedId (want + 1, juce::dontSendNotification);
            if (info.hasFile && ! busy && want != info.voiceRequest) { pc.setSpeakerCount (want); info = pc.info(); }
        }
        const bool canHear = info.hasResult || pc.liveActive();
        abBtn.setEnabled (canHear);
        matchBtn.setEnabled (canHear);
        if (info.hasFile) pc.setLiveOptions (proc.readPodcastOptions());          // the knobs drive the live preview even before anything is playing
        if (liveBtn.getToggleState() != pc.live()) liveBtn.setToggleState (pc.live(), juce::dontSendNotification);
        playBtn.setButtonText (pc.isPlaying() ? "STOP" : "PLAY");
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
            g.drawText ("Load a recording to see it here. Click the waveform to jump around.", r.toNearestInt(), juce::Justification::centred);
            return;
        }
        const double pos = proc.getPodcast().position();
        auto half = [&] (const std::vector<float>& wf, juce::Rectangle<float> area, juce::Colour c, const juce::String& tag)
        {
            if (wf.size() < 4) return;
            const int cols = (int) wf.size() / 2;
            const float midY = area.getCentreY(), hh = area.getHeight() * 0.5f - 2.f;
            for (int x = 0; x < (int) area.getWidth(); ++x)
            {
                const int cI = juce::jlimit (0, cols - 1, (int) ((double) x / area.getWidth() * cols));
                const float mn = wf[(size_t) cI * 2], mx = wf[(size_t) cI * 2 + 1];
                const bool played = (double) x / area.getWidth() < pos;
                g.setColour (played ? c : c.withAlpha (0.45f));
                g.drawVerticalLine ((int) area.getX() + x, midY - std::min (1.f, mx) * hh, midY - std::max (-1.f, mn) * hh + 1.f);
            }
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText (tag, area.toNearestInt().reduced (6, 1), juce::Justification::topLeft);
        };
        auto top = r.withHeight (r.getHeight() * 0.5f), bot = r.withTrimmedTop (r.getHeight() * 0.5f);
        half (info.waveform, top, col::dim.brighter (0.2f), "ORIGINAL");
        if (info.hasResult) half (info.waveformAfter, bot, podCol, "PODCAST READY");
        else
        {
            g.setColour (col::dim);
            g.setFont (juce::Font (juce::FontOptions (12.f)));
            g.drawText ("The result appears here", bot.toNearestInt(), juce::Justification::centred);
        }
        g.setColour (col::edge); g.drawHorizontalLine ((int) top.getBottom(), r.getX(), r.getRight());
        const float px = r.getX() + (float) pos * r.getWidth();
        g.setColour (col::text);
        g.fillRect (px - 1.f, r.getY(), 2.f, r.getHeight());
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        g.drawText (mmss (pos * info.seconds) + " / " + mmss (info.seconds), r.toNearestInt().reduced (6, 2), juce::Justification::topRight);
    }

    static juce::Colour voiceColour (int v)
    {
        static const juce::Colour c[4] = { juce::Colour (0xff5ec8ff), juce::Colour (0xffffb454), juce::Colour (0xffe879f9), juce::Colour (0xff6ee7a0) };
        return c[juce::jlimit (0, 3, v)];
    }

    void paintVoices (juce::Graphics& g)
    {
        auto r = tlRect.toFloat();
        g.setColour (col::bg.brighter (0.03f)); g.fillRoundedRectangle (r, 4.f);
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        if (! info.hasFile) return;
        if (info.voiceCount < 2)
        {
            g.setColour (col::dim);
            g.drawText (info.voiceCount == 1 ? "One voice found: every part gets the same processing." : "No clear speech found to tell voices apart.",
                        tlRect.reduced (8, 0), juce::Justification::centredLeft);
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawText ("Several people on one mic? Set the number of voices at the right.", legendRect, juce::Justification::centredLeft);
            return;
        }
        for (const auto& s : info.segments)
        {
            const float x0 = r.getX() + (float) s.t0 * r.getWidth(), x1 = r.getX() + (float) s.t1 * r.getWidth();
            g.setColour (voiceColour (s.voice).withAlpha (0.85f));
            g.fillRect (x0, r.getY() + 3.f, std::max (1.f, x1 - x0), r.getHeight() - 6.f);
        }
        const float px = r.getX() + (float) proc.getPodcast().position() * r.getWidth();
        g.setColour (col::text); g.fillRect (px - 1.f, r.getY(), 2.f, r.getHeight());
        // legend
        g.setFont (juce::Font (juce::FontOptions (11.f)));
        int x = legendRect.getX();
        for (int v = 0; v < info.voiceCount && v < (int) info.voices.size(); ++v)
        {
            const auto& vo = info.voices[(size_t) v];
            g.setColour (voiceColour (v)); g.fillEllipse ((float) x, (float) legendRect.getCentreY() - 4.f, 8.f, 8.f);
            g.setColour (col::text);
            const juce::String t = "Voice " + juce::String (v + 1) + (vo.f0 > 0.f ? "  ~" + juce::String ((int) std::lround (vo.f0)) + " Hz" : juce::String()) + "  " + mmss ((double) vo.seconds);
            g.drawText (t, x + 12, legendRect.getY(), 150, legendRect.getHeight(), juce::Justification::centredLeft);
            x += 170;
        }
        g.setColour (col::dim);
        g.drawText ("click a block to change its voice", x, legendRect.getY(), std::max (0, legendRect.getRight() - x), legendRect.getHeight(), juce::Justification::centredRight);
    }

    void paintTable (juce::Graphics& g)
    {
        auto r = table.reduced (16, 34);
        g.setFont (juce::Font (juce::FontOptions (13.f)));
        const int c1 = r.getX() + 180, c2 = r.getX() + 286, rowH = 24;
        int y = r.getY();
        g.setColour (col::dim);
        g.drawText ("Before", c1, y, 100, rowH, juce::Justification::centredRight);
        g.drawText ("After", c2, y, 100, rowH, juce::Justification::centredRight);
        y += rowH;
        const bool has = info.hasFile && info.before.valid, aft = has && info.hasResult;
        const auto& B = info.before; const auto& A = info.report.after;
        auto row = [&] (const juce::String& name, const juce::String& a, const juce::String& b, bool good)
        {
            g.setColour (col::dim);  g.drawText (name, r.getX(), y, 180, rowH, juce::Justification::centredLeft);
            g.setColour (col::text); g.drawText (a, c1, y, 100, rowH, juce::Justification::centredRight);
            g.setColour (good ? podCol : col::text); g.drawText (b, c2, y, 100, rowH, juce::Justification::centredRight);
            y += rowH;
        };
        const double target = info.used.targetLufs;
        row ("Loudness (LUFS)", has ? dbText (B.lufs) : "--", aft ? dbText (A.lufs) : "--", aft && std::abs (A.lufs - target) < 0.7);
        row ("True peak (dBTP)", has ? dbText (B.truePeakDb) : "--", aft ? dbText (A.truePeakDb) : "--", aft && A.truePeakDb <= info.used.ceilingDb + 0.1);
        row ("Loud-to-quiet gap (dB)", has ? dbText (B.spreadDb) : "--", aft ? dbText (A.spreadDb) : "--", aft && A.spreadDb < B.spreadDb);
        row ("Voice vs background (dB)", has && B.noiseFloorDb > -150.0 ? dbText (B.snrDb, 0) : "--", aft && A.noiseFloorDb > -150.0 ? dbText (A.snrDb, 0) : "--", aft && A.snrDb > B.snrDb);
        row ("Clipped samples", has ? juce::String (B.clipped) : "--", aft ? juce::String (A.clipped) : "--", aft && A.clipped < B.clipped);
        y += 6;
        g.setColour (col::dim);
        g.setFont (juce::Font (juce::FontOptions (11.5f)));
        g.drawFittedText ("\"Loud-to-quiet gap\" compares the loudest and quietest spoken passages (pauses are ignored). Smaller means a steadier volume.",
                          juce::Rectangle<int> (r.getX(), y, r.getWidth(), 34), juce::Justification::topLeft, 2);
    }

    void paintNotes (juce::Graphics& g)
    {
        auto r = notes.reduced (16, 34);
        r.removeFromBottom (112);
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        {
            const auto bs = proc.getPodcast().batchStatus();
            if (bs.total > 0)
            {
                g.setColour (col::text);
                int y0 = r.getY();
                g.drawFittedText (bs.finished ? juce::String::formatted ("Batch finished: %d done, %d skipped.", bs.done, bs.failed)
                                              : juce::String::formatted ("Batch: %d of %d done...", bs.done + bs.failed, bs.total), r.getX(), y0, r.getWidth(), 18, juce::Justification::topLeft, 1);
                y0 += 22;
                g.setColour (col::dim);
                g.setFont (juce::Font (juce::FontOptions (11.5f)));
                const int first = std::max (0, bs.log.size() - 6);
                for (int i = first; i < bs.log.size(); ++i) { g.drawFittedText (bs.log[i], r.getX(), y0, r.getWidth(), 28, juce::Justification::topLeft, 2); y0 += 28; }
                return;
            }
        }
        if (! info.hasResult)
        {
            g.setColour (col::dim);
            g.drawFittedText ("Nothing processed yet.", r, juce::Justification::topLeft, 1);
            return;
        }
        const auto& rep = info.report;
        juce::StringArray lines;
        lines.add (rep.declipped > 0 ? "Rebuilt " + juce::String (rep.declipped) + " clipped samples." : (info.used.declip ? "No clipping found." : "Clipping repair was off."));
        lines.add (rep.denoiseEased ? "Noise reduction eased off: no clear pauses to learn the noise from."
                                    : (rep.denoised ? "Noise learned from your pauses and removed." : "Noise reduction was off."));
        lines.add (juce::String ("Volume ") + (rep.gainDb >= 0.0 ? "raised " : "lowered ") + juce::String (std::abs (rep.gainDb), 1) + " dB after levelling.");
        if (rep.limiterPeakGrDb < -0.1 || rep.limiterPeakGrDb > 0.1)
            lines.add ("Peak limiter worked up to " + juce::String (std::abs (rep.limiterPeakGrDb), 1) + " dB.");
        if (! rep.reachedTarget) lines.add ("Could not quite reach the target without breaking the peak ceiling.");
        if (info.used.mono) lines.add ("Exports as a one-channel (mono) file.");
        if (info.voiceCount >= 2 && info.used.speakers) lines.add (juce::String (info.voiceCount) + " voices: each got its own EQ and level.");
        if (info.resultStale) lines.add ("The voices were edited since this render: run it again to apply that.");
        g.setColour (col::text);
        int y = r.getY();
        for (auto& s : lines)
        {
            g.drawFittedText (s, r.getX(), y, r.getWidth(), 34, juce::Justification::topLeft, 2);
            y += 28 + (g.getCurrentFont().getHeight() * 1.2f > 14.f ? 4 : 0);
        }
    }

    AIOProcessor& proc;
    Knob kDenoise, kLevel, kTone, kDeEss, kComp, kTarget, kCeil, kLowCut, kEcho, kWarmth, kSpkBal;
    Toggle tDeclip, tMono, tSpk;
    juce::TextButton loadBtn, runBtn, exportBtn, playBtn, abBtn, batchBtn;
    juce::ComboBox voiceBox;
    juce::ToggleButton loopBtn, liveBtn, matchBtn;
    juce::ComboBox fmt, preset;
    juce::Label hint;
    juce::Rectangle<int> left, wave, table, notes, fileInfoRect, progressRect, waveRect, tlRect, legendRect;
    PodcastSession::Info info;
    PodcastSession::State lastState = PodcastSession::Idle;
    juce::String statusNote;
    int tick = 0;
    std::unique_ptr<juce::FileChooser> chooser;
public:
    PodcastSession::Info lastInfo() const { return info; }
};
#endif
