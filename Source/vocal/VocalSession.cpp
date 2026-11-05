#include "VocalSession.h"

namespace aio
{
VocalSession::VocalSession() : juce::Thread ("AIO vocal worker")
{
    formats.registerBasicFormats();
    startThread (juce::Thread::Priority::low);
}

VocalSession::~VocalSession()
{
    cancelAll.store (true);
    cancelRender.store (true);
    signalThreadShouldExit();
    notify();
    stopThread (4000);
}

void VocalSession::prepare (double sr)
{
    if (sr <= 0.0) return;
    const double old = hostSr.exchange (sr);
    if (std::abs (old - sr) > 0.5 && hasClipV.load())
    {
        origBuiltSr = 0.0;
        notesChanged();
    }
}

void VocalSession::setStatus (const juce::String& m)
{
    std::lock_guard<std::mutex> lk (resMx);
    statusMsg = m;
}

juce::String VocalSession::status() const
{
    auto* self = const_cast<VocalSession*> (this);
    std::lock_guard<std::mutex> lk (self->resMx);
    return statusMsg;
}

double VocalSession::renderedSeconds() const
{
    auto* self = const_cast<VocalSession*> (this);
    std::lock_guard<std::mutex> lk (self->resMx);
    return editedShared ? (double) editedShared->getNumSamples() / std::max (1.0, hostSr.load()) : 0.0;
}

std::vector<float> VocalSession::renderedCopy() const
{
    auto* self = const_cast<VocalSession*> (this);
    std::lock_guard<std::mutex> lk (self->resMx);
    if (! editedShared) return {};
    const float* p = editedShared->getReadPointer (0);
    return std::vector<float> (p, p + editedShared->getNumSamples());
}

// =================================================================================================================
// message-thread requests
// =================================================================================================================
void VocalSession::loadFile (const juce::File& f)
{
    if (recModeV.load() != RecIdle) return;
    {
        std::lock_guard<std::mutex> lk (mx);
        pendingFile = f; wantLoad = true;
    }
    busyV.store (true);
    progressV.store (0.f);
    setStatus ("Reading " + f.getFileName() + "...");
    wake();
}

void VocalSession::redetect (const AnalysisOptions& a, const SegmentOptions& s)
{
    {
        std::lock_guard<std::mutex> lk (mx);
        if (! source) return;
        pendingA = a; pendingS = s; wantAnalyse = true;
    }
    busyV.store (true);
    progressV.store (0.f);
    wake();
}

void VocalSession::clear()
{
    {
        std::lock_guard<std::mutex> lk (mx);
        source.reset(); flacValid = false; wantRender = false; wantAnalyse = false;
    }
    {
        std::lock_guard<std::mutex> lk (resMx);
        resDoc = VocalDoc(); resNewClip = true; resChanged = true;
        editedShared.reset(); origShared.reset();
    }
    editedPtr.store (nullptr); origPtr.store (nullptr);
    playing.store (false);
    hasClipV.store (false);
    docV = VocalDoc();
    setStatus ({});
}

void VocalSession::setRenderOptions (const RenderOptions& o)
{
    if (std::abs (o.glideMs - renderOpts.glideMs) < 0.01f && std::abs (o.globalFormant - renderOpts.globalFormant) < 0.001f) return;
    renderOpts = o;
    if (hasClipV.load()) notesChanged();
}

void VocalSession::notesChanged()
{
    {
        std::lock_guard<std::mutex> lk (mx);
        if (! source) return;
        pendingRender.notes = docV.notes;
        pendingRender.opts = renderOpts;
        pendingRender.hop = docV.hop;
        pendingRender.sr = docV.sr;
        pendingRender.serial = renderSerial.fetch_add (1) + 1;
        wantRender = true;
    }
    cancelRender.store (true);
    wake();
}

bool VocalSession::takeAnchor (double& beat)
{
    std::lock_guard<std::mutex> lk (resMx);
    if (! resAnchorValid) return false;
    beat = resAnchor; resAnchorValid = false;
    return true;
}

bool VocalSession::poll()
{
    bool ch = false;
    const int m = recModeV.load();
    if (m == RecDone) { finishRecording(); ch = true; }
    else if (m == RecRecording && stopReq.load() && juce::Time::currentTimeMillis() - stopRequestedAt > 800)
        recModeV.store (RecDone);              // the audio thread never answered (host not processing): take what we have

    std::lock_guard<std::mutex> lk (resMx);
    if (resNewClip)
    {
        docV = resDoc;
        resNewClip = false;
        clipArrived = true;
        hasClipV.store (docV.clipSeconds > 0.0);
        ch = true;
    }
    if (resChanged) { resChanged = false; ch = true; }
    return ch;
}

// =================================================================================================================
// recording
// =================================================================================================================
void VocalSession::startRecording (bool waitForHost)
{
    if (recModeV.load() != RecIdle) return;
    const double sr = hostSr.load();
    recBuf.assign ((size_t) (kMaxRecSeconds * sr), 0.f);
    recData = recBuf.data();
    recCap = (int) recBuf.size();
    recPos = 0;
    recLenV.store (0);
    stopReq.store (false);
    recWaitHost = waitForHost;
    recAnchorPending = ! waitForHost;
    lastHostPlaying = false;
    playing.store (false);
    setStatus (waitForHost ? "Armed - press play in your DAW to start recording" : "Recording...");
    recModeV.store (waitForHost ? RecArmed : RecRecording);
}

void VocalSession::stopRecording()
{
    int exp = RecArmed;
    if (recModeV.compare_exchange_strong (exp, RecIdle))
    {
        recBuf.clear(); recBuf.shrink_to_fit(); recData = nullptr; recCap = 0;
        setStatus ("Recording cancelled");
        return;
    }
    if (recModeV.load() == RecRecording)
    {
        stopRequestedAt = juce::Time::currentTimeMillis();
        stopReq.store (true);
    }
}

void VocalSession::capture (const float* l, const float* r, int n, bool hostPlaying, double hostBeat)
{
    int m = recModeV.load();
    if (m == RecIdle || m == RecDone) { lastHostPlaying = hostPlaying; return; }
    if (m == RecArmed)
    {
        if (! hostPlaying) { lastHostPlaying = false; return; }
        recAnchor = hostBeat;
        recAnchorOut.store (hostBeat);
        recPos = 0;
        recModeV.store (RecRecording);
        m = RecRecording;
    }
    else if (recAnchorPending)
    {
        recAnchor = hostPlaying ? hostBeat : 0.0;
        recAnchorOut.store (recAnchor);
        recAnchorPending = false;
    }

    bool finish = stopReq.load() || (recWaitHost && lastHostPlaying && ! hostPlaying);
    const int k = finish ? 0 : std::min (n, recCap - recPos);
    for (int i = 0; i < k; ++i) recData[recPos + i] = 0.5f * (l[i] + r[i]);
    recPos += k;
    recLenV.store (recPos);
    if (k < n) finish = true;
    lastHostPlaying = hostPlaying;
    if (finish) recModeV.store (RecDone);
}

void VocalSession::finishRecording()
{
    const int len = recLenV.load();
    const double sr = hostSr.load();
    if (len < 4096)
    {
        recModeV.store (RecIdle);
        recBuf.clear(); recBuf.shrink_to_fit(); recData = nullptr; recCap = 0;
        setStatus ("Nothing was recorded");
        return;
    }
    auto src = std::make_shared<std::vector<float>> (recBuf.begin(), recBuf.begin() + len);
    {
        std::lock_guard<std::mutex> lk (mx);
        source = src; sourceSr = sr; flacValid = false;
        pendingA = {}; pendingS = {}; wantAnalyse = true;
    }
    {
        std::lock_guard<std::mutex> lk (resMx);
        resAnchor = recAnchorOut.load(); resAnchorValid = true;
    }
    recBuf.clear(); recBuf.shrink_to_fit(); recData = nullptr; recCap = 0;
    recModeV.store (RecIdle);
    busyV.store (true);
    progressV.store (0.f);
    setStatus ("Finding the notes...");
    wake();
}

// =================================================================================================================
// worker
// =================================================================================================================
void VocalSession::run()
{
    while (! threadShouldExit())
    {
        wait (-1);
        if (threadShouldExit()) break;
        for (;;)
        {
            if (threadShouldExit()) return;
            int job = 0;
            juce::File f;
            {
                std::lock_guard<std::mutex> lk (mx);
                if (wantState)        { job = 1; wantState = false; }
                else if (wantLoad)    { job = 2; wantLoad = false; f = pendingFile; }
                else if (wantAnalyse) { job = 3; wantAnalyse = false; }
                else if (wantRender)  { job = 4; }
            }
            if (job == 0) break;
            cancelAll.store (false);
            if (job == 1) doLoadState();
            else if (job == 2) doLoad (f);
            else if (job == 3) doAnalyse();
            else doRender();
        }
    }
}

void VocalSession::doLoad (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr) { setStatus ("Couldn't read that file (unsupported or damaged)."); busyV.store (false); return; }
    const long long len = reader->lengthInSamples;
    const double sr = reader->sampleRate;
    if (len < 4096 || sr < 8000.0) { setStatus ("That file is too short or has an unusable sample rate."); busyV.store (false); return; }
    if ((double) len > sr * 600.0) { setStatus ("Clips longer than 10 minutes aren't supported here - cut it down first."); busyV.store (false); return; }

