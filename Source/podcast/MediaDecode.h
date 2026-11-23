#pragma once
#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>

namespace aio
{
// Audio from video files and other containers: MP4, MOV, M4V, MKV, AVI, M4A, AAC, WMV...
//
// The Podcast Ready player and the processing want to jump around in a file, which compressed / video containers don't allow
// reliably, so these files are first decoded (sequentially, once) into a temporary 24-bit WAV and everything else works on that.
//   Windows: the system's Media Foundation decoders (the same ones Windows Media Player and the Photos app use).
//   macOS:   Core Audio (through JUCE).   Anything JUCE can read itself (WAV, FLAC, OGG, MP3...) is read directly.
namespace media
{
// true when the file should be decoded to a temporary WAV first (by its extension)
bool needsDecode (const juce::File& f);
void forceDecodeForTests (bool on);          // tests only: makes needsDecode() say yes for every file

// The extensions the file chooser offers.
juce::String chooserPattern();

// Decodes the first audio track to a 24-bit WAV (mono or stereo; more channels are reduced to the front pair).
// prog gets 0..1 (when the length is known) and returns false to cancel. On failure `error` says why, in words for the user.
bool decodeToWav (const juce::File& in, const juce::File& outWav, juce::AudioFormatManager& formats,
                  const std::function<bool (float)>& prog, juce::String& error);
} // namespace media
} // namespace aio
