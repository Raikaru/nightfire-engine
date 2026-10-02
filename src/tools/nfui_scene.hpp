#pragma once

#include <memory>
#include <string>
#include <vector>

#include "assets/game_files.hpp"
#include "assets/ui_assets.hpp"
#include "game/input.hpp"
#include "ui/renderer.hpp"
#include "ui/text.hpp"

namespace nf {

// A previewable piece of UI for `nfui <gamedir> <mode>`. `update` runs once per 30 Hz logic frame
// with the pad sample of that frame; `draw` renders one 640x448 frame.
class Scene {
public:
    virtual ~Scene() = default;
    virtual void update(const PadHistory& pad) = 0;
    virtual void draw(ui::Renderer& renderer, ui::TextRenderer& text) = 0;
    virtual bool finished() const { return false; }  // interactive scenes may end (movie playback)
};

struct SceneArgs {
    GameFiles& files;
    UiAssets& assets;        // scenes may add level sprites to assets.sprites
    std::string gamedir;
    std::vector<std::string> extra;  // mode-specific command line arguments (everything nfui does not consume)
};

// One factory per mode, defined in src/tools/nfui_<mode>.cpp.
std::unique_ptr<Scene> make_font_scene(SceneArgs& args);
std::unique_ptr<Scene> make_menu_scene(SceneArgs& args);
std::unique_ptr<Scene> make_hud_scene(SceneArgs& args);
std::unique_ptr<Scene> make_movie_scene(SceneArgs& args);

// Headless text modes (no window, dispatched by nfui.cpp before the window is created); return the exit code.
int run_mp_text(SceneArgs& args);
int run_movie_stats(const std::string& gamedir, const std::vector<std::string>& extra);
int run_import_save(SceneArgs& args);

}  // namespace nf
