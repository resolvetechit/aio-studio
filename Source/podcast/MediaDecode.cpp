#include "MediaDecode.h"
#include <algorithm>
#include <vector>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX 1
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN 1
 #endif
 #include <windows.h>
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #include <mferror.h>
 #if JUCE_MSVC
  #pragma comment (lib, "mfplat.lib")
  #pragma comment (lib, "mfreadwrite.lib")
  #pragma comment (lib, "mfuuid.lib")
  #pragma comment (lib, "ole32.lib")
 #endif
#endif

namespace aio
{
namespace media
{
static bool forceAll = false;
void forceDecodeForTests (bool on) { forceAll = on; }

bool needsDecode (const juce::File& f)
{
    if (forceAll) return true;
    static const char* const exts[] = { "mp4", "m4v", "mov", "mkv", "webm", "avi", "mxf", "mts", "m2ts", "mpg", "mpeg", "3gp", "wmv", "asf", "m4a", "aac", "mka", "wma", nullptr };
    const auto e = f.getFileExtension().trimCharactersAtStart (".").toLowerCase();
    for (int i = 0; exts[i] != nullptr; ++i) if (e == exts[i]) return true;
    return false;
}

juce::String chooserPattern()
{
    return "*.wav;*.mp3;*.flac;*.aif;*.aiff;*.ogg;*.mp4;*.mov;*.m4v;*.mkv;*.avi;*.mxf;*.mts;*.m2ts;*.3gp;*.webm;*.wmv;*.m4a;*.aac;*.wma";
}

namespace
{
// copies any reader JUCE can make into a 24-bit WAV
bool copyReaderToWav (juce::AudioFormatReader& rd, const juce::File& outWav, const std::function<bool (float)>& prog, juce::String& error)
{
    const int nch = rd.numChannels >= 2 ? 2 : 1;
    outWav.deleteFile();
    auto stream = outWav.createOutputStream();
    if (stream == nullptr) { error = "Couldn't write a temporary file (is the disk full?)."; return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), rd.sampleRate, (unsigned) nch, 24, {}, 0));
    if (w == nullptr) { error = "Couldn't create a temporary file."; return false; }
    stream.release();
    constexpr int C = 1 << 15;
    juce::AudioBuffer<float> buf (2, C);
    for (long long pos = 0; pos < rd.lengthInSamples; pos += C)
    {
        if (prog && ! prog ((float) pos / (float) std::max<long long> (1, rd.lengthInSamples))) return false;
        const int n = (int) std::min<long long> (C, rd.lengthInSamples - pos);
        buf.clear();
        if (! rd.read (&buf, 0, n, pos, true, true)) { error = "The file stopped being readable part-way through."; return false; }
        const float* ch[2] = { buf.getReadPointer (0), buf.getReadPointer (rd.numChannels >= 2 ? 1 : 0) };
        if (! w->writeFromFloatArrays (ch, nch, n)) { error = "Couldn't write the temporary file (is the disk full?)."; return false; }
    }
    return true;
}

#if JUCE_WINDOWS
template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr() { if (p != nullptr) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool decodeWithMediaFoundation (const juce::File& in, const juce::File& outWav, const std::function<bool (float)>& prog, juce::String& error)
{
    const HRESULT coHr = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED (coHr);
    struct Cleanup { bool co, mf; ~Cleanup() { if (mf) MFShutdown(); if (co) CoUninitialize(); } } cleanup { coOwned, false };
    if (FAILED (MFStartup (MF_VERSION, MFSTARTUP_LITE))) { error = "Windows couldn't start its media decoder."; return false; }
    cleanup.mf = true;

    ComPtr<IMFSourceReader> reader;
    if (FAILED (MFCreateSourceReaderFromURL (in.getFullPathName().toWideCharPointer(), nullptr, &reader)) || ! reader)
    {
        error = "Windows can't open that file. If it is an unusual video format, export its audio from your editor as a WAV instead.";
        return false;
    }
    const DWORD kAudio = (DWORD) MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (FAILED (reader->SetStreamSelection (kAudio, TRUE))) { error = "That file has no audio track."; return false; }

    {
        ComPtr<IMFMediaType> want;
        if (FAILED (MFCreateMediaType (&want)) || ! want) { error = "Windows couldn't set up the audio decoder."; return false; }
        want->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        want->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_Float);
        if (FAILED (reader->SetCurrentMediaType (kAudio, nullptr, want.p))) { error = "Windows can't decode the audio in that file."; return false; }
    }
    UINT32 channels = 0, rate = 0;
    {
        ComPtr<IMFMediaType> got;
        if (FAILED (reader->GetCurrentMediaType (kAudio, &got)) || ! got) { error = "Windows couldn't describe the audio in that file."; return false; }
        got->GetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, &channels);
        got->GetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    }
    if (channels < 1 || rate < 8000) { error = "The audio in that file has an unusable format."; return false; }

