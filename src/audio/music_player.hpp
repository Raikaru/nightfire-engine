#pragma once

#include <cstdint>
#include <memory>

#include "assets/music.hpp"

namespace nf::audio {

// Interactive music: walks a track's marker map the way the IOP's stream reader does
// (UpdateMusicMarkers, SFXCheckStreamJumpInstant, SFXJumpToMusicMarkerUpdate in SFX.IRX).
//
// Playback is a position in the stereo ADPCM data that advances in 256-byte blocks. Each block boundary
// first resolves the current marker once the position reaches it (StreamAlign'd to 256 bytes):
//   End, EndAndFinish  playback stops
//   any marker but LoopBackLocked, with a jump pending: continue at the requested section
//   LoopBack, LoopBackLocked: continue at the marker's loop_start, its loop_marker becomes current
//   otherwise the next marker becomes current
// A jump request to a section whose `instant` flag is set (or made with `instant`) is taken at the next
// block instead of waiting for a marker.
class MusicPlayer {
public:
    explicit MusicPlayer(std::shared_ptr<const MusicTrack> track);

    // SFXStartMusic: begin at `section`.
    void start(std::uint32_t section);
    // SFXJumpToMusicMarker: requests to the section already playing or already requested are ignored;
    // a non-instant request is ignored until the first marker has been passed.
    void jump(std::uint32_t section, bool instant);

    bool finished() const { return finished_; }
    std::uint32_t now_playing() const { return section_; }                 // section the position is in (changes as markers flow on)
    std::int64_t last_jump() const { return last_jump_; }                   // last completed jump target = EE MusicLastJump (-1 = none)
    std::int64_t pending_jump() const { return pending_; }
    std::size_t position() const { return pos_; }
    const MusicTrack& track() const { return *track_; }

    // Decodes the next kStereoBlockFrames stereo frames (interleaved L,R at 32 kHz). Returns false and
    // writes silence once the track has finished.
    bool next_block(std::int16_t* out);

private:
    void take_jump(std::uint32_t section);
    void resolve_markers();

    std::shared_ptr<const MusicTrack> track_;
    std::size_t pos_ = 0;
    std::size_t marker_ = 0;
    std::uint32_t section_ = 0;
    std::int64_t last_jump_ = -1;
    std::int64_t pending_ = -1;
    bool first_marker_passed_ = false;
    bool finished_ = true;
    AdpcmState left_, right_;
};

}  // namespace nf::audio
