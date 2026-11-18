#include "PodcastSession.h"
#include <random>
#include <cmath>
#include <algorithm>

namespace aio
{
// =================================================================================================================
// Disk-backed store: 24-bit WAV (optionally scaled down so a signal that goes over full scale still fits)
// =================================================================================================================
class FileStore : public podcast::Store
{
public:
    FileStore (const juce::File& f, double sr, int nch, float scale) : file (f), rate (sr), channels (nch), gain (scale) {}
    ~FileStore() override { reader.reset(); writer.reset(); }

    bool begin()
    {
        file.deleteFile();
        auto stream = file.createOutputStream();
        if (stream == nullptr) return false;
        juce::WavAudioFormat wav;
        writer.reset (wav.createWriterFor (stream.get(), rate, (unsigned) channels, 24, {}, 0));
        if (writer == nullptr) return false;
        stream.release();
        return true;
    }

    bool append (const float* l, const float* r, int n) override
    {
        if (writer == nullptr) return false;
        tmpL.resize ((size_t) n); tmpR.resize ((size_t) n);
        for (int i = 0; i < n; ++i) { tmpL[(size_t) i] = std::clamp (l[i] * gain, -1.f, 1.f); tmpR[(size_t) i] = std::clamp (r[i] * gain, -1.f, 1.f); }
        const float* ch[2] = { tmpL.data(), tmpR.data() };
        if (! writer->writeFromFloatArrays (ch, channels, n)) return false;
        written += n;
        return true;
    }

    bool finishWrite() override
    {
        writer.reset();
        juce::WavAudioFormat wav;
        auto stream = file.createInputStream();
        if (stream == nullptr) return false;
        reader.reset (wav.createReaderFor (stream.get(), true));
        if (reader == nullptr) return false;
        stream.release();
        return reader->lengthInSamples == written;
    }

    bool read (float* l, float* r, long long pos, int n) override
    {
        if (reader == nullptr) return false;
        tmpL.resize ((size_t) n); tmpR.resize ((size_t) n);
        float* dst[2] = { tmpL.data(), tmpR.data() };
        const int avail = (int) std::max<long long> (0, std::min<long long> (n, written - pos));
        if (avail > 0 && ! reader->read (dst, channels, pos, avail)) return false;
        const float inv = 1.f / gain;
        for (int i = 0; i < n; ++i)
        {
            const float a = i < avail ? tmpL[(size_t) i] * inv : 0.f;
            l[i] = a;
            r[i] = channels > 1 ? (i < avail ? tmpR[(size_t) i] * inv : 0.f) : a;
        }
        return true;
    }

    long long length() const override { return written; }
    int numChannels() const { return channels; }
    double sampleRate() const { return rate; }
    juce::File getFile() const { return file; }
    void closeFiles() { reader.reset(); writer.reset(); }

private:
    juce::File file;
    double rate;
    int channels;
    float gain;
    long long written = 0;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    std::unique_ptr<juce::AudioFormatReader> reader;
    std::vector<float> tmpL, tmpR;
};

// =================================================================================================================
// Streaming preview player. A worker thread decodes (and resamples to the host rate) both the original and the
// processed file in step, into a ring buffer; the audio thread only copies out of the ring.
// =================================================================================================================
class PodcastPlayer : private juce::Thread
{
public:
    PodcastPlayer() : juce::Thread ("Podcast preview") { startThread(); }
    ~PodcastPlayer() override { signalThreadShouldExit(); stopThread (3000); }

    std::atomic<bool> loopFlag { true };

    // ---- message thread. Blocks briefly until the worker has let go of any old files.
    void setSources (const juce::File& orig, const juce::File& proc, double srcRate, long long srcFrames, int origChannels, int procChannels)
    {
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            fileO = orig; fileP = proc; srcSr = srcRate; srcLen = srcFrames; chO = origChannels; chP = procChannels;
            hasProc.store (proc != juce::File());
            totalHost = hostSr > 0 && srcSr > 0 ? (long long) std::ceil ((double) srcLen * hostSr / srcSr) : 0;
            posHost = std::min (posHost, std::max<long long> (0, totalHost - 1));
            resetPosHost = posHost; ++epoch; rIdx = wIdx = 0;
            dirty = true;
        }
        for (int i = 0; i < 1500 && dirtyFlag(); ++i) juce::Thread::sleep (1);
    }