    double durationSec = 0.0;
    {
        PROPVARIANT pv; PropVariantInit (&pv);
        if (SUCCEEDED (reader->GetPresentationAttribute ((DWORD) MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &pv)) && pv.vt == VT_UI8)
            durationSec = (double) pv.uhVal.QuadPart / 1.0e7;
        PropVariantClear (&pv);
    }

    const int nch = channels >= 2 ? 2 : 1;
    outWav.deleteFile();
    auto stream = outWav.createOutputStream();
    if (stream == nullptr) { error = "Couldn't write a temporary file (is the disk full?)."; return false; }
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), (double) rate, (unsigned) nch, 24, {}, 0));
    if (w == nullptr) { error = "Couldn't create a temporary file."; return false; }
    stream.release();

    std::vector<float> a, b;
    long long frames = 0;
    for (;;)
    {
        DWORD streamIndex = 0, flags = 0; LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        const HRESULT hr = reader->ReadSample (kAudio, 0, &streamIndex, &flags, &ts, &sample);
        if (FAILED (hr)) { error = "Windows stopped decoding part-way through that file."; return false; }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) break;
        if (! sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED (sample->ConvertToContiguousBuffer (&buf)) || ! buf) continue;
        BYTE* data = nullptr; DWORD cur = 0;
        if (FAILED (buf->Lock (&data, nullptr, &cur))) continue;
        const int n = (int) (cur / (sizeof (float) * channels));
        a.resize ((size_t) n); b.resize ((size_t) n);
        const float* f = reinterpret_cast<const float*> (data);
        for (int i = 0; i < n; ++i) { a[(size_t) i] = f[(size_t) i * channels]; b[(size_t) i] = channels >= 2 ? f[(size_t) i * channels + 1] : a[(size_t) i]; }
        buf->Unlock();
        const float* ch[2] = { a.data(), b.data() };
        if (n > 0 && ! w->writeFromFloatArrays (ch, nch, n)) { error = "Couldn't write the temporary file (is the disk full?)."; return false; }
        frames += n;
        if (prog && ! prog (durationSec > 0.0 ? (float) std::min (1.0, ((double) frames / (double) rate) / durationSec) : 0.f)) return false;
    }
    if (frames < 1024) { error = "No audio could be read from that file."; return false; }
    return true;
}
#endif
} // namespace

bool decodeToWav (const juce::File& in, const juce::File& outWav, juce::AudioFormatManager& formats,
                  const std::function<bool (float)>& prog, juce::String& error)
{
    error.clear();
    if (! in.existsAsFile()) { error = "That file doesn't exist any more."; return false; }

    // 1. anything JUCE itself can read (macOS Core Audio covers MP4 / MOV / M4A; everything covers OGG, FLAC, MP3...)
    if (std::unique_ptr<juce::AudioFormatReader> rd { formats.createReaderFor (in) })
    {
        if (rd->lengthInSamples < 1024) { error = "No audio could be read from that file."; return false; }
        return copyReaderToWav (*rd, outWav, prog, error);
    }

    // 2. Windows: Media Foundation reads MP4 / MOV / M4V / M4A / AAC / AVI / WMV...
   #if JUCE_WINDOWS
    return decodeWithMediaFoundation (in, outWav, prog, error);
   #else
    error = "This computer can't read audio from that kind of file. Export the audio from your editor as a WAV and load that.";
    return false;
   #endif
}
} // namespace media
} // namespace aio
