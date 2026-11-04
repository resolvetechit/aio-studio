#include "MasterSession.h"
#include <random>
#include <cstring>

namespace aio
{
// =================================================================================================================
// LiveLearn
// =================================================================================================================
LiveLearn::LiveLearn() : juce::Thread ("AIO learn")
{
    bufL.assign ((size_t) kFifo, 0.f); bufR.assign ((size_t) kFifo, 0.f);
    tmpL.assign (4096, 0.f); tmpR.assign (4096, 0.f);
    analyser.prepare (sr);
    startThread();
}

LiveLearn::~LiveLearn() { stopThread (2000); }

void LiveLearn::prepare (double sampleRate)
{
    std::lock_guard<std::mutex> g (lock);
    sr = sampleRate;
    analyser.prepare (sr);
    fifo.reset();
    learning.store (false);
}

void LiveLearn::start()
{
    {
        std::lock_guard<std::mutex> g (lock);
        analyser.prepare (sr);
        fifo.reset();
    }
    learnedSeconds.store (0.0);
    learning.store (true);
}

MasterAnalysis LiveLearn::stop()
{
    learning.store (false);
    drain();
    std::lock_guard<std::mutex> g (lock);
    return analyser.result();
}

void LiveLearn::feed (const float* l, const float* r, int n)
{
    if (! learning.load (std::memory_order_relaxed)) return;
    const int w = std::min (n, fifo.getFreeSpace());
    if (w <= 0) return;
    int s1, n1, s2, n2;
    fifo.prepareToWrite (w, s1, n1, s2, n2);
    if (n1 > 0) { std::memcpy (bufL.data() + s1, l, (size_t) n1 * sizeof (float)); std::memcpy (bufR.data() + s1, r, (size_t) n1 * sizeof (float)); }
    if (n2 > 0) { std::memcpy (bufL.data() + s2, l + n1, (size_t) n2 * sizeof (float)); std::memcpy (bufR.data() + s2, r + n1, (size_t) n2 * sizeof (float)); }
    fifo.finishedWrite (n1 + n2);
}

void LiveLearn::drain()
{
    std::lock_guard<std::mutex> g (lock);
    for (;;)
    {
        const int n = std::min (fifo.getNumReady(), 4096);
        if (n <= 0) break;
        int s1, n1, s2, n2;
        fifo.prepareToRead (n, s1, n1, s2, n2);
        if (n1 > 0) { std::memcpy (tmpL.data(), bufL.data() + s1, (size_t) n1 * sizeof (float)); std::memcpy (tmpR.data(), bufR.data() + s1, (size_t) n1 * sizeof (float)); }
        if (n2 > 0) { std::memcpy (tmpL.data() + n1, bufL.data() + s2, (size_t) n2 * sizeof (float)); std::memcpy (tmpR.data() + n1, bufR.data() + s2, (size_t) n2 * sizeof (float)); }
        fifo.finishedRead (n1 + n2);
        analyser.push (tmpL.data(), tmpR.data(), n1 + n2);
        double e = 0; for (int i = 0; i < n1 + n2; ++i) e += (double) tmpL[(size_t) i] * tmpL[(size_t) i];
        levelDb.store ((float) (10.0 * std::log10 (e / (n1 + n2) + 1.0e-12)));
    }
    learnedSeconds.store (analyser.seconds());
}

void LiveLearn::run()
{
    while (! threadShouldExit())
    {
        if (learning.load()) drain();
        wait (25);
    }
}

// =================================================================================================================
// MasterSession
// =================================================================================================================
class MasterSession::Job : public juce::Thread
{
public:
    explicit Job (std::function<void()> f) : juce::Thread ("AIO master job"), fn (std::move (f)) { startThread(); }
    ~Job() override { stopThread (60000); }
    void run() override { fn(); }
private:
    std::function<void()> fn;
};

MasterSession::MasterSession() { formats.registerBasicFormats(); }
MasterSession::~MasterSession() { cancelFlag.store (true); job.reset(); }

void MasterSession::setMessage (const juce::String& m) { const juce::ScopedLock sl (lockInfo); msg = m; }
juce::String MasterSession::message() const { const juce::ScopedLock sl (lockInfo); return msg; }
MasterSession::Info MasterSession::info() const { const juce::ScopedLock sl (lockInfo); return infoV; }

void MasterSession::startJob (int initialState, std::function<void()> fn)
{
    cancelFlag.store (true);
    job.reset();                      // waits for the previous job to notice the cancel flag
    cancelFlag.store (false);
    stateV.store (initialState);      // busy() is true as soon as the call returns, not when the worker gets scheduled
    progressV.store (0.f);
    job = std::make_unique<Job> (std::move (fn));
}

void MasterSession::cancel() { cancelFlag.store (true); }

bool MasterSession::takeResult (MasterSettings& s, RenderStats& st)
{
    const juce::ScopedLock sl (lockInfo);
    if (! resultReady) return false;
    resultReady = false; s = resultSettings; st = resultStats;
    return true;
}

bool MasterSession::takeExportDone (juce::String& m)
{
    const juce::ScopedLock sl (lockInfo);
    if (! exportDone) return false;
    exportDone = false; m = exportMsg;
    return true;
}

// ------------------------------------------------------------------------------------------------ load + analyse
void MasterSession::loadFile (const juce::File& file)
{
    startJob (Loading, [this, file]
    {
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); progressV.store (0.f); if (onWake) onWake(); };
        stateV.store (Loading); progressV.store (0.f);
        setMessage ("Reading " + file.getFileName() + "...");

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr) { fail ("Couldn't read that file (unsupported or damaged)."); return; }
        const long long len = reader->lengthInSamples;
        const double sr = reader->sampleRate;
        if (len < 2048 || sr < 8000.0) { fail ("That file is too short or has an unusable sample rate."); return; }
        if ((double) len > sr * 60.0 * 20.0) { fail ("Files longer than 20 minutes aren't supported."); return; }

