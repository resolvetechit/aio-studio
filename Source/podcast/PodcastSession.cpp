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
// Source reader shared by the load / process / preview code: positional reads, the right channel is a copy of the left for mono
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

// =================================================================================================================
// Streaming preview player.
//
// A worker thread produces blocks of host-rate audio (original, and "podcast ready": either the rendered result file or the
// live chain running on the source) into a ring of slots; the audio thread only copies finished slots out. There are no
// locks on the audio thread: the ring is single-producer / single-consumer with atomic indices, and every slot is stamped
// with an "epoch" so that a seek or a change of source simply makes the stale slots invisible.
// =================================================================================================================
class PodcastPlayer : private juce::Thread
{
public:
    static constexpr int kBlock = 2048, kSlots = 14;

    PodcastPlayer() : juce::Thread ("Podcast preview"), slots (new Slot[kSlots])
    {
        for (auto& v : optV) v.store (0.f);
        startThread();
    }
    ~PodcastPlayer() override { signalThreadShouldExit(); stopThread (4000); }

    std::atomic<bool> loopFlag { true };

    // ---- message thread ------------------------------------------------------------------------------------
    void setSources (const juce::File& orig, const juce::File& proc, double srcRate, long long srcFrames,
                     std::shared_ptr<const podcast::Analysis> analysis, bool dualSource)
    {
        const double frac = posA.load();
        {
            const juce::ScopedLock sl (cs);
            fileO = orig; fileP = proc; srcSr = srcRate; srcLen = srcFrames; an = std::move (analysis); dual = dualSource;
            hasProcFile.store (proc != juce::File());
            hasAnalysis.store (an != nullptr);
            recomputeTotal();
            reqPos.store ((long long) (frac * (double) totalA.load()));
            ++srcVer;
        }
        ++curEpoch;
    }

    void setHostRate (double sr)
    {
        const double frac = posA.load();
        {
            const juce::ScopedLock sl (cs);
            if (std::abs (sr - hostSr) < 0.5) return;
            hostSr = sr;
            recomputeTotal();
            reqPos.store ((long long) (frac * (double) totalA.load()));
            ++srcVer;
        }
        ++curEpoch;
    }

    void setLive (bool on)
    {
        if (liveWanted.exchange (on) == on) return;
        reqPos.store ((long long) (posA.load() * (double) totalA.load()));
        ++curEpoch;
    }
    bool live() const { return liveWanted.load(); }

    void setLiveOptions (const podcast::Options& o)
    {
        optV[0].store (o.declip ? 1.f : 0.f); optV[1].store (o.denoise); optV[2].store (o.level); optV[3].store (o.tone);
        optV[4].store (o.deess); optV[5].store (o.comp); optV[6].store (o.targetLufs); optV[7].store (o.ceilingDb); optV[8].store (o.mono ? 1.f : 0.f);
        optsSet.store (true);
    }

    void seek (double norm)
    {
        const long long tot = totalA.load();
        if (tot <= 0) return;
        norm = std::clamp (norm, 0.0, 1.0);
        reqPos.store ((long long) (norm * (double) (tot - 1)));
        posA.store (norm);
        ++curEpoch;
    }

    double position() const { return posA.load(); }
    bool hasProcessed() const { return hasProcFile.load() || (liveWanted.load() && hasAnalysis.load() && optsSet.load()); }
    bool liveActive() const { return liveWanted.load() && hasAnalysis.load() && optsSet.load(); }
    float liveGainDb() const { return liveGainA.load(); }
    int underruns() const { return underrunCount.load(); }
    int bufferedMs() const
    {
        const long long wr = wrIdx.load (std::memory_order_acquire), rd = std::max (rdIdx.load (std::memory_order_acquire), flushMark.load (std::memory_order_acquire));
        const int ep = curEpoch.load();
        long long frames = 0;
        for (long long i = rd; i < wr; ++i) { const Slot& s = slots[(size_t) (i % kSlots)]; if (s.epoch == ep) frames += s.count; }
        frames -= slotOffA.load();
        const double sr = hostSrA.load();
        return sr > 0 ? (int) (1000.0 * (double) std::max<long long> (0, frames) / sr) : 0;
    }