    const int ch = (int) std::min (2u, std::max (1u, reader->numChannels));
    juce::AudioBuffer<float> buf (ch, (int) len);
    reader->read (&buf, 0, (int) len, 0, true, ch > 1);
    auto mono = std::make_shared<std::vector<float>> ((size_t) len, 0.f);
    for (int c = 0; c < ch; ++c)
    {
        const float* p = buf.getReadPointer (c);
        for (int i = 0; i < (int) len; ++i) (*mono)[(size_t) i] += p[i] / (float) ch;
    }
    {
        std::lock_guard<std::mutex> lk (mx);
        source = mono; sourceSr = sr; flacValid = false;
        pendingA = {}; pendingS = {}; wantAnalyse = true;
    }
    setStatus ("Finding the notes...");
}

void VocalSession::buildOriginal()
{
    std::shared_ptr<const std::vector<float>> src;
    double sr;
    { std::lock_guard<std::mutex> lk (mx); src = source; sr = sourceSr; }
    if (! src) return;
    const double host = hostSr.load();
    if (std::abs (host - sr) < 0.5) publishBuffer (*src, false);
    else publishBuffer (resampleSinc (*src, sr, host), false);
    origBuiltSr = host;
}

void VocalSession::publishBuffer (const std::vector<float>& mono, bool edited)
{
    auto b = std::make_shared<juce::AudioBuffer<float>> (1, (int) mono.size());
    if (! mono.empty()) std::copy (mono.begin(), mono.end(), b->getWritePointer (0));
    {
        std::lock_guard<std::mutex> lk (resMx);
        (edited ? editedShared : origShared) = b;
        resChanged = true;
    }
    keep[(size_t) keepPos] = b;
    keepPos = (keepPos + 1) % (int) keep.size();
    (edited ? editedPtr : origPtr).store (b.get());
}

