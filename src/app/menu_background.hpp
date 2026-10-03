#pragma once

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "media/movie_player.hpp"
#include "ui/renderer.hpp"

#include "ui/layout.hpp"
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
        if (frame_.rgba.empty()) return;
        const ui::Layout layout = ui::Layout::from_canvas_width(renderer.canvas_width());
        float w = layout.width(), h = layout.height();
        const float aspect = float(frame_.width) / float(std::max(frame_.height, 1)) * ui::kPssPixelAspect;
        if (layout.width() * ui::kScreenXScale / layout.height() > aspect)
            h = w * ui::kScreenXScale / aspect;
        else
            w = h * aspect / ui::kScreenXScale;
        renderer.draw_frame({ui::Layout::kDesignWidth * 0.5f - w * 0.5f,
                             ui::Layout::kDesignHeight * 0.5f - h * 0.5f, w, h},
                            frame_.width, frame_.height, frame_.rgba.data());
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