    void setHostRate (double sr)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (std::abs (sr - hostSr) < 0.5) return;
        const double frac = totalHost > 0 ? (double) posHost / (double) totalHost : 0.0;
        hostSr = sr;
        totalHost = srcSr > 0 ? (long long) std::ceil ((double) srcLen * hostSr / srcSr) : 0;
        posHost = (long long) (frac * (double) totalHost);
        resetPosHost = posHost; ++epoch; rIdx = wIdx = 0;
        dirty = true;
        posA.store (frac);
    }

    void seek (double norm)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        if (totalHost <= 0) return;
        posHost = (long long) (std::clamp (norm, 0.0, 1.0) * (double) (totalHost - 1));
        resetPosHost = posHost; ++epoch; rIdx = wIdx = 0;
        posA.store ((double) posHost / (double) totalHost);
    }

    double position() const { return posA.load(); }
    bool hasProcessed() const { return hasProc.load(); }
    int bufferedMs() const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        return hostSr > 0 ? (int) (1000.0 * (double) (wIdx - rIdx) / hostSr) : 0;
    }
    int underruns() const { return underrunCount.load(); }
    bool ready() const { const juce::SpinLock::ScopedLockType sl (lock); return totalHost > 0 && ! dirty; }

    // ---- audio thread. Returns false when it had nothing for you (silence is written), `ended` is set at the end of a non-looping file.
    bool read (float* l, float* r, int n, bool wantOrig, float gain, bool& ended)
    {
        ended = false;
        if (! lock.tryEnter()) { fillSilence (l, r, n); return false; }
        const bool useProc = ! wantOrig && hasProc.load();
        bool ok = false;
        if (! dirty && totalHost > 0 && wIdx - rIdx >= n)
        {
            const auto& A = ring[useProc ? 2 : 0]; const auto& B = ring[useProc ? 3 : 1];
            for (int i = 0; i < n; ++i)
            {
                const size_t k = (size_t) ((rIdx + i) % cap);
                l[i] = A[k] * gain; r[i] = B[k] * gain;
            }
            rIdx += n; posHost += n;
            ok = true;
            if (posHost >= totalHost)
            {
                if (loopFlag.load()) posHost -= totalHost;
                else { ended = true; posHost = 0; resetPosHost = 0; ++epoch; rIdx = wIdx = 0; }
            }
            posA.store ((double) posHost / (double) std::max<long long> (1, totalHost));
        }
        else
        {
            fillSilence (l, r, n);
            if (! dirty && totalHost > 0) ++underrunCount;
        }
        lock.exit();
        return ok;
    }