    // ---- audio thread ----------------------------------------------------------------------------------------
    // Always fills l / r (silence for what it hasn't got). Returns true when it delivered all n frames.
    bool read (float* l, float* r, int n, bool wantOrig, float gain, bool& ended)
    {
        ended = false;
        const int chA = (wantOrig || ! hasProcessed()) ? 0 : 2;
        int done = 0;
        const int ep = curEpoch.load (std::memory_order_acquire);
        long long rd = rdIdx.load (std::memory_order_relaxed);
        const long long fm = flushMark.load (std::memory_order_acquire);          // everything before this was made for an older position
        if (fm > rd) { rd = fm; slotOff = 0; rdIdx.store (rd, std::memory_order_release); }
        while (done < n)
        {
            const long long wr = wrIdx.load (std::memory_order_acquire);
            if (rd == wr) break;
            const Slot& s = slots[(size_t) (rd % kSlots)];
            if (s.epoch != ep) { rd++; rdIdx.store (rd, std::memory_order_release); slotOff = 0; continue; }       // stale: from before a seek / source change
            const int take = std::min (n - done, s.count - slotOff);
            for (int i = 0; i < take; ++i) { l[done + i] = s.d[chA][slotOff + i] * gain; r[done + i] = s.d[chA + 1][slotOff + i] * gain; }
            done += take; slotOff += take;
            lastPlayed = s.start + slotOff;
            if (slotOff >= s.count)
            {
                const bool last = s.last;
                slotOff = 0; rd++; rdIdx.store (rd, std::memory_order_release);
                if (last) { ended = true; break; }
            }
        }
        slotOffA.store (slotOff);
        if (done < n)
        {
            std::fill (l + done, l + n, 0.f); std::fill (r + done, r + n, 0.f);
            if (! ended && totalA.load() > 0) ++underrunCount;
        }
        const long long tot = totalA.load();
        if (tot > 0 && done > 0) posA.store ((double) std::min (lastPlayed, tot) / (double) tot);
        return done == n;
    }

private:
    struct Slot
    {
        float d[4][kBlock];
        long long start = 0;
        int count = 0, epoch = -1;
        bool last = false;
    };

    void recomputeTotal()
    {
        totalA.store (hostSr > 0 && srcSr > 0 ? (long long) std::ceil ((double) srcLen * hostSr / srcSr) : 0);
        hostSrA.store (hostSr);
    }

    // ---- worker side ---------------------------------------------------------------------------------------
    struct Src
    {
        std::unique_ptr<juce::AudioFormatReader> rd;
        juce::LagrangeInterpolator interp[2];
        std::vector<float> in[2];
        void open (const juce::File& f, juce::AudioFormatManager& fm) { rd.reset (f != juce::File() ? fm.createReaderFor (f) : nullptr); resetInterp(); }
        void resetInterp() { interp[0].reset(); interp[1].reset(); }
    };

    // the podcast-ready chain running on the source, one instance per option set
    struct LiveStream
    {
        podcast::LiveChain chain;
        podcast::Options opts;
        long long inPos = 0, outStart = 0, srcLen = 0;
        SourceReader* reader;
        std::vector<float> oL, oR, bl, br;

        LiveStream (std::shared_ptr<const podcast::Analysis> a, const podcast::Options& o, double sr, bool dual, double gain0, long long from, long long len, SourceReader* rd)
            : chain (std::move (a), o, sr, dual, gain0), opts (o), inPos (from), outStart (from), srcLen (len), reader (rd) { bl.resize (2048); br.resize (2048); }