        auto buf = std::make_shared<juce::AudioBuffer<float>> (2, (int) len);
        constexpr int chunk = 1 << 16;
        juce::AudioBuffer<float> tmp (2, chunk);
        for (long long pos = 0; pos < len; pos += chunk)
        {
            if (cancelFlag.load()) { stateV.store (Idle); return; }
            const int n = (int) std::min<long long> (chunk, len - pos);
            tmp.clear();
            reader->read (&tmp, 0, n, pos, true, true);
            if (reader->numChannels < 2) tmp.copyFrom (1, 0, tmp, 0, 0, n);
            buf->copyFrom (0, (int) pos, tmp, 0, 0, n);
            buf->copyFrom (1, (int) pos, tmp, 1, 0, n);
            progressV.store (0.35f * (float) pos / (float) len);
        }

        stateV.store (Analysing);
        setMessage ("Analysing loudness, spectrum and stereo image...");
        const auto a = analyseBuffer (buf->getReadPointer (0), buf->getReadPointer (1), (size_t) len, sr,
                                      [this] (float x) { progressV.store (0.35f + 0.65f * x); return ! cancelFlag.load(); });
        if (cancelFlag.load()) { stateV.store (Idle); return; }

        Info in;
        in.hasFile = true; in.name = file.getFileName(); in.sampleRate = sr; in.seconds = (double) len / sr; in.before = a;
        constexpr int cols = 1200;
        in.waveform.assign ((size_t) cols * 2, 0.f);
        const float* wl = buf->getReadPointer (0); const float* wr = buf->getReadPointer (1);
        for (int c = 0; c < cols; ++c)
        {
            const long long a0 = len * c / cols, a1 = std::max (a0 + 1, len * (c + 1) / cols);
            float mn = 1.f, mx = -1.f;
            const long long step = std::max<long long> (1, (a1 - a0) / 64);
            for (long long i = a0; i < a1; i += step) { const float v = 0.5f * (wl[i] + wr[i]); mn = std::min (mn, v); mx = std::max (mx, v); }
            in.waveform[(size_t) c * 2] = mn; in.waveform[(size_t) c * 2 + 1] = mx;
        }
        {
            const juce::ScopedLock sl (lockInfo);
            original = buf; infoV = in;
            resultReady = false; exportDone = false;
        }
        playing.store (false); playPos.store (0);
        rebuildPlayback();
        setMessage ("Loaded " + file.getFileName());
        progressV.store (1.f);
        stateV.store (Idle);
        if (onWake) onWake();
    });
}

// ------------------------------------------------------------------------------------------------ auto master
void MasterSession::runAutoMaster (const MasterOptions& opts)
{
    startJob (Mastering, [this, opts]
    {
        std::shared_ptr<juce::AudioBuffer<float>> buf;
        Info in;
        { const juce::ScopedLock sl (lockInfo); buf = original; in = infoV; }
        if (buf == nullptr || ! in.before.valid)
        {
            setMessage (buf == nullptr ? "Load a file first." : "That file is silent - nothing to master.");
            stateV.store (Failed); if (onWake) onWake(); return;
        }
        stateV.store (Mastering); progressV.store (0.f);
        setMessage ("Mastering: fitting EQ, setting dynamics, solving loudness...");

        auto s0 = deriveMaster (in.before, opts);
        RenderStats st;
        auto s = refineMaster (buf->getReadPointer (0), buf->getReadPointer (1), (size_t) buf->getNumSamples(), in.sampleRate, in.before, s0, opts, &st,
                               [this] (float x) { progressV.store (x); return ! cancelFlag.load(); });
        if (cancelFlag.load()) { stateV.store (Idle); return; }
        {
            const juce::ScopedLock sl (lockInfo);
            resultReady = true; resultSettings = s; resultStats = st;
            infoV.hasAfter = true; infoV.after = st; infoV.hasSettings = true; infoV.settings = s;
        }
        matchGainLin.store (dbToGain (std::clamp ((float) (st.lufs - in.before.lufs), -24.f, 24.f)));
        setMessage (juce::String::formatted ("Done: %.1f LUFS, true peak %.1f dB", st.lufs, st.truePeakDb));
        progressV.store (1.f);
        stateV.store (Idle);
        if (onWake) onWake();
    });
}