private:
    static void fillSilence (float* l, float* r, int n) { std::fill (l, l + n, 0.f); std::fill (r, r + n, 0.f); }
    bool dirtyFlag() const { const juce::SpinLock::ScopedLockType sl (lock); return dirty; }

    struct Src
    {
        std::unique_ptr<juce::AudioFormatReader> rd;
        juce::LagrangeInterpolator interp[2];
        std::vector<float> in[2];
        void open (const juce::File& f, juce::AudioFormatManager& fm) { rd.reset (f != juce::File() ? fm.createReaderFor (f) : nullptr); interp[0].reset(); interp[1].reset(); }
        void close() { rd.reset(); }
        void resetInterp() { interp[0].reset(); interp[1].reset(); }
    };

    // decodes `nOut` host frames starting at the source cursor into outL/outR; returns the number of source frames consumed
    int decode (Src& s, long long cursor, double ratio, int nOut, float* outL, float* outR)
    {
        if (s.rd == nullptr) { std::fill (outL, outL + nOut, 0.f); std::fill (outR, outR + nOut, 0.f); return 0; }
        const bool direct = std::abs (ratio - 1.0) < 1.0e-6;
        const int nIn = direct ? nOut : (int) std::ceil ((double) nOut * ratio) + 8;
        for (auto& v : s.in) v.assign ((size_t) nIn, 0.f);
        const long long len = s.rd->lengthInSamples;
        const int avail = (int) std::max<long long> (0, std::min<long long> (nIn, len - cursor));
        if (avail > 0)
        {
            float* dst[2] = { s.in[0].data(), s.in[1].data() };
            const int nc = (int) std::min<unsigned> (2u, s.rd->numChannels);
            s.rd->read (dst, nc, cursor, avail);
            if (nc < 2) std::copy (s.in[0].begin(), s.in[0].end(), s.in[1].begin());
        }
        if (direct)
        {
            std::copy (s.in[0].begin(), s.in[0].begin() + nOut, outL);
            std::copy (s.in[1].begin(), s.in[1].begin() + nOut, outR);
            return nOut;
        }
        const int used = s.interp[0].process (ratio, s.in[0].data(), outL, nOut);
        s.interp[1].process (ratio, s.in[1].data(), outR, nOut);
        return used;
    }

    void run() override
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        Src orig, proc;
        constexpr int block = 4096;
        std::vector<float> bl[4];
        for (auto& v : bl) v.assign ((size_t) block, 0.f);
        long long gen = 0, cursor = 0, myEpoch = -1;
        double ratio = 1.0;
        long long total = 0;
        bool lastLoop = loopFlag.load();

        while (! threadShouldExit())
        {
            juce::File fo, fp;
            bool reopen = false;
            {
                const juce::SpinLock::ScopedLockType sl (lock);
                if (dirty) { fo = fileO; fp = fileP; reopen = true; }
                if (reopen || epoch != myEpoch)
                {
                    myEpoch = epoch; gen = resetPosHost;
                    ratio = hostSr > 0 ? srcSr / hostSr : 1.0;
                    total = totalHost;
                    cursor = (long long) std::llround ((double) gen * ratio);
                }
            }
            if (reopen)
            {
                orig.open (fo, fm); proc.open (fp, fm);
                const juce::SpinLock::ScopedLockType sl (lock);
                dirty = false;
            }
            orig.resetInterp(); proc.resetInterp();      // (cheap; only matters right after a reset, harmless otherwise because we re-create the phase below)

            bool wantLoop = loopFlag.load();
            if (wantLoop != lastLoop)
            {
                lastLoop = wantLoop;
                const juce::SpinLock::ScopedLockType sl (lock);      // the ring holds data wrapped for the old setting: start again from where we are playing
                resetPosHost = posHost; ++epoch; rIdx = wIdx = 0;
                continue;
            }

            bool haveRoom;
            {
                const juce::SpinLock::ScopedLockType sl (lock);
                haveRoom = ! dirty && total > 0 && (cap - (wIdx - rIdx)) >= block && epoch == myEpoch;
            }
            if (! haveRoom || orig.rd == nullptr) { juce::Thread::sleep (4); continue; }

            // one block. Past the end of the file: wrap when looping, otherwise silence (the audio thread stops at the end).
            int nOut = block;
            bool atEnd = gen >= total;
            if (atEnd && wantLoop) { gen = 0; cursor = 0; atEnd = false; orig.resetInterp(); proc.resetInterp(); }
            if (! atEnd) nOut = (int) std::min<long long> (block, total - gen);
            if (atEnd)
            {
                for (auto& v : bl) std::fill (v.begin(), v.end(), 0.f);
            }
            else
            {
                const int usedO = decode (orig, cursor, ratio, nOut, bl[0].data(), bl[1].data());
                if (proc.rd != nullptr) decode (proc, cursor, ratio, nOut, bl[2].data(), bl[3].data());
                else { std::fill (bl[2].begin(), bl[2].end(), 0.f); std::fill (bl[3].begin(), bl[3].end(), 0.f); }
                cursor += usedO; gen += nOut;
            }
            {
                const juce::SpinLock::ScopedLockType sl (lock);
                if (epoch == myEpoch && ! dirty)
                {
                    for (int i = 0; i < nOut; ++i)
                    {
                        const size_t k = (size_t) ((wIdx + i) % cap);
                        for (int c = 0; c < 4; ++c) ring[c][k] = bl[c][(size_t) i];
                    }
                    wIdx += nOut;
                }
            }
            if (atEnd) juce::Thread::sleep (2);
        }
    }

    static constexpr long long cap = 1 << 17;
    mutable juce::SpinLock lock;
    std::vector<float> ring[4] = { std::vector<float> ((size_t) cap, 0.f), std::vector<float> ((size_t) cap, 0.f),
                                   std::vector<float> ((size_t) cap, 0.f), std::vector<float> ((size_t) cap, 0.f) };
    long long rIdx = 0, wIdx = 0, posHost = 0, resetPosHost = 0, totalHost = 0, epoch = 0, srcLen = 0;
    double hostSr = 44100.0, srcSr = 0.0;
    int chO = 0, chP = 0;
    juce::File fileO, fileP;
    bool dirty = false;
    std::atomic<bool> hasProc { false };
    std::atomic<double> posA { 0.0 };
    std::atomic<int> underrunCount { 0 };
};