juce::MemoryBlock VocalSession::encodeSource() const
{
    std::shared_ptr<const std::vector<float>> src;
    double sr;
    {
        auto* self = const_cast<VocalSession*> (this);
        std::lock_guard<std::mutex> lk (self->mx);
        src = source; sr = sourceSr;
    }
    juce::MemoryBlock mb;
    if (! src || src->empty()) return mb;
    auto* mos = new juce::MemoryOutputStream (mb, false);
    juce::FlacAudioFormat ff;
    std::unique_ptr<juce::AudioFormatWriter> w (ff.createWriterFor (mos, sr, 1, 24, {}, 5));
    if (w == nullptr) { delete mos; return {}; }
    juce::AudioBuffer<float> b (1, (int) src->size());
    std::copy (src->begin(), src->end(), b.getWritePointer (0));
    w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    w.reset();
    return mb;
}

void VocalSession::doAnalyse()
{
    std::shared_ptr<const std::vector<float>> src;
    double sr;
    AnalysisOptions ao; SegmentOptions so;
    {
        std::lock_guard<std::mutex> lk (mx);
        src = source; sr = sourceSr; ao = pendingA; so = pendingS;
    }
    if (! src) { busyV.store (false); return; }
    busyV.store (true);
    setStatus ("Finding the notes...");
    auto track = analysePitch (src->data(), src->size(), sr, ao, &cancelAll, &progressV);
    if (cancelAll.load()) { busyV.store (false); return; }
    auto notes = segmentNotes (track, so);

    VocalDoc d;
    d.sr = sr; d.hop = track.hop; d.clipSeconds = (double) src->size() / sr;
    d.notes = std::move (notes);
    const int count = (int) d.notes.size();
    {
        std::lock_guard<std::mutex> lk (resMx);
        resDoc = d; resNewClip = true; resChanged = true;
    }
    buildOriginal();
    {
        std::lock_guard<std::mutex> lk (mx);
        pendingRender.notes = d.notes; pendingRender.opts = renderOpts; pendingRender.hop = d.hop; pendingRender.sr = sr;
        pendingRender.serial = renderSerial.fetch_add (1) + 1;
        wantRender = true;
        flacValid = false;
    }
    setStatus (count > 0 ? juce::String (count) + " notes found" : "No pitched notes found - is this a sung or spoken vocal?");
    busyV.store (false);
    progressV.store (1.f);
}

