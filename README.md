# AIO Studio  (VST3 for FL Studio)

One plugin, eight tabs (the Instrument build keeps the first five):

| Tab | What it does |
|---|---|
| **TUNE** | Real-time pitch correction (Auto-Tune style): key + scale (12 scales, chromatic, or draw your own on the 12-key display), retune speed 0-400 ms (0 = hard "robot" tuning), amount, fine tune, voice range, gate. Live detected-note / target-note monitor. |
| **EQ** | 6-band parametric EQ (bell, low/high shelf, high/low pass, notch) with draggable nodes and a live spectrum analyser behind the curve. Mouse-wheel over a node changes Q, double-click toggles a band. |
| **DYNAMICS + SAT** | Stereo-linked compressor (threshold, ratio, attack, release, soft knee, makeup, parallel mix, GR meter) and a 2x-oversampled saturator (Tape, Tube, Soft clip, Hard clip, Wavefolder) with drive, tone, mix and output. |
| **SPACE** | Chorus, tempo-synced ping-pong delay, reverb (pre-delay, damping, width, low/high cut, 5 presets), stereo width. |
| **COMPOSE** | Chord + melody + bass generator. 14 progressions plus two random "composers", 12 scales/modes, 7 chord types, 3 voicings with smooth voice-leading, 10 chord rhythms (sustain, tresillo, arps, strum...), melody styles, density, range, rests, swing, humanise. Built-in preview synth, piano-roll view, **drag the MIDI straight into FL Studio**. |

| **MASTER** | **Auto Master.** Load a WAV / MP3 / FLAC / AIFF / OGG, press *AUTO MASTER*. It measures loudness (ITU-R BS.1770 LUFS, true peak), the 1/3-octave spectrum and the stereo image, fits a mild corrective EQ toward a target curve (Balanced / Warm / Bright / Punchy / Loud), sets compressor "glue", a touch of tape saturation and mono-bass, then solves the output gain so the result lands on your loudness target under the limiter ceiling. Waveform with seek, **A/B (original vs mastered, loudness-matched)**, tonal-balance view (your mix vs target, EQ correction, expected result), before/after numbers, **Export WAV (16 / 24 / 32-bit)**. The same settings are written into the plug-in's own EQ / Dynamics / Sat / Limiter, so the plug-in on your FL **Master** keeps mastering whatever you play. *Learn from playback* does the same analysis on the live signal; *Follow loudness* gently rides the output gain toward the target while you play. |
| **VOICE** | **Auto Voice.** For vocals, instruments and podcasts (Vocal / Speech / Instrument). Listens and adapts: a gain rider levels phrases that are too quiet or too loud toward a target, a split-band de-esser tames harsh S sounds relative to the voice level, an 8-band adaptive tone EQ nudges the long-term spectrum toward a healthy shape (small, slow moves), a mode-tuned compressor evens out the rest, and a gap expander lowers the room between phrases. Zero latency. Live meters show exactly what it is doing. |
| **SING** | **Sing game** (think the old karaoke-style pitch games). The melody scrolls toward a now-line; sing or hum into the plug-in's input and a pink bar shows your pitch live. Notes light up where you hit them, each note is rated *Perfect / Great / Good / miss*, and you get a 0-10000 score, stars and a rank per pass. Melody = the Compose melody or any imported MIDI file. Difficulty (pitch tolerance), any-octave scoring, a guide tone, and an optional **Assist** that gently pulls your voice onto the current note. |

Always-on: input/output gain + meters, output limiter, 8 factory presets (host preset menu).

Signal chain: `Auto Voice -> Tuner -> (Compose synth / Sing guide tone) -> EQ -> Compressor -> Saturator -> Chorus -> Delay -> Reverb -> Width + mono-bass -> Output -> (Follow loudness) -> Look-ahead limiter`

## Two builds

* **AIO Studio** (effect) - put it on a mixer insert (vocal, bus, master). Tuner + FX process the incoming audio. The Compose engine plays through the same chain whenever the FL transport runs (switch *COMPOSE ON*) or when you press *Play preview*.
* **AIO Studio Instrument** (generator) - same engine, but it is a generator, so it can **output MIDI**. In FL Studio set the plugin's *MIDI output port* and send the chords / melody / bass (MIDI channels 1 / 2 / 3) to any other synth, while hearing the built-in preview synth if you want.