// =================================================================================================================
class PodcastSession::Job : public juce::Thread
{
public:
    explicit Job (std::function<void()> f) : juce::Thread ("Podcast job"), fn (std::move (f)) { startThread(); }
    ~Job() override { stopThread (180000); }
    void run() override { fn(); }
private:
    std::function<void()> fn;
};

namespace
{
// sequential-friendly reader over the user's file; the right channel is a copy of the left for mono files
struct SourceReader
{
    std::unique_ptr<juce::AudioFormatReader> rd;
    juce::AudioBuffer<float> tmp;
    bool read (float* l, float* r, long long pos, int n)
    {
        if (rd == nullptr) return false;
        if (tmp.getNumSamples() < n) tmp.setSize (2, n, false, false, true);
        tmp.clear (0, n);
        if (! rd->read (&tmp, 0, n, pos, true, true)) return false;
        std::copy (tmp.getReadPointer (0), tmp.getReadPointer (0) + n, l);
        std::copy (tmp.getReadPointer (rd->numChannels >= 2 ? 1 : 0), tmp.getReadPointer (rd->numChannels >= 2 ? 1 : 0) + n, r);
        return true;
    }
};
}

PodcastSession::PodcastSession()
{
    formats.registerBasicFormats();
    player = std::make_unique<PodcastPlayer>();
    // clean up after a crash: old temporary folders
    auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory);
    for (const auto& d : tmp.findChildFiles (juce::File::findDirectories, false, "AIOStudioPodcast_*"))
        if (d.getCreationTime() < juce::Time::getCurrentTime() - juce::RelativeTime::hours (24)) d.deleteRecursively();
}

PodcastSession::~PodcastSession()
{
    cancelFlag.store (true);
    job.reset();
    player.reset();
    finalStore.reset();
    clearTemp();
}

void PodcastSession::clearTemp()
{
    if (tempDir != juce::File() && tempDir.exists()) tempDir.deleteRecursively();
    tempDir = juce::File();
}

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

