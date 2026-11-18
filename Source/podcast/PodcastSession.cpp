#include "PodcastSession.h"
#include <random>
#include <cmath>
#include <algorithm>

namespace aio
{
class PodcastSession::Job : public juce::Thread
{
public:
    explicit Job (std::function<void()> f) : juce::Thread ("Podcast job"), fn (std::move (f)) { startThread(); }
    ~Job() override { stopThread (120000); }
    void run() override { fn(); }
private:
    std::function<void()> fn;
};

PodcastSession::PodcastSession() { formats.registerBasicFormats(); }
PodcastSession::~PodcastSession() { cancelFlag.store (true); job.reset(); }

void PodcastSession::setMessage (const juce::String& m) { const juce::ScopedLock sl (lockInfo); msg = m; }
juce::String PodcastSession::message() const { const juce::ScopedLock sl (lockInfo); return msg; }
PodcastSession::Info PodcastSession::info() const { const juce::ScopedLock sl (lockInfo); return infoV; }

void PodcastSession::startJob (int initialState, std::function<void()> fn)
{
    cancelFlag.store (true);
    job.reset();
    cancelFlag.store (false);
    stateV.store (initialState);
    progressV.store (0.f);
    job = std::make_unique<Job> (std::move (fn));
}

void PodcastSession::cancel() { cancelFlag.store (true); }

bool PodcastSession::takeExportDone (juce::String& m)
{
    const juce::ScopedLock sl (lockInfo);
    if (! exportDone) return false;
    exportDone = false; m = exportMsg;
    return true;
}

void PodcastSession::makeWaveform (const juce::AudioBuffer<float>& b, std::vector<float>& out)
{
    constexpr int cols = 1200;
    const long long len = b.getNumSamples();
    out.assign ((size_t) cols * 2, 0.f);
    const float* wl = b.getReadPointer (0);
    const float* wr = b.getReadPointer (b.getNumChannels() > 1 ? 1 : 0);
    for (int c = 0; c < cols; ++c)
    {
        const long long a0 = len * c / cols, a1 = std::max (a0 + 1, len * (c + 1) / cols);
        float mn = 1.f, mx = -1.f;
        const long long step = std::max<long long> (1, (a1 - a0) / 64);
        for (long long i = a0; i < a1 && i < len; i += step) { const float v = 0.5f * (wl[i] + wr[i]); mn = std::min (mn, v); mx = std::max (mx, v); }
        out[(size_t) c * 2] = mn; out[(size_t) c * 2 + 1] = mx;
    }
}

// ------------------------------------------------------------------------------------------------ load
void PodcastSession::loadFile (const juce::File& file)
{
    startJob (Loading, [this, file]
    {
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); progressV.store (0.f); if (onWake) onWake(); };
        setMessage ("Reading " + file.getFileName() + "...");
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) { fail ("Couldn't read that file (unsupported or damaged)."); return; }
        const long long len = reader->lengthInSamples;
        const double sr = reader->sampleRate;
        const int nch = reader->numChannels >= 2 ? 2 : 1;
        if (len < 8192 || sr < 8000.0) { fail ("That file is too short or has an unusable sample rate."); return; }
        if (len * nch > kMaxSamples)
        {
            fail ("That file is too long for the in-plug-in processor (about 13 minutes of stereo or 26 of mono at 48 kHz). Split it and do it in parts.");
            return;
        }
        auto buf = std::make_shared<juce::AudioBuffer<float>> (nch, (int) len);
        constexpr int chunk = 1 << 16;
        juce::AudioBuffer<float> tmp (2, chunk);
        for (long long pos = 0; pos < len; pos += chunk)
        {
            if (cancelFlag.load()) { stateV.store (Idle); return; }
            const int n = (int) std::min<long long> (chunk, len - pos);
            tmp.clear();
            reader->read (&tmp, 0, n, pos, true, true);
            for (int c = 0; c < nch; ++c) buf->copyFrom (c, (int) pos, tmp, c, 0, n);
            progressV.store (0.6f * (float) pos / (float) len);
        }

        setMessage ("Measuring loudness, noise and clipping...");
        podcast::Stats st = podcast::measure (buf->getReadPointer (0), buf->getReadPointer (nch > 1 ? 1 : 0), (size_t) len, sr);
        if (cancelFlag.load()) { stateV.store (Idle); return; }
        if (! st.valid) { fail ("That file is silent - there is nothing to process."); return; }

        Info in;
        in.hasFile = true; in.name = file.getFileName(); in.sampleRate = sr; in.seconds = (double) len / sr; in.channels = nch; in.before = st;
        makeWaveform (*buf, in.waveform);
        {
            const juce::ScopedLock sl (lockInfo);
            original = buf; processed.reset(); infoV = in; exportDone = false;
        }
        playing.store (false); playPos.store (0);
        rebuildPlayback();
        setMessage (juce::String::formatted ("Loaded %s  -  %.1f LUFS, true peak %.1f dB", file.getFileName().toRawUTF8(), st.lufs, st.truePeakDb));
        progressV.store (1.f);
        stateV.store (Idle);
        if (onWake) onWake();
    });
}

