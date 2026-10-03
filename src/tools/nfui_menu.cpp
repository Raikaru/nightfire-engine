// nfui menu: the front end (or a level's pause menu) on the original menu script.
//   nfui <gamedir> menu [--page 0x40000002] [--pause <level.bin>] [--trace] [--shot out.bmp] [--press up,cross,...]
// --trace prints every page change with the frame it happened on; --dump lists the parsed script.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>

#include <SDL3/SDL.h>
#include "app/menu_background.hpp"
#include "assets/menu_validate.hpp"
#include "assets/mp_data.hpp"
#include "assets/sp_menu.hpp"
#include "assets/tweak_data.hpp"
#include "assets/credit_data.hpp"
#include "media/movie_player.hpp"
#include "tools/nfui_scene.hpp"
#include "ui/frontend.hpp"

namespace nf {

namespace {

class MenuScene : public Scene {
public:
    explicit MenuScene(SceneArgs& args) : gamedir_(args.gamedir) {
        std::optional<std::uint32_t> page;
        std::string pause_bin;
        for (std::size_t i = 0; i < args.extra.size(); ++i) {
            const std::string& a = args.extra[i];
            if (a == "--page" && i + 1 < args.extra.size()) page = std::uint32_t(std::stoul(args.extra[++i], nullptr, 0));
            else if (a == "--pause" && i + 1 < args.extra.size()) pause_bin = args.extra[++i];
            else if (a == "--trace") trace_ = true;
            else if (a == "--dump") dump_ = true;
            else throw std::runtime_error("menu: unknown option " + a);
        }
        const std::string bin_name = pause_bin.empty() ? std::string(kFrontEndBin) : pause_bin;
        const GameFile* f = args.files.find(bin_name);
        if (!f) throw std::runtime_error("menu: FILES.BIN has no " + bin_name);
        auto data = args.files.read(*f);
        menu_ = load_menu_from_bin(Bytes(data));
        if (!pause_bin.empty()) add_level_sprites(args.assets.sprites, Bytes(data));
        if (dump_) dump(menu_);
        mp_data_ = std::make_unique<MpData>(load_mp_data(args.files, args.gamedir, args.assets.strings));
        sp_data_ = std::make_unique<SpMenuData>(load_sp_menu(Elf32(read_file(std::filesystem::path(args.gamedir) / "ACTION.ELF"))));
        tweak_data_ =
            std::make_unique<TweakData>(load_tweak_data(Elf32(read_file(std::filesystem::path(args.gamedir) / "ACTION.ELF"))));
        frontend_ = std::make_unique<Frontend>(args.assets, menu_, mp_data_.get(), sp_data_.get(), tweak_data_.get());
        frontend_->set_credits(
            load_credits(Elf32(read_file(std::filesystem::path(args.gamedir) / "ACTION.ELF")), args.assets.strings));
        if (pause_bin.empty()) {
            background_ = std::make_unique<app::MenuBackground>(args.gamedir);
            background_->advance();
        }
        frontend_->open(pause_bin.empty() ? FrontendMode::MainMenu : FrontendMode::Pause, page);
    }

    ~MenuScene() override {
        if (movie_audio_) SDL_DestroyAudioStream(movie_audio_);
    }

    void update(const PadHistory& pad) override {
        if (background_) background_->advance();
        frontend_->update(pad);
        if (trace_ && frontend_->page_id() != last_page_) {
            last_page_ = frontend_->page_id();
            std::printf("frame %u: page 0x%08x\n", frame_, last_page_);
        }
        if (trace_ && frontend_->manager() && frontend_->manager()->input_locked() != last_lock_) {
            last_lock_ = frontend_->manager()->input_locked();
            std::printf("frame %u: input %s\n", frame_, last_lock_ ? "locked" : "unlocked");
        }
        ++frame_;
        update_movie();
        if (frontend_->wants_close() && !reported_) {
            reported_ = true;
            report(frontend_->result());
        }
    }
    void draw(ui::Renderer& r, ui::TextRenderer& text) override {
        if (movie_playing_ && !movie_frame_.rgba.empty()) {
            r.draw_frame({0, 0, ui::kScreenW, ui::kScreenH}, movie_frame_.width, movie_frame_.height,
                         movie_frame_.rgba.data());
            return;
        }
        if (background_)
            background_->draw(r);
        else
            r.fill({0, 0, ui::kScreenW, ui::kScreenH}, {0x10, 0x14, 0x1c, 0x80});
        frontend_->draw(r, text);
    }