        void ensure (long long upTo)
        {
            while (outStart + (long long) oL.size() < upTo)
            {
                const int n = 2048;
                const int real = (int) std::max<long long> (0, std::min<long long> (n, srcLen - inPos));
                std::fill (bl.begin(), bl.end(), 0.f); std::fill (br.begin(), br.end(), 0.f);
                if (real > 0) reader->read (bl.data(), br.data(), inPos, real);
                chain.push (bl.data(), br.data(), n, oL, oR);
                inPos += n;
                if (inPos > srcLen + 40000 && outStart + (long long) oL.size() < upTo) { oL.resize ((size_t) (upTo - outStart), 0.f); oR.resize (oL.size(), 0.f); }
            }
        }
        void get (long long pos, int n, float* L, float* R)
        {
            ensure (pos + n);
            for (int i = 0; i < n; ++i)
            {
                const long long k = pos + i - outStart;
                L[i] = (k >= 0 && k < (long long) oL.size()) ? oL[(size_t) k] : 0.f;
                R[i] = (k >= 0 && k < (long long) oL.size()) ? oR[(size_t) k] : 0.f;
            }
        }
        void trim (long long before)
        {
            const long long cut = std::min<long long> (before - outStart, (long long) oL.size());
            if (cut > 8192) { oL.erase (oL.begin(), oL.begin() + cut); oR.erase (oR.begin(), oR.begin() + cut); outStart += cut; }
        }
    };

    podcast::Options loadOpts() const
    {
        podcast::Options o;
        o.declip = optV[0].load() > 0.5f; o.denoise = optV[1].load(); o.level = optV[2].load(); o.tone = optV[3].load();
        o.deess = optV[4].load(); o.comp = optV[5].load(); o.targetLufs = optV[6].load(); o.ceilingDb = optV[7].load(); o.mono = optV[8].load() > 0.5f;
        return o;
    }
    static bool same (const podcast::Options& a, const podcast::Options& b)
    {
        return a.declip == b.declip && a.mono == b.mono && std::abs (a.denoise - b.denoise) < 1e-4f && std::abs (a.level - b.level) < 1e-4f && std::abs (a.tone - b.tone) < 1e-4f
            && std::abs (a.deess - b.deess) < 1e-4f && std::abs (a.comp - b.comp) < 1e-4f && std::abs (a.targetLufs - b.targetLufs) < 1e-3f && std::abs (a.ceilingDb - b.ceilingDb) < 1e-3f;
    }

    void run() override
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        Src orig, procF;
        SourceReader liveReader;
        std::unique_ptr<LiveStream> live, oldLive;
        std::vector<float> procL, procR;                 // the source-rate "podcast ready" stream, as far as it has been generated
        long long procStart = 0;
        long long xfStart = 0, xfLen = 1;
        double carryGain = 0.0;

        int myVer = -1, myEpoch = -1;
        long long gen = 0, cursor = 0, total = 0, srcLenL = 0;
        double ratio = 1.0, srcRate = 0.0, hostRate = 44100.0;
        bool dualL = false, atEnd = false;
        std::shared_ptr<const podcast::Analysis> anL;
        double lastSwitchMs = 0.0;
        constexpr double prerollSec = 6.0;
        std::vector<float> bl[4];
        for (auto& v : bl) v.assign ((size_t) kBlock, 0.f);

        auto useLive = [&] { return liveWanted.load() && anL != nullptr && optsSet.load() && orig.rd != nullptr; };

        auto makeLive = [&] (const podcast::Options& o, long long at, double gain0)
        {
            const long long from = std::max<long long> (0, at - (long long) (prerollSec * srcRate));
            auto ls = std::make_unique<LiveStream> (anL, o, srcRate, dualL, gain0, from, srcLenL, &liveReader);
            ls->ensure (at);                              // warm-up: the audio before the playhead is thrown away
            return ls;
        };

