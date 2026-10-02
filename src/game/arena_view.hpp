#pragma once

#include <vector>

namespace nf {

// Viewport of one local player in window pixels (origin top left).
struct ViewRect {
    int x = 0, y = 0, w = 0, h = 0;
    float aspect() const { return h > 0 ? float(w) / float(h) : 1.0f; }
};

// Camera_CreateCameras: the viewer rectangles of the original for `players` local players on its 512x448 frame,
// scaled to `width` x `height`. One player owns the screen; two split it into halves (`side_by_side`, DrawInfo == 1:
// 256x448 columns, otherwise 512x224 rows, the default); three or four use 256x224 quadrants (the fourth stays empty
// with three players). Each viewer then projects with a vertical field of view of 60 degrees (Camera_CalcViewAngles
// 1.0471976) and the aspect of its own rectangle.
inline std::vector<ViewRect> split_screen_layout(int players, int width, int height, bool side_by_side = false) {
    auto scaled = [&](float x0, float y0, float x1, float y1) {
        ViewRect r;
        r.x = int(x0 * float(width) / 512.0f + 0.5f);
        r.y = int(y0 * float(height) / 448.0f + 0.5f);
        r.w = int(x1 * float(width) / 512.0f + 0.5f) - r.x;
        r.h = int(y1 * float(height) / 448.0f + 0.5f) - r.y;
        return r;
    };
    std::vector<ViewRect> out;
    if (players <= 1) {
        out.push_back(scaled(0, 0, 512, 448));
    } else if (players == 2) {
        if (side_by_side) {
            out.push_back(scaled(0, 0, 256, 448));
            out.push_back(scaled(256, 0, 512, 448));
        } else {
            out.push_back(scaled(0, 0, 512, 224));
            out.push_back(scaled(0, 224, 512, 448));
        }
    } else {
        out.push_back(scaled(0, 0, 256, 224));
        out.push_back(scaled(256, 0, 512, 224));
        out.push_back(scaled(0, 224, 256, 448));
        if (players >= 4) out.push_back(scaled(256, 224, 512, 448));
    }
    return out;
}

constexpr float kViewFovY = 1.0471976f;   // Camera_CalcViewAngles(viewer, 1.0471976)

}  // namespace nf