    // take_movie_request playback: video fullscreen over the menu, PCM through a short-lived
    // SDL device, A/V by the audio clock. The menu's own Back-skip/transition stops playback.
    void update_movie() {
        if (!movie_playing_) {
            const std::uint32_t id = frontend_->take_movie_request();
            if (!id) return;
            char name[16];
            std::snprintf(name, sizeof(name), "%08X.PSS", id);
            const std::string path = gamedir_ + "/MOVIES/30_FPS/" + name;
            if (!movie_.open(path)) return;  // no file: the page falls back to its hold transition
            movie_page_ = frontend_->page_id();
            if (movie_.audio().present) {
                SDL_AudioSpec spec{};
                spec.format = SDL_AUDIO_S16;
                spec.channels = movie_.audio().channels;
                spec.freq = movie_.audio().sample_rate;
                movie_audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
                if (movie_audio_) SDL_ResumeAudioStreamDevice(movie_audio_);
            }
            movie_playing_ = true;
            movie_audio_eof_ = !movie_.audio().present;
            movie_video_eof_ = false;
            return;
        }
        if (frontend_->page_id() != movie_page_) {
            stop_movie();
            return;
        }
        if (movie_audio_ && !movie_audio_eof_) {
            const int queued = SDL_GetAudioStreamQueued(movie_audio_);
            const int bps = movie_.audio().sample_rate * movie_.audio().channels * 2;
            if (bps > 0 && queued < bps / 4) {
                std::vector<std::int16_t> pcm;
                if (movie_.next_audio(pcm, 0.25))
                    SDL_PutAudioStreamData(movie_audio_, pcm.data(), int(pcm.size() * 2));
                else
                    movie_audio_eof_ = true;
            }
        }
        const double clock = movie_clock();
        for (;;) {
            if (!movie_pending_valid_) {
                if (!movie_.next_video(movie_pending_)) {
                    movie_video_eof_ = true;
                    break;
                }
                movie_pending_valid_ = true;
            }
            if (movie_pending_.pts > clock + 1e-3) break;
            movie_frame_ = std::move(movie_pending_);
            movie_pending_valid_ = false;
        }
        if (movie_video_eof_ && !movie_pending_valid_ && (!movie_audio_ || movie_audio_eof_)) {
            stop_movie();
            frontend_->movie_finished();
        }
    }

    double movie_clock() {
        if (movie_audio_ && movie_.audio().present && !movie_audio_eof_) {
            const int queued = SDL_GetAudioStreamQueued(movie_audio_);
            const int bps = movie_.audio().sample_rate * movie_.audio().channels * 2;
            if (bps > 0) return movie_.audio_position() - double(queued) / double(bps);
        }
        movie_wall_ += 1.0 / 30.0;
        return movie_wall_;
    }

    void stop_movie() {
        movie_playing_ = false;
        movie_pending_valid_ = false;
        movie_frame_ = {};
        if (movie_audio_) {
            SDL_DestroyAudioStream(movie_audio_);
            movie_audio_ = nullptr;
        }
    }
private:
    // Text listing of the parsed script (pages, controls, messages, scripts) for reversing work.
    static void dump(const MenuFile& f) {
        for (const MenuComponentSet& set : f.skins) {
            std::printf("skin set %u:", set.id);
            for (const MenuComponent& c : set.components) std::printf(" %zu", c.instances.size());
            std::printf("\n");
        }
        for (const MenuPage& p : f.pages) {
            std::printf("page 0x%08x menu 0x%08x extra 0x%x\n", p.id, p.menu, p.extra);
            for (const MenuControl& c : p.controls) {
                std::printf("  ctl 0x%08x type %u box %d,%d %dx%d layer %u index %u skin %u a=%u b=%u\n", c.id, c.type, c.x, c.y,
                            c.w, c.h, c.layer, c.index, c.skin, c.a, c.b);
                for (const MenuMessage& m : c.messages) std::printf("    msg 0x%02x a=0x%x b=0x%x\n", m.type, m.a, m.b);
                for (const MenuScript& sc : c.scripts) {
                    std::printf("    script 0x%08x param %u\n", sc.id, sc.param);
                    for (const MenuKeyframe& k : sc.keyframes) {
                        std::printf("      keyframe t=%u box %u,%u %ux%u flags 0x%x\n", k.v[0], k.v[1], k.v[2], k.v[3], k.v[4], k.tail);
                        for (const MenuKeyframeMessage& m : k.messages)
                            std::printf("        -> 0x%08x msg 0x%02x a=0x%x b=0x%x\n", m.target, m.type, m.a, m.b);
                    }
                }
            }
        }
    }

    static void report(const FrontendResult& r) {
        static const char* const names[] = {"none", "start-multiplayer", "start-mission", "resume", "restart-mission",
                                            "quit-to-menu", "mp-rematch", "mission-done", "quit"};
        std::printf("result: %s level=%s (0x%08x)", names[int(r.action)], r.level_bin.c_str(), r.level_id);
        if (r.launch) {
            const MpSettings& s = r.launch->settings;
            std::printf(" mode=0x%08x humans=%u bots=%u duration=%d score-limit=%d weapon-set=%d participants=%zu", s.mode,
                        s.human_count, s.bot_count, s.duration, s.score_limit, s.weapon_set, r.launch->participants.size());
        }
        std::printf("\n");
    }

    bool reported_ = false, trace_ = false, dump_ = false;
    std::uint32_t last_page_ = 0, frame_ = 0;
    bool last_lock_ = false;
    std::string gamedir_;
    MenuFile menu_;
    std::unique_ptr<MpData> mp_data_;
    std::unique_ptr<SpMenuData> sp_data_;
    std::unique_ptr<TweakData> tweak_data_;
    std::unique_ptr<Frontend> frontend_;
    std::unique_ptr<app::MenuBackground> background_;
    // take_movie_request playback state (video over the menu, PCM via SDL).
    media::MoviePlayer movie_;
    media::MovieFrame movie_frame_, movie_pending_;
    bool movie_playing_ = false, movie_pending_valid_ = false;
    bool movie_audio_eof_ = false, movie_video_eof_ = false;
    std::uint32_t movie_page_ = 0;
    double movie_wall_ = 0;
    SDL_AudioStream* movie_audio_ = nullptr;
};

}  // namespace

std::unique_ptr<Scene> make_menu_scene(SceneArgs& args) { return std::make_unique<MenuScene>(args); }

}  // namespace nf