// ------------------------------------------------------------------------------------------------ process
void PodcastSession::run (const podcast::Options& o)
{
    startJob (Processing, [this, o]
    {
        std::shared_ptr<juce::AudioBuffer<float>> src;
        Info in;
        { const juce::ScopedLock sl (lockInfo); src = original; in = infoV; }
        if (src == nullptr) { setMessage ("Load a file first."); stateV.store (Failed); if (onWake) onWake(); return; }
        setMessage ("Making it podcast ready: repairing, cleaning, levelling, setting loudness...");
        auto work = std::make_shared<juce::AudioBuffer<float>> (src->getNumChannels(), src->getNumSamples());
        for (int c = 0; c < work->getNumChannels(); ++c) work->copyFrom (c, 0, *src, c, 0, src->getNumSamples());
        podcast::Report rep;
        const bool ok = podcast::process (*work, in.sampleRate, o, rep, [this] (float x) { progressV.store (x); return ! cancelFlag.load(); });
        if (cancelFlag.load()) { stateV.store (Idle); return; }
        if (! ok) { setMessage ("Couldn't process that file (it may be silent)."); stateV.store (Failed); if (onWake) onWake(); return; }

        std::vector<float> wf;
        makeWaveform (*work, wf);
        {
            const juce::ScopedLock sl (lockInfo);
            processed = work;
            infoV.hasResult = true; infoV.report = rep; infoV.used = o; infoV.waveformAfter = wf;
        }
        double g = rep.after.lufs - in.before.lufs;                       // volume match for A/B: original raised/lowered to the same loudness
        g = std::min (g, -0.5 - in.before.truePeakDb);                    // ...but never into clipping
        matchGainLin.store ((float) std::pow (10.0, std::clamp (g, -40.0, 40.0) / 20.0));
        rebuildPlayback();
        juce::String m = juce::String::formatted ("Done: %.1f LUFS, true peak %.1f dB", rep.after.lufs, rep.after.truePeakDb);
        if (rep.declipped > 0) m << juce::String::formatted ("  -  rebuilt %lld clipped samples", rep.declipped);
        if (rep.denoiseEased) m << "  -  no clear pauses to learn the noise from, so noise reduction was eased off";
        setMessage (m);
        progressV.store (1.f);
        stateV.store (Idle);
        if (onWake) onWake();
    });
}

// ------------------------------------------------------------------------------------------------ export
void PodcastSession::exportFile (const juce::File& out, int bitDepth)
{
    startJob (Exporting, [this, out, bitDepth]
    {
        std::shared_ptr<juce::AudioBuffer<float>> buf;
        Info in;
        { const juce::ScopedLock sl (lockInfo); buf = processed; in = infoV; }
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); if (onWake) onWake(); };
        if (buf == nullptr) { fail ("Run PODCAST READY first, then export."); return; }
        setMessage ("Writing " + out.getFileName() + "...");
        const bool flac = out.hasFileExtension ("flac");
        const int depth = flac ? std::min (bitDepth, 24) : bitDepth;
        const int outCh = (in.used.mono || buf->getNumChannels() == 1) ? 1 : 2;

        out.deleteFile();
        auto stream = out.createOutputStream();
        if (stream == nullptr) { fail ("Couldn't write to " + out.getFullPathName()); return; }
        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (flac) { juce::FlacAudioFormat f; writer.reset (f.createWriterFor (stream.get(), in.sampleRate, (unsigned) outCh, depth, {}, 5)); }
        else      { juce::WavAudioFormat f;  writer.reset (f.createWriterFor (stream.get(), in.sampleRate, (unsigned) outCh, depth, {}, 0)); }
        if (writer == nullptr) { fail ("Couldn't create the output file writer."); return; }
        stream.release();

        std::mt19937 rng (4321);
        std::uniform_real_distribution<float> u (-0.5f, 0.5f);
        const int n = buf->getNumSamples();
        constexpr int chunk = 1 << 15;
        std::vector<float> d0 ((size_t) chunk), d1 ((size_t) chunk);
        for (int pos = 0; pos < n; pos += chunk)
        {
            if (cancelFlag.load()) { writer.reset(); out.deleteFile(); stateV.store (Idle); return; }
            const int m = std::min (chunk, n - pos);
            const float* a = buf->getReadPointer (0) + pos;
            const float* b = buf->getReadPointer (buf->getNumChannels() > 1 ? 1 : 0) + pos;
            std::copy (a, a + m, d0.begin()); std::copy (b, b + m, d1.begin());
            if (depth == 16)
            {
                const float lsb = 1.f / 32768.f;
                for (int i = 0; i < m; ++i) { d0[(size_t) i] += (u (rng) + u (rng)) * lsb; d1[(size_t) i] += (u (rng) + u (rng)) * lsb; }
            }
            for (int i = 0; i < m; ++i) { d0[(size_t) i] = std::clamp (d0[(size_t) i], -1.f, 1.f); d1[(size_t) i] = std::clamp (d1[(size_t) i], -1.f, 1.f); }
            const float* ch[2] = { d0.data(), d1.data() };
            writer->writeFromFloatArrays (ch, outCh, m);
            progressV.store ((float) pos / (float) n);
        }
        writer.reset();
        {
            const juce::ScopedLock sl (lockInfo);
            exportDone = true;
            exportMsg = juce::String::formatted ("Saved %s  -  %.1f LUFS, true peak %.1f dB", out.getFileName().toRawUTF8(), in.report.after.lufs, in.report.after.truePeakDb);
        }
        setMessage (exportMsg);
        progressV.store (1.f);
        stateV.store (Idle);
        if (onWake) onWake();
    });
}

