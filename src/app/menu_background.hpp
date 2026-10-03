#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "media/movie_player.hpp"
#include "ui/renderer.hpp"

namespace nf::app {

// The front-end pages use the looping 0x7350048 PSS as a full-screen background movie.
class MenuBackground {
public:
    explicit MenuBackground(const std::filesystem::path& gamedir)
        : path_((gamedir / "MOVIES" / "30_FPS" / "07350048.PSS").string()), player_(std::make_unique<media::MoviePlayer>()) {}

    void advance() {
        if (!opened_) {
            open();
        } else if (!player_->next_video(frame_)) {
            player_ = std::make_unique<media::MoviePlayer>();
            open();
        }
    }

    void draw(ui::Renderer& renderer) const {
        if (!frame_.rgba.empty())
            renderer.draw_frame({0, 0, ui::kScreenW, ui::kScreenH}, frame_.width, frame_.height, frame_.rgba.data());
    }

private:
    void open() {
        if (!player_->open(path_)) throw std::runtime_error("cannot open menu background movie " + path_ + ": " + player_->error());
        if (!player_->next_video(frame_)) throw std::runtime_error("menu background movie has no video frame: " + path_);
        opened_ = true;
    }

    std::string path_;
    std::unique_ptr<media::MoviePlayer> player_;
    media::MovieFrame frame_;
    bool opened_ = false;
};

}  // namespace nf::app