        // makes sure the source-rate podcast-ready stream covers [from, upTo)
        auto extendProc = [&] (long long from, long long upTo)
        {
            if (procStart + (long long) procL.size() < from) { procL.clear(); procR.clear(); procStart = from; }     // (jumped)
            if (procL.empty()) procStart = from;
            while (procStart + (long long) procL.size() < upTo)
            {
                const long long p = procStart + (long long) procL.size();
                constexpr int n = 2048;
                std::vector<float> aL ((size_t) n), aR ((size_t) n);
                live->get (p, n, aL.data(), aR.data());
                if (oldLive != nullptr)
                {
                    std::vector<float> oLv ((size_t) n), oRv ((size_t) n);
                    oldLive->get (p, n, oLv.data(), oRv.data());
                    for (int i = 0; i < n; ++i)
                    {
                        const double t = std::clamp ((double) (p + i - xfStart) / (double) xfLen, 0.0, 1.0);
                        const float w = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * t));
                        aL[(size_t) i] = oLv[(size_t) i] * (1.f - w) + aL[(size_t) i] * w;
                        aR[(size_t) i] = oRv[(size_t) i] * (1.f - w) + aR[(size_t) i] * w;
                    }
                    if (p + n >= xfStart + xfLen) oldLive.reset();
                }
                procL.insert (procL.end(), aL.begin(), aL.end()); procR.insert (procR.end(), aR.begin(), aR.end());
            }
        };

        // fetches nIn source frames of one of the two streams into s.in
        auto fetch = [&] (Src& s, bool isProc, long long at, int nIn)
        {
            for (auto& v : s.in) v.assign ((size_t) nIn, 0.f);
            if (isProc && live != nullptr)
            {
                extendProc (at, at + nIn);
                for (int i = 0; i < nIn; ++i)
                {
                    const long long k = at + i - procStart;
                    if (k >= 0 && k < (long long) procL.size()) { s.in[0][(size_t) i] = procL[(size_t) k]; s.in[1][(size_t) i] = procR[(size_t) k]; }
                }
                if (procStart < at - 16384 && (long long) procL.size() > 40000)
                {
                    const long long cut = at - 8192 - procStart;
                    procL.erase (procL.begin(), procL.begin() + cut); procR.erase (procR.begin(), procR.begin() + cut); procStart += cut;
                }
                live->trim (at - 16384);
                if (oldLive != nullptr) oldLive->trim (at - 16384);
                return;
            }
            if (s.rd == nullptr) return;
            const long long len = s.rd->lengthInSamples;
            const int avail = (int) std::max<long long> (0, std::min<long long> (nIn, len - at));
            if (avail > 0)
            {
                float* dst[2] = { s.in[0].data(), s.in[1].data() };
                const int nc = (int) std::min<unsigned> (2u, s.rd->numChannels);
                s.rd->read (dst, nc, at, avail);
                if (nc < 2) std::copy (s.in[0].begin(), s.in[0].end(), s.in[1].begin());
            }
        };

        // converts to the host rate: returns the number of source frames used
        auto convert = [&] (Src& s, int nOut, double r, float* outL, float* outR)
        {
            if (std::abs (r - 1.0) < 1.0e-6)
            {
                std::copy (s.in[0].begin(), s.in[0].begin() + nOut, outL); std::copy (s.in[1].begin(), s.in[1].begin() + nOut, outR);
                return nOut;
            }
            const int used = s.interp[0].process (r, s.in[0].data(), outL, nOut);
            s.interp[1].process (r, s.in[1].data(), outR, nOut);
            return used;
        };

        auto dropStreams = [&] { live.reset(); oldLive.reset(); procL.clear(); procR.clear(); procStart = 0; };

        while (! threadShouldExit())
        {
            // ---- new sources / host rate
            if (srcVer.load() != myVer)
            {
                juce::File fo, fp;
                {
                    const juce::ScopedLock sl (cs);
                    fo = fileO; fp = fileP; anL = an; dualL = dual; srcRate = srcSr; srcLenL = srcLen; hostRate = hostSr; myVer = srcVer.load();
                    total = totalA.load();
                }
                orig.open (fo, fm); procF.open (fp, fm);
                liveReader.rd.reset (fo != juce::File() ? fm.createReaderFor (fo) : nullptr);
                dropStreams();
                myEpoch = -1;
                if (anL != nullptr) carryGain = std::clamp ((double) (optsSet.load() ? loadOpts().targetLufs : -16.f) - anL->stats.lufs, -20.0, 30.0);
            }
            // ---- seek / mode change: start again from the requested position
            const int ep = curEpoch.load();
            if (ep != myEpoch)
            {
                myEpoch = ep;
                flushMark.store (wrIdx.load (std::memory_order_relaxed), std::memory_order_release);      // whatever is queued is stale now, even if nobody is playing
                gen = reqPos.load();
                total = totalA.load();
                ratio = hostRate > 0 ? srcRate / hostRate : 1.0;
                cursor = (long long) std::llround ((double) gen * ratio);
                orig.resetInterp(); procF.resetInterp();
                if (live != nullptr) carryGain = live->chain.gainDb();
                dropStreams();
                atEnd = false;
                lastSwitchMs = juce::Time::getMillisecondCounterHiRes();
            }

            // ---- knobs moved: build the new chain (warmed up) and cross-fade to it
            if (useLive() && live != nullptr && oldLive == nullptr)
            {
                const auto now = loadOpts();
                const double ms = juce::Time::getMillisecondCounterHiRes();
                if (! same (now, live->opts) && ms - lastSwitchMs > 300.0)
                {
                    lastSwitchMs = ms;
                    const long long at = procStart + (long long) procL.size();          // everything before this is already in the stream
                    auto ns = makeLive (now, at, live->chain.gainDb());
                    oldLive = std::move (live); live = std::move (ns);
                    xfStart = at; xfLen = std::max<long long> (256, (long long) (0.06 * srcRate));
                }
            }

            // ---- room for another block?
            const long long wr = wrIdx.load (std::memory_order_relaxed);
            if (total <= 0 || orig.rd == nullptr || wr - std::max (rdIdx.load (std::memory_order_acquire), flushMark.load (std::memory_order_acquire)) >= kSlots || (atEnd && ! loopFlag.load()))
            {
                if (atEnd && loopFlag.load()) atEnd = false;
                juce::Thread::sleep (3);
                continue;
            }

            if (gen >= total)
            {
                if (loopFlag.load()) { gen = 0; cursor = 0; orig.resetInterp(); procF.resetInterp(); if (live != nullptr) carryGain = live->chain.gainDb(); dropStreams(); }
                else
                {
                    Slot& s = slots[(size_t) (wr % kSlots)];
                    s.start = total; s.count = 0; s.last = true; s.epoch = myEpoch;
                    wrIdx.store (wr + 1, std::memory_order_release);
                    atEnd = true;
                    continue;
                }
            }
            if (useLive() && live == nullptr) live = makeLive (loadOpts(), cursor, carryGain);

            const int nOut = (int) std::min<long long> (kBlock, total - gen);
            const bool direct = std::abs (ratio - 1.0) < 1.0e-6;
            const int nIn = direct ? nOut : (int) std::ceil ((double) nOut * ratio) + 8;
            fetch (orig, false, cursor, nIn);
            const int used = convert (orig, nOut, ratio, bl[0].data(), bl[1].data());
            if (live != nullptr) { fetch (procF, true, cursor, nIn); convert (procF, nOut, ratio, bl[2].data(), bl[3].data()); }
            else if (procF.rd != nullptr) { fetch (procF, false, cursor, nIn); convert (procF, nOut, ratio, bl[2].data(), bl[3].data()); }
            else { std::fill (bl[2].begin(), bl[2].end(), 0.f); std::fill (bl[3].begin(), bl[3].end(), 0.f); }
            if (live != nullptr) liveGainA.store ((float) live->chain.gainDb());

            if (curEpoch.load() != myEpoch) continue;          // a seek happened while we were busy: this block is stale
            Slot& s = slots[(size_t) (wr % kSlots)];
            for (int c = 0; c < 4; ++c) std::copy (bl[c].begin(), bl[c].begin() + nOut, s.d[c]);
            s.start = gen; s.count = nOut; s.epoch = myEpoch;
            s.last = (gen + nOut >= total) && ! loopFlag.load();
            wrIdx.store (wr + 1, std::memory_order_release);
            if (s.last) atEnd = true;
            cursor += used; gen += nOut;
        }
    }

    juce::CriticalSection cs;                          // message thread <-> worker only; never taken on the audio thread
    std::unique_ptr<Slot[]> slots;
    std::atomic<long long> wrIdx { 0 }, rdIdx { 0 }, flushMark { 0 }, reqPos { 0 }, totalA { 0 };
    std::atomic<int> curEpoch { 0 }, srcVer { 0 }, underrunCount { 0 }, slotOffA { 0 };
    std::atomic<double> posA { 0.0 }, hostSrA { 44100.0 };
    std::atomic<bool> hasProcFile { false }, hasAnalysis { false }, liveWanted { true }, optsSet { false };
    std::atomic<float> liveGainA { 0.f };
    std::atomic<float> optV[9];
    // (audio thread only)
    int slotOff = 0;
    long long lastPlayed = 0;
    // (shared under cs)
    juce::File fileO, fileP;
    double srcSr = 0.0, hostSr = 44100.0;
    long long srcLen = 0;
    std::shared_ptr<const podcast::Analysis> an;
    bool dual = false;
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