// ------------------------------------------------------------------------------------------------ export
void MasterSession::exportFile (const juce::File& out, const ChainParams& p, int bitDepth)
{
    startJob (Exporting, [this, out, p, bitDepth]
    {
        std::shared_ptr<juce::AudioBuffer<float>> buf;
        Info in;
        { const juce::ScopedLock sl (lockInfo); buf = original; in = infoV; }
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); if (onWake) onWake(); };
        if (buf == nullptr) { fail ("Load a file first."); return; }

        stateV.store (Exporting); progressV.store (0.f);
        setMessage ("Rendering " + out.getFileName() + "...");
        out.deleteFile();
        auto stream = out.createOutputStream();
        if (stream == nullptr) { fail ("Couldn't write to " + out.getFullPathName()); return; }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor (stream.get(), in.sampleRate, 2, bitDepth, {}, 0));
        if (writer == nullptr) { fail ("Couldn't create the WAV writer."); return; }
        stream.release();

        std::mt19937 rng (12345);
        std::uniform_real_distribution<float> u (-0.5f, 0.5f);
        std::vector<float> dl, dr;
        auto sink = [&] (const float* l, const float* r, int n)
        {
            if (bitDepth == 16)
            {
                dl.assign (l, l + n); dr.assign (r, r + n);
                const float lsb = 1.f / 32768.f;
                for (int i = 0; i < n; ++i) { dl[(size_t) i] += (u (rng) + u (rng)) * lsb; dr[(size_t) i] += (u (rng) + u (rng)) * lsb; }
                const float* ch[2] = { dl.data(), dr.data() };
                writer->writeFromFloatArrays (ch, 2, n);
            }
            else
            {
                const float* ch[2] = { l, r };
                writer->writeFromFloatArrays (ch, 2, n);
            }
        };
        const auto st = renderPass (buf->getReadPointer (0), buf->getReadPointer (1), (size_t) buf->getNumSamples(), in.sampleRate, p, sink,
                                    [this] (float x) { progressV.store (x); return ! cancelFlag.load(); });
        writer.reset();
        if (st.cancelled) { out.deleteFile(); stateV.store (Idle); return; }
        {
            const juce::ScopedLock sl (lockInfo);
            infoV.hasAfter = true; infoV.after = st;
            exportDone = true;
            exportMsg = juce::String::formatted ("Saved %s  -  %.1f LUFS, true peak %.1f dB", out.getFileName().toRawUTF8(), st.lufs, st.truePeakDb);
        }
        setMessage (exportMsg);
        progressV.store (1.f);
        stateV.store (Idle);
        if (onWake) onWake();
    });
}

// ------------------------------------------------------------------------------------------------ playback
void MasterSession::prepare (double hostSampleRate)
{
    if (std::abs (hostSampleRate - hostSr) < 0.5 && playPtr.load() != nullptr) return;
    hostSr = hostSampleRate;
    if (! busy()) rebuildPlayback();
}

void MasterSession::rebuildPlayback()
{
    std::shared_ptr<juce::AudioBuffer<float>> src;
    double srcSr = 0;
    { const juce::ScopedLock sl (lockInfo); src = original; srcSr = infoV.sampleRate; }
    if (src == nullptr) { playPtr.store (nullptr); return; }

    std::shared_ptr<juce::AudioBuffer<float>> pb;
    if (std::abs (srcSr - hostSr) < 1.0) pb = src;
    else
    {
        const double ratio = srcSr / hostSr;
        const int outLen = (int) std::ceil ((double) src->getNumSamples() / ratio);
        pb = std::make_shared<juce::AudioBuffer<float>> (2, outLen);
        for (int ch = 0; ch < 2; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, src->getReadPointer (ch), pb->getWritePointer (ch), outLen);
        }
    }
    const double frac = position();
    keep[(size_t) keepPos] = pb; keepPos = (keepPos + 1) % (int) keep.size();
    playPos.store ((long long) (frac * pb->getNumSamples()));
    playPtr.store (pb.get());
}

double MasterSession::position() const
{
    auto* b = playPtr.load();
    if (b == nullptr || b->getNumSamples() == 0) return 0.0;
    return std::clamp ((double) playPos.load() / (double) b->getNumSamples(), 0.0, 1.0);
}

void MasterSession::seek (double normalised)
{
    if (auto* b = playPtr.load())
        playPos.store ((long long) (std::clamp (normalised, 0.0, 1.0) * (double) b->getNumSamples()));
}

void MasterSession::readPlayback (float* l, float* r, int n)
{
    const auto* b = playPtr.load();
    if (b == nullptr || ! playing.load())
    {
        std::fill (l, l + n, 0.f); std::fill (r, r + n, 0.f);
        return;
    }
    const int len = b->getNumSamples();
    const float* sl = b->getReadPointer (0);
    const float* sr_ = b->getReadPointer (1);
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
        std::memcpy (l + done, sl + pos, (size_t) m * sizeof (float));
        std::memcpy (r + done, sr_ + pos, (size_t) m * sizeof (float));
        pos += m; done += m;
    }
    playPos.store (pos);
}
} // namespace aio