// ------------------------------------------------------------------------------------------------ load
void PodcastSession::loadFile (const juce::File& file)
{
    startJob (Loading, [this, file]
    {
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); progressV.store (0.f); if (onWake) onWake(); };
        setMessage ("Reading " + file.getFileName() + "...");
        playing.store (false);
        resultPlayable.store (false);
        player->setSources ({}, {}, 0.0, 0, 0, 0);
        finalStore.reset();
        clearTemp();

        SourceReader sr;
        sr.rd.reset (formats.createReaderFor (file));
        if (sr.rd == nullptr) { fail ("Couldn't read that file (unsupported or damaged)."); return; }
        const long long len = sr.rd->lengthInSamples;
        const double rate = sr.rd->sampleRate;
        const int nch = sr.rd->numChannels >= 2 ? 2 : 1;
        if (len < 8192 || rate < 8000.0) { fail ("That file is too short or has an unusable sample rate."); return; }
        if ((double) len / rate > kMaxSeconds) { fail ("Files longer than 4 hours aren't supported."); return; }

        setMessage ("Measuring loudness, noise and clipping...");
        auto a = std::make_shared<podcast::Analysis> (podcast::analyse ([&] (float* l, float* r, long long pos, int n) { return sr.read (l, r, pos, n); },
                                                                       len, rate, [this] (float x) { progressV.store (x); return ! cancelFlag.load(); }));
        if (cancelFlag.load()) { stateV.store (Idle); return; }
        if (! a->valid) { fail ("That file is silent (or unreadable) - there is nothing to process."); return; }

        Info in;
        in.hasFile = true; in.name = file.getFileName(); in.sampleRate = rate; in.seconds = (double) len / rate; in.channels = nch;
        in.before = a->stats; in.waveform = a->waveform;
        {
            const juce::ScopedLock sl (lockInfo);
            infoV = in; exportDone = false;
        }
        sourceFile = file; analysis = a; sourceDual = nch > 1;
        player->setHostRate (hostSrCache);
        player->setSources (file, {}, rate, len, nch, 0);
        setMessage (juce::String::formatted ("Loaded %s  -  %.1f LUFS, true peak %.1f dB", file.getFileName().toRawUTF8(), a->stats.lufs, a->stats.truePeakDb));
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
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); if (onWake) onWake(); };
        std::shared_ptr<podcast::Analysis> a = analysis;
        Info in;
        { const juce::ScopedLock sl (lockInfo); in = infoV; }
        if (a == nullptr || ! in.hasFile) { fail ("Load a file first."); return; }

        // let go of the previous result before replacing it
        playing.store (false);
        resultPlayable.store (false);
        player->setSources (sourceFile, {}, in.sampleRate, a->length, in.channels, 0);
        finalStore.reset();
        clearTemp();
        {
            const juce::ScopedLock sl (lockInfo);
            infoV.hasResult = false; infoV.waveformAfter.clear();
        }

        const int outCh = (in.channels > 1 && ! o.mono) ? 2 : 1;
        tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("AIOStudioPodcast_" + juce::String (juce::Time::currentTimeMillis()));
        if (! tempDir.createDirectory().wasOk()) { fail ("Couldn't create a temporary folder."); return; }
        const double needBytes = (double) a->length * outCh * 3.0 * 2.0 * 1.15;
        const auto freeBytes = tempDir.getBytesFreeOnVolume();
        if (freeBytes > 0 && (double) freeBytes < needBytes)
        {
            clearTemp();
            fail (juce::String::formatted ("Not enough free disk space for the temporary files (about %.1f GB needed).", needBytes / 1.0e9));
            return;
        }

        setMessage ("Making it podcast ready: repairing, cleaning, levelling, setting loudness...");
        auto pre = std::make_unique<FileStore> (tempDir.getChildFile ("pre.wav"), in.sampleRate, outCh, 0.25f);
        auto out = std::make_shared<FileStore> (tempDir.getChildFile ("result.wav"), in.sampleRate, outCh, 1.f);
        if (! pre->begin() || ! out->begin()) { fail ("Couldn't write the temporary files."); return; }

        SourceReader sr;
        sr.rd.reset (formats.createReaderFor (sourceFile));
        if (sr.rd == nullptr) { fail ("The source file can't be read any more (was it moved?)."); return; }

        podcast::Report rep;
        std::vector<float> wf;
        const bool ok = podcast::processStream ([&] (float* l, float* r, long long pos, int n) { return sr.read (l, r, pos, n); },
                                                a->length, in.sampleRate, in.channels > 1, *a, o, *pre, *out, rep, &wf,
                                                [this] (float x) { progressV.store (x); return ! cancelFlag.load(); });
        pre->closeFiles(); pre.reset();
        tempDir.getChildFile ("pre.wav").deleteFile();
        if (cancelFlag.load()) { out.reset(); clearTemp(); stateV.store (Idle); return; }
        if (! ok) { out.reset(); clearTemp(); fail ("Couldn't process that file (it may be silent, or the disk filled up)."); return; }

        finalStore = out;
        {
            const juce::ScopedLock sl (lockInfo);
            infoV.hasResult = true; infoV.report = rep; infoV.used = o; infoV.waveformAfter = wf;
        }
        double g = rep.after.lufs - in.before.lufs;                       // volume match for A/B: original raised/lowered to the same loudness
        g = std::min (g, -0.5 - in.before.truePeakDb);                    // ...but never into clipping
        matchGainLin.store ((float) std::pow (10.0, std::clamp (g, -40.0, 40.0) / 20.0));
        player->setSources (sourceFile, out->getFile(), in.sampleRate, a->length, in.channels, outCh);
        resultPlayable.store (true);

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
        std::shared_ptr<FileStore> store = finalStore;
        Info in;
        { const juce::ScopedLock sl (lockInfo); in = infoV; }
        auto fail = [this] (const juce::String& m) { setMessage (m); stateV.store (Failed); if (onWake) onWake(); };
        if (store == nullptr || ! in.hasResult) { fail ("Run PODCAST READY first, then export."); return; }
        setMessage ("Writing " + out.getFileName() + "...");
        const bool flac = out.hasFileExtension ("flac");
        const int depth = flac ? std::min (bitDepth, 24) : bitDepth;
        const int outCh = store->numChannels();

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
        const long long n = store->length();
        constexpr int chunk = 1 << 15;
        std::vector<float> d0 ((size_t) chunk), d1 ((size_t) chunk);
        for (long long pos = 0; pos < n; pos += chunk)
        {
            if (cancelFlag.load()) { writer.reset(); out.deleteFile(); stateV.store (Idle); return; }
            const int m = (int) std::min<long long> (chunk, n - pos);
            if (! store->read (d0.data(), d1.data(), pos, m)) { writer.reset(); out.deleteFile(); fail ("Couldn't read the processed audio back from disk."); return; }
            if (depth == 16)
            {
                const float lsb = 1.f / 32768.f;
                for (int i = 0; i < m; ++i) { d0[(size_t) i] += (u (rng) + u (rng)) * lsb; d1[(size_t) i] += (u (rng) + u (rng)) * lsb; }
            }
            for (int i = 0; i < m; ++i) { d0[(size_t) i] = std::clamp (d0[(size_t) i], -1.f, 1.f); d1[(size_t) i] = std::clamp (d1[(size_t) i], -1.f, 1.f); }
            const float* ch[2] = { d0.data(), d1.data() };
            if (! writer->writeFromFloatArrays (ch, outCh, m)) { writer.reset(); out.deleteFile(); fail ("Couldn't write the file (disk full?)."); return; }
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
void PodcastSession::prepare (double hostSampleRate)
{
    hostSrCache = hostSampleRate;
    player->setHostRate (hostSampleRate);
}

void PodcastSession::setPlaying (bool p)
{
    if (p && ! info().hasFile) return;
    playing.store (p);
}

void PodcastSession::seek (double normalised) { player->seek (normalised); }
double PodcastSession::position() const { return player->position(); }
int PodcastSession::bufferedMs() const { return player->bufferedMs(); }
int PodcastSession::underruns() const { return player->underruns(); }

void PodcastSession::readPlayback (float* l, float* r, int n)
{
    player->loopFlag.store (loop.load());
    if (! playing.load()) { std::fill (l, l + n, 0.f); std::fill (r, r + n, 0.f); return; }
    const bool hasRes = resultPlayable.load() && player->hasProcessed();
    const bool wantOrig = abOriginal.load() || ! hasRes;
    const float gain = (wantOrig && hasRes && matchVolume.load()) ? matchGainLin.load() : 1.f;
    bool ended = false;
    player->read (l, r, n, wantOrig, gain, ended);
    if (ended) playing.store (false);
}
} // namespace aio
