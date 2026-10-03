#pragma once

#include <algorithm>

namespace nf::ui {

// UI coordinates retain the game's 640x448 authored space at 4:3. At other
// aspects the canvas expands horizontally; the original 4:3 composition remains
// centred and elements can be moved relative to natural screen anchors.
enum class HorizontalAnchor { Left, Center, Right };
enum class VerticalAnchor { Top, Center, Bottom };

class Layout {
public:
    static constexpr float kDesignWidth = 640.0f;
    static constexpr float kDesignHeight = 448.0f;

    Layout(float window_w, float window_h, float safe_margin = 24.0f)
        : width_(kDesignHeight * std::max(window_w, 1.0f) / std::max(window_h, 1.0f) *
                 (kDesignWidth / (kDesignHeight * 4.0f / 3.0f))),
          safe_margin_(std::clamp(safe_margin, 0.0f, kDesignWidth * 0.5f)) {}

    float width() const { return width_; }
    constexpr float height() const { return kDesignHeight; }
    static Layout from_canvas_width(float logical_width, float safe_margin = 24.0f) {
        Layout layout(1.0f, 1.0f, safe_margin);
        layout.width_ = std::max(logical_width, kDesignWidth);
        return layout;
    }
    float safe_left() const { return safe_margin_ - (width_ - kDesignWidth) * 0.5f; }
    float safe_right() const { return kDesignWidth + (width_ - kDesignWidth) * 0.5f - safe_margin_; }
    float safe_top() const { return safe_margin_; }
    float safe_bottom() const { return kDesignHeight - safe_margin_; }

    float x(float design_x, HorizontalAnchor anchor = HorizontalAnchor::Center) const {
        const float extra = width_ - kDesignWidth;
        switch (anchor) {
            case HorizontalAnchor::Left: return design_x - extra * 0.5f;
            case HorizontalAnchor::Right: return design_x + extra * 0.5f;
            case HorizontalAnchor::Center: return design_x;
        }
        return design_x;
    }

    constexpr float y(float design_offset, VerticalAnchor anchor = VerticalAnchor::Center) const {
        switch (anchor) {
            case VerticalAnchor::Top: return design_offset;
            case VerticalAnchor::Bottom: return kDesignHeight - design_offset;
            case VerticalAnchor::Center: return kDesignHeight * 0.5f + design_offset;
        }
        return design_offset;
    }

private:
    float width_;
    float safe_margin_;
};

}  // namespace nf::ui