PodcastSession::PodcastSession()
{
    formats.registerBasicFormats();
    player = std::make_unique<PodcastPlayer>();
    player->setLive (liveDefault);
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
        player->setSources ({}, {}, 0.0, 0, nullptr, false);
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
        player->setSources (file, {}, rate, len, a, nch > 1);
        if (getOptions) setLiveOptions (getOptions());
        beforeLufs.store ((float) a->stats.lufs); beforePeak.store ((float) a->stats.truePeakDb);
        refreshMatchGain();
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
        player->setSources (sourceFile, {}, in.sampleRate, a->length, a, in.channels > 1);
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
        fileMatchGain.store ((float) std::pow (10.0, std::clamp (g, -40.0, 40.0) / 20.0));
        player->setSources (sourceFile, out->getFile(), in.sampleRate, a->length, a, in.channels > 1);
        resultPlayable.store (true);
        refreshMatchGain();

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
void PodcastSession::setLive (bool on) { player->setLive (on); refreshMatchGain(); }
bool PodcastSession::live() const { return player->live(); }
bool PodcastSession::liveActive() const { return player->liveActive(); }
float PodcastSession::liveGainDb() const { return player->liveGainDb(); }

void PodcastSession::setLiveOptions (const podcast::Options& o)
{
    liveTarget.store (o.targetLufs);
    player->setLiveOptions (o);
    refreshMatchGain();
}

// the original is brought to the loudness of the podcast-ready version for a fair A/B: from the finished run when listening to it,
// from the target when listening to the live chain (which levels towards the target)
void PodcastSession::refreshMatchGain()
{
    if (player->live() && player->liveActive())
    {
        double g = (double) liveTarget.load() - (double) beforeLufs.load();
        g = std::min (g, -0.5 - (double) beforePeak.load());
        matchGainLin.store ((float) std::pow (10.0, std::clamp (g, -40.0, 40.0) / 20.0));
    }
    else matchGainLin.store (fileMatchGain.load());
}

void PodcastSession::readPlayback (float* l, float* r, int n)
{
    player->loopFlag.store (loop.load());
    if (! playing.load()) { std::fill (l, l + n, 0.f); std::fill (r, r + n, 0.f); return; }
    const bool hasProc = player->hasProcessed();
    const bool wantOrig = abOriginal.load() || ! hasProc;
    const float gain = (wantOrig && hasProc && matchVolume.load()) ? matchGainLin.load() : 1.f;
    bool ended = false;
    player->read (l, r, n, wantOrig, gain, ended);
    if (ended) { playing.store (false); player->seek (0.0); }
}
} // namespace aio