void VocalSession::doRender()
{
    RenderJob j;
    std::shared_ptr<const std::vector<float>> src;
    {
        std::lock_guard<std::mutex> lk (mx);
        j = std::move (pendingRender);
        wantRender = false;
        src = source;
    }
    cancelRender.store (false);
    if (! src) { renderDone.store (j.serial); return; }
    if (std::abs (origBuiltSr - hostSr.load()) > 0.5) buildOriginal();

    bool anyEdit = false;
    for (const auto& n : j.notes) if (n.edited()) { anyEdit = true; break; }
    std::vector<float> out;
    if (! anyEdit) out = *src;
    else
    {
        out = renderVocal (*src, j.sr, j.hop, j.notes, j.opts, &cancelRender);
        if (out.empty()) return;                         // superseded by a newer request
    }
    const double host = hostSr.load();
    if (std::abs (host - j.sr) > 0.5) out = resampleSinc (out, j.sr, host);
    publishBuffer (out, true);
    renderDone.store (j.serial);
}

void VocalSession::doLoadState()
{
    struct Done { std::atomic<bool>& b; ~Done() { b.store (false); } } done { busyV };
    juce::MemoryBlock blk;
    { std::lock_guard<std::mutex> lk (mx); blk = pendingState; }
    juce::MemoryInputStream in (blk, false);
    if (in.readInt() != 0x53444556 || in.readInt() != 1) return;
    const double sr = in.readDouble();
    const juce::int64 flacSize = in.readInt64();
    if (flacSize <= 0 || flacSize > (juce::int64) blk.getSize()) return;
    juce::MemoryBlock flac;
    in.readIntoMemoryBlock (flac, (ssize_t) flacSize);
    const juce::int64 noteSize = in.readInt64();
    std::vector<uint8_t> nb;
    if (noteSize > 0 && noteSize <= (juce::int64) blk.getSize())
    {
        nb.resize ((size_t) noteSize);
        in.read (nb.data(), (int) noteSize);
    }

    juce::AudioFormatManager local;
    local.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (local.createReaderFor (std::make_unique<juce::MemoryInputStream> (flac.getData(), flac.getSize(), true)));
    if (reader == nullptr) return;
    const int len = (int) reader->lengthInSamples;
    juce::AudioBuffer<float> buf (1, len);
    reader->read (&buf, 0, len, 0, true, true);
    auto mono = std::make_shared<std::vector<float>> (buf.getReadPointer (0), buf.getReadPointer (0) + len);

    std::vector<VocalNote> notes;
    double nsr = sr; int hop = 256;
    const bool haveNotes = ! nb.empty() && deserialiseNotes (nb.data(), nb.size(), notes, nsr, hop);
    {
        std::lock_guard<std::mutex> lk (mx);
        source = mono; sourceSr = sr; flacCache = flac; flacValid = true;
    }
    if (! haveNotes)
    {
        { std::lock_guard<std::mutex> lk (mx); pendingA = {}; pendingS = {}; wantAnalyse = true; }
        return;
    }
    VocalDoc d;
    d.sr = sr; d.hop = hop; d.clipSeconds = (double) len / sr; d.notes = notes;
    {
        std::lock_guard<std::mutex> lk (resMx);
        resDoc = d; resNewClip = true; resChanged = true;
    }
    buildOriginal();
    {
        std::lock_guard<std::mutex> lk (mx);
        pendingRender.notes = notes; pendingRender.opts = renderOpts; pendingRender.hop = hop; pendingRender.sr = sr;
        pendingRender.serial = renderSerial.fetch_add (1) + 1;
        wantRender = true;
    }
    setStatus (juce::String ((int) notes.size()) + " notes restored");
}