## Building

Requirements: CMake 3.22+, a C++17 compiler (Visual Studio 2022 on Windows, Xcode on macOS). JUCE 8 is downloaded automatically.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target AIOStudio_VST3 AIOStudioInstrument_VST3
```

Add `-DAIO_COPY_AFTER_BUILD=ON` to copy the plugins into your system VST3 folder.

No Visual Studio on your PC? Push this folder to a GitHub repository: the included workflow (`.github/workflows/build.yml`) builds Windows + macOS VST3 bundles (and runs the tests) on GitHub's servers and attaches the plugins to the run as downloadable artifacts (Actions tab -> latest run -> Artifacts). JUCE 8 itself does not support MinGW cross-compiling from Linux, so MSVC (Windows) is the supported route.

### Installing in FL Studio (Windows)

1. Copy the `AIO Studio.vst3` and `AIO Studio Instrument.vst3` folders to `C:\Program Files\Common Files\VST3\`.
2. FL Studio -> *Options -> Manage plugins* -> *Find installed plugins* (Verify plugins).
3. Add via the mixer insert slot (AIO Studio) or the channel rack *+* (AIO Studio Instrument).

## Tests

```
cmake -B build -DAIO_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target AIOTests
./build/AIOTests_artefacts/Release/AIOTests
```

126 checks. DSP: the BS.1770 K-weighting filters against the published coefficients, loudness of reference sines at three sample rates, gating, true-peak detection of an inter-sample peak, the limiter never exceeding its ceiling, Auto Voice leveling / noise handling / de-essing / tone correction, the full Auto Master on synthetic mixes with a known tonal defect (loudness lands within 0.5 LU of target, true peak stays under the ceiling, tonal error drops), a WAV-in / WAV-out session round trip, Sing scoring rules and MIDI import. Plus the original: chord naming, melody-stays-in-scale / chord-tone-on-strong-beat, determinism, EQ / compressor accuracy, delay timing, reverb, saturator safety, pitch-correction accuracy (a note 40 cents sharp lands within a fraction of a cent of the target; vibrato flattening; scale snapping), the whole processor with odd block sizes, MIDI export round-trip, state save/load and presets.

## Notes and limits

* **Auto Master is a mastering *assistant*, not a replacement for a mastering engineer.** It makes measured, conservative moves (the EQ moves are capped at about 5 dB) and hits a loudness target; it can't fix a bad mix, and it can't hear taste. Use the A/B button. The limiter works on sample peaks with a half-sample estimate: the true peak can land up to ~0.3 dB above the ceiling, which is why the default ceiling is -1.0 dB.
* **Auto Voice** levels, de-esses and EQs by listening; its gap expander is *not* a spectral noise remover. Very noisy recordings need a proper denoiser first.
* **Sing** is a live pitch game with optional assist. It is **not** Melodyne-style editing of a recorded take (you can't drag notes of a finished recording). Pitch detection is monophonic.
* MP3 reading uses JUCE's built-in decoder (`JUCE_USE_MP3AUDIOFORMAT=1`); MP3 export is not offered (export is WAV).
* The limiter adds 2 ms of look-ahead, the saturator's oversampler ~1 ms, and the tuner (when on) ~32 ms; the active total is reported to the host.
* While a loaded file is auditioning, the plug-in output is that file (your live input and the Compose/Sing engines are paused).

## Notes and limits (v1)

* The tuner is **monophonic** (one voice / lead line at a time) and adds ~32 ms of reported latency while it is switched on. It is not Melodyne-style polyphonic note editing; a note-by-note editor is the natural next step.
* Compose assumes 4/4.
* Fixed effect order. The Compose engine's MIDI-out only exists in the Instrument build (an effect plugin cannot send MIDI in FL Studio).
* JUCE is dual licensed (AGPLv3 / commercial). Fine for personal use; if you plan to sell the plugin you need a JUCE licence.
