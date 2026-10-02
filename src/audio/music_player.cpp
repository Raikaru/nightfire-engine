#include "audio/music_player.hpp"

#include <algorithm>
#include <cstring>

namespace nf::audio {

namespace {
std::size_t align_block(std::size_t v) { return v / nf::kStreamBlockBytes * nf::kStreamBlockBytes; }
}  // namespace

MusicPlayer::MusicPlayer(std::shared_ptr<const MusicTrack> track) : track_(std::move(track)) {}

void MusicPlayer::start(std::uint32_t section) {
    const auto& map = track_->map;
    if (section >= map.sections.size()) throw FormatError("music section out of range");
    left_ = right_ = {};
    take_jump(section);
    last_jump_ = section;
    pending_ = -1;
    first_marker_passed_ = false;
    finished_ = false;
}

void MusicPlayer::take_jump(std::uint32_t section) {
    const Section& s = track_->map.sections[section];
    pos_ = align_block(s.marker.pos);
    marker_ = s.marker_index;
    section_ = section;
    last_jump_ = section;
}

void MusicPlayer::jump(std::uint32_t section, bool instant) {
    if (finished_ || section >= track_->map.sections.size()) return;
    if (static_cast<std::int64_t>(section) == last_jump_ || section == section_ ||
        static_cast<std::int64_t>(section) == pending_)
        return;
    const bool now = instant || track_->map.sections[section].instant;
    if (!now && !first_marker_passed_) return;
    if (now) {
        take_jump(section);
        pending_ = -1;
    } else {
        pending_ = section;
    }
}

void MusicPlayer::resolve_markers() {
    const auto& markers = track_->map.markers;
    // Every iteration consumes a marker or redirects to a later one; the bound only guards corrupt data.
    for (std::size_t guard = 0; guard < 2 * markers.size() + 8; ++guard) {
        if (marker_ >= markers.size()) { finished_ = true; return; }
        const Marker& m = markers[marker_];
        if (align_block(m.pos) > pos_) return;
        first_marker_passed_ = true;
        if (m.section != 0xFFFFFFFFu) section_ = m.section;
        if (m.type == MarkerType::End || m.type == MarkerType::EndAndFinish) { finished_ = true; return; }
        if (pending_ >= 0 && m.type != MarkerType::LoopBackLocked) {
            take_jump(static_cast<std::uint32_t>(pending_));
            pending_ = -1;
        } else if (m.type == MarkerType::LoopBack || m.type == MarkerType::LoopBackLocked) {
            pos_ = align_block(m.loop_start);
            marker_ = m.loop_marker;
            section_ = markers[marker_].section;
        } else {
            ++marker_;
        }
    }
    finished_ = true;
}

bool MusicPlayer::next_block(std::int16_t* out) {
    if (!finished_) resolve_markers();
    if (!finished_ && pos_ + kStreamBlockBytes > track_->audio.size()) finished_ = true;
    if (finished_) {
        std::memset(out, 0, kStereoBlockFrames * 2 * sizeof(std::int16_t));
        return false;
    }
    decode_stereo_block(track_->audio.data() + pos_, left_, right_, out);
    pos_ += kStreamBlockBytes;
    return true;
}

}  // namespace nf::audio