// ------------------------------------------------------------------------------------------------ playback
std::shared_ptr<juce::AudioBuffer<float>> PodcastSession::resampled (const std::shared_ptr<juce::AudioBuffer<float>>& src, double srcSr, double dstSr)
{
    if (src == nullptr || std::abs (srcSr - dstSr) < 1.0) return src;
    const double ratio = srcSr / dstSr;
    const int outLen = (int) std::ceil ((double) src->getNumSamples() / ratio);
    auto pb = std::make_shared<juce::AudioBuffer<float>> (src->getNumChannels(), outLen);
    for (int ch = 0; ch < src->getNumChannels(); ++ch)
    {
        juce::LagrangeInterpolator interp;
        interp.process (ratio, src->getReadPointer (ch), pb->getWritePointer (ch), outLen);
    }
    return pb;
}

void PodcastSession::prepare (double hostSampleRate)
{
    if (std::abs (hostSampleRate - hostSr) < 0.5 && playPtrOrig.load() != nullptr) return;
    hostSr = hostSampleRate;
    if (! busy()) rebuildPlayback();
}

void PodcastSession::rebuildPlayback()
{
    std::shared_ptr<juce::AudioBuffer<float>> o, p;
    double srcSr = 0;
    { const juce::ScopedLock sl (lockInfo); o = original; p = processed; srcSr = infoV.sampleRate; }
    if (o == nullptr) { playPtrOrig.store (nullptr); playPtrProc.store (nullptr); return; }
    auto po = resampled (o, srcSr, hostSr);
    auto pp = resampled (p, srcSr, hostSr);
    const double frac = position();
    keep[(size_t) keepPos] = po; keepPos = (keepPos + 1) % (int) keep.size();
    keep[(size_t) keepPos] = pp; keepPos = (keepPos + 1) % (int) keep.size();
    playPos.store ((long long) (frac * po->getNumSamples()));
    playPtrOrig.store (po.get());
    playPtrProc.store (pp != nullptr ? pp.get() : nullptr);
}

double PodcastSession::position() const
{
    auto* b = playPtrOrig.load();
    if (b == nullptr || b->getNumSamples() == 0) return 0.0;
    return std::clamp ((double) playPos.load() / (double) b->getNumSamples(), 0.0, 1.0);
}

void PodcastSession::seek (double normalised)
{
    if (auto* b = playPtrOrig.load())
        playPos.store ((long long) (std::clamp (normalised, 0.0, 1.0) * (double) b->getNumSamples()));
}

void PodcastSession::readPlayback (float* l, float* r, int n)
{
    const auto* pr = playPtrProc.load();
    const bool wantOrig = abOriginal.load() || pr == nullptr;
    const auto* b = wantOrig ? playPtrOrig.load() : pr;
    if (b == nullptr || ! playing.load())
    {
        std::fill (l, l + n, 0.f); std::fill (r, r + n, 0.f);
        return;
    }
    const float gain = (wantOrig && pr != nullptr && matchVolume.load()) ? matchGainLin.load() : 1.f;
    const int len = b->getNumSamples();
    const float* sl = b->getReadPointer (0);
    const float* sr_ = b->getReadPointer (b->getNumChannels() > 1 ? 1 : 0);
    long long pos = playPos.load();
    int done = 0;
    while (done < n)
    {
        if (pos >= len)
        {
            if (loop.load()) pos = 0;
            else { std::fill (l + done, l + n, 0.f); std::fill (r + done, r + n, 0.f); playing.store (false); pos = 0; break; }
        }
        const int m = (int) std::min<long long> (n - done, len - pos);
        for (int i = 0; i < m; ++i) { l[done + i] = sl[pos + i] * gain; r[done + i] = sr_[pos + i] * gain; }
        pos += m; done += m;
    }
    playPos.store (pos);
}
} // namespace aio