// =================================================================================================================
// persistence / export
// =================================================================================================================
juce::MemoryBlock VocalSession::saveState()
{
    if (! hasClipV.load()) return {};
    juce::MemoryBlock flac;
    {
        std::lock_guard<std::mutex> lk (mx);
        if (flacValid) flac = flacCache;
    }
    if (flac.isEmpty())
    {
        flac = encodeSource();
        std::lock_guard<std::mutex> lk (mx);
        flacCache = flac; flacValid = ! flac.isEmpty();
    }
    if (flac.isEmpty()) return {};
    const auto nb = serialiseNotes (docV.notes, docV.sr, docV.hop);
    juce::MemoryOutputStream o;
    o.writeInt (0x53444556);
    o.writeInt (1);
    o.writeDouble (docV.sr);
    o.writeInt64 ((juce::int64) flac.getSize());
    o.write (flac.getData(), flac.getSize());
    o.writeInt64 ((juce::int64) nb.size());
    o.write (nb.data(), nb.size());
    return o.getMemoryBlock();
}

void VocalSession::loadState (const void* data, size_t size)
{
    if (data == nullptr || size < 32) return;
    {
        std::lock_guard<std::mutex> lk (mx);
        pendingState = juce::MemoryBlock (data, size);
        wantState = true;
    }
    busyV.store (true);
    wake();
}

juce::Result VocalSession::exportWav (const juce::File& out, int bitDepth) const
{
    std::shared_ptr<juce::AudioBuffer<float>> b;
    {
        auto* self = const_cast<VocalSession*> (this);
        std::lock_guard<std::mutex> lk (self->resMx);
        b = editedShared;
    }
    if (! b || b->getNumSamples() < 16) return juce::Result::fail ("There is no edited clip to export yet.");
    out.deleteFile();
    auto stream = std::make_unique<juce::FileOutputStream> (out);
    if (! stream->openedOk()) return juce::Result::fail ("Couldn't write to " + out.getFullPathName());
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), hostSr.load(), 1, bitDepth, {}, 0));
    if (w == nullptr) return juce::Result::fail ("Couldn't create the WAV writer.");
    stream.release();
    w->writeFromAudioSampleBuffer (*b, 0, b->getNumSamples());
    return juce::Result::ok();
}

juce::File VocalSession::dragFile() const
{
    auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("AIO Vocal Edit.wav");
    return exportWav (f, 24).wasOk() ? f : juce::File();
}

void VocalSession::auditionRange (double s0, double s1)
{
    const double sr = hostSr.load();
    if (s1 <= s0) return;
    stopAt.store ((long long) std::llround (s1 * sr));
    playPos.store ((long long) std::llround (std::max (0.0, s0) * sr));
    playing.store (true);
}

// =================================================================================================================
// audio thread
// =================================================================================================================
bool VocalSession::process (float* l, float* r, int n, bool hostPlaying, double hostBeat, double bpm, const PlayParams& pp)
{
    if (recModeV.load (std::memory_order_relaxed) != RecIdle) return false;
    const juce::AudioBuffer<float>* buf = pp.original ? origPtr.load() : editedPtr.load();
    if (buf == nullptr) return false;
    const bool useHost = pp.follow && hostPlaying;
    const bool internal = playing.load();
    if (! useHost && ! internal) return false;

    const long long len = buf->getNumSamples();
    const float* s = buf->getReadPointer (0);
    const double sr = hostSr.load();

    if (useHost)
    {
        const long long pos = (long long) std::llround ((hostBeat - pp.anchorBeat) * 60.0 / std::max (20.0, bpm) * sr);
        for (int i = 0; i < n; ++i)
        {
            const long long p = pos + i;
            l[i] = r[i] = (p >= 0 && p < len) ? s[p] : 0.f;
        }
        playheadSec.store ((double) pos / sr);
        return true;
    }

    long long p = playPos.load();
    const long long sa = stopAt.load();
    playheadSec.store ((double) p / sr);
    bool stopped = false;
    for (int i = 0; i < n; ++i)
    {
        if (! stopped)
        {
            if (p >= len)
            {
                if (pp.loop && sa < 0 && len > 0) p = 0;
                else { stopped = true; playing.store (false); stopAt.store (-1); }
            }
            else if (sa >= 0 && p >= sa) { stopped = true; playing.store (false); stopAt.store (-1); }
        }
        l[i] = r[i] = stopped ? 0.f : s[p++];
    }
    playPos.store (stopped ? std::min (p, len) : p);
    return true;
}
} // namespace aio
