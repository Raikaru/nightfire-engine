#pragma once

#include <cstdint>
#include <vector>

#include "assets/reader.hpp"
#include "assets/spu_adpcm.hpp"

namespace nf {

// Marker types (`MusicMarkerData::MType`), as consumed by UpdateMusicMarkers in SFX.IRX. Positions are
// byte offsets into the stereo `.SSD` data, read 256 bytes at a time (StreamAlign).
enum class MarkerType : std::uint32_t {
    Plain = 0,         // beat/bar marker: a pending jump request is taken here
    EndAndFinish = 5,  // stop; stream reports finished immediately
    LoopBack = 6,      // jump requests are taken; otherwise continue at loop_start / loop_marker
    LoopBackLocked = 7,// like LoopBack but jump requests are ignored (section must play out)
    End = 9,           // stop after this position
    SectionStart = 10, // first marker of a section: a pending jump request is taken here too
};

// MusicMarkerData (32 bytes).
struct Marker {
    std::uint32_t section;      // index of the section this marker belongs to
    std::uint32_t pos;          // byte position in the audio data
    MarkerType type;
    std::uint32_t flags;        // always 2
    std::uint32_t extra;        // always 0
    std::uint32_t loop_start;   // LoopBack*: position playback continues from
    std::uint32_t index;        // sound tool marker number (informational)
    std::uint32_t loop_marker;  // LoopBack*: marker that becomes current after the jump
};

// MusicMarkerStartData (52 bytes): a jump target ("section").
struct Section {
    Marker marker;                 // the section's own marker record (pos = where playback starts)
    std::uint32_t marker_index;    // marker that is current when playback starts here
    bool instant;                  // jump requests to this section do not wait for a marker
};

// StreamMarkerHeaderData (20 bytes) + start markers + markers. The same header describes an SMF music
// file and the 0x88-byte header in front of every STREAMS.BIN entry.
struct MarkerMap {
    std::uint32_t base_volume = 0;  // 0..100
    std::vector<Section> sections;
    std::vector<Marker> markers;
};

// `data_size` is the size of the audio the positions refer to; every position and index is checked.
MarkerMap parse_marker_map(Bytes header, std::size_t data_size);

// Seconds of stereo audio at the music sample rate for `bytes` of interleaved data.
constexpr std::uint32_t kMusicSampleRate = 32000;
constexpr std::uint32_t kStreamSampleRate = 22050;
inline double music_seconds(std::size_t bytes) {
    return double(bytes / kStreamBlockBytes) * kStereoBlockFrames / kMusicSampleRate;
}
inline double stream_seconds(std::size_t bytes) {
    return double(bytes / kAdpcmFrameBytes) * kAdpcmFrameSamples / kStreamSampleRate;
}

// MFX_n.SMF + MFX_n.SSD.
struct MusicTrack {
    int number;                         // file number n
    MarkerMap map;
    std::vector<std::uint8_t> audio;    // .SSD, 2-channel interleaved ADPCM
    double seconds() const { return music_seconds(audio.size()); }
};

// One STREAMS.BIN entry (speech and long effects): mono 22050 Hz ADPCM with a marker header.
struct StreamClip {
    std::uint32_t index;
    MarkerMap map;
    std::vector<std::uint8_t> audio;
    double seconds() const { return stream_seconds(audio.size()); }
};

}  // namespace nf
