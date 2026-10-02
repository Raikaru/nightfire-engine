// nfplay: play or export Nightfire (PS2) sound data.
//   nfplay <gamedir> sfx <bank slot> <effect index>   one effect of a sound bank
//   nfplay <gamedir> sfx-name <SFX_NAME>              an effect by name (loads the bank that has it)
//   nfplay <gamedir> stream <index>                   a STREAMS.BIN clip
//   nfplay <gamedir> music <n> [section]              music track MFX_<n>, starting at a section
// Options:
//   --wav out.wav       render to a 48 kHz stereo WAV instead of the sound device
//   --seconds S         play/render length (music: default 30; effects: until they end, at most 60)
//   --at x,y,z          place an effect in the world (listener at the origin facing +z, left axis +x)
//   --jump T:S          music: at T seconds request a jump to section S (repeatable; T:S:i = instant)
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "audio/audio.hpp"
#include "audio/wav.hpp"
#include "tools/nfplay_driving.hpp"

using namespace nf;
using namespace nf::audio;

namespace {

struct Jump {
    double at;
    std::uint32_t section;
    bool instant;
};

struct Options {
    std::string wav;
    double seconds = -1;
    std::optional<Vec3> at;
    std::vector<Jump> jumps;
};

Vec3 parse_vec3(const std::string& s) {
    Vec3 v{};
    if (std::sscanf(s.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) throw std::runtime_error("bad vector " + s);
    return v;
}

Jump parse_jump(const std::string& s) {
    double t;
    unsigned section, instant = 0;
    if (std::sscanf(s.c_str(), "%lf:%u:%u", &t, &section, &instant) < 2) throw std::runtime_error("bad --jump " + s);
    return {t, section, instant != 0};
}

constexpr std::size_t kFramesPerUpdate = 800;  // 48000 / 60

// Runs the game loop for the sound engine: update() once per 1/60 s, rendering in between, until
// `done()` or the time limit. A device plays in real time; otherwise the audio is collected.
template <typename Done>
std::vector<std::int16_t> run(AudioSystem& audio, double limit, bool realtime, const std::vector<Jump>& jumps,
                              Done done) {
    std::vector<std::int16_t> out;
    std::size_t next_jump = 0;
    std::vector<std::int16_t> block(kFramesPerUpdate * 2);
    double t = 0;
    while (t < limit && !done()) {
        while (next_jump < jumps.size() && jumps[next_jump].at <= t) {
            audio.jump_music(jumps[next_jump].section, jumps[next_jump].instant);
            ++next_jump;
        }
        audio.update();
        if (realtime) {
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
        } else {
            audio.render(block.data(), kFramesPerUpdate);
            out.insert(out.end(), block.begin(), block.end());
        }
        t += 1.0 / 60.0;
    }
    return out;
}

void report(const std::vector<std::int16_t>& pcm, const std::string& path) {
    double sum = 0;
    int peak = 0;
    for (auto s : pcm) {
        sum += double(s) * s;
        peak = std::max(peak, std::abs(int(s)));
    }
    std::printf("wrote %s: %.2f s, peak %d, rms %.0f\n", path.c_str(), double(pcm.size()) / 2 / 48000.0, peak,
                pcm.empty() ? 0.0 : std::sqrt(sum / double(pcm.size())));
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args;
    Options opt;
    DrivingPlayOptions dopt;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
                return argv[++i];
            };
            if (a == "--wav") opt.wav = dopt.wav = value();
            else if (a == "--seconds") opt.seconds = dopt.seconds = std::stod(value());
            else if (a == "--at") opt.at = parse_vec3(value());
            else if (a == "--jump") opt.jumps.push_back(parse_jump(value()));
            else if (a == "--level") dopt.level = value();
            else if (a == "--lang") dopt.lang = value();
            else if (a == "--pitch") dopt.pitch = std::stof(value());
            else if (a == "--volume") dopt.volume = std::stof(value());
            else if (a == "--pan") dopt.pan = std::stof(value());
            else if (a == "--loop") dopt.loop = true;
            else if (a == "--gas") dopt.gas = std::stof(value());
            else if (a == "--cabin") dopt.cabin = std::stof(value());
            else if (a == "--speed") dopt.speed = std::stof(value());
            else if (a == "--rpm") {
                if (std::sscanf(value().c_str(), "%f:%f", &dopt.rpm_from, &dopt.rpm_to) != 2) throw std::runtime_error("--rpm needs A:B");
            }
            else args.push_back(a);
        }
        if (args.size() < 2) throw std::runtime_error("usage: nfplay <gamedir> sfx|sfx-name|stream|music|driving-list|driving-sfx|driving-music|driving-speech|engine ... [--wav out.wav]");
        if (is_driving_command(args[1])) return run_driving(args, dopt);

        SoundArchive archive(args[0]);
        AudioSystem audio(archive);
        const bool realtime = opt.wav.empty();
        if (realtime && !audio.open_device()) {
            std::fprintf(stderr, "no audio device (%s); use --wav out.wav to render to a file\n",
                         audio.last_error().c_str());
            return 1;
        }
        PlayOptions play;
        play.position = opt.at;
        const std::string& what = args[1];
        bool music = false;
        SfxHandle handle = 0;
        std::string label;

        if (what == "sfx" && args.size() == 4) {
            int slot = std::stoi(args[2]);
            std::size_t index = std::stoul(args[3]);
            audio.load_bank(slot);
            SoundBank bank = archive.load_bank(slot);
            if (index >= bank.effects().size()) throw std::runtime_error("effect index out of range");
            const SfxEntry& e = bank.effects()[index];
            label = "bank " + std::to_string(slot) + " effect " + std::to_string(index) + " id " +
                    std::to_string(e.id) + " " + std::string(archive.sfx_name(e.id));
            handle = audio.play_sfx(e.id, play);
        } else if (what == "sfx-name" && args.size() == 3) {
            auto id = archive.sfx_id(args[2]);
            if (!id) throw std::runtime_error("unknown SFX name " + args[2]);
            for (std::size_t slot = 0; slot < archive.bank_count() && !handle; ++slot) {
                if (!archive.load_bank(int(slot)).find(*id)) continue;
                audio.load_bank(int(slot));
                handle = audio.play_sfx(*id, play);
                label = args[2] + " from bank " + std::to_string(slot);
            }
        } else if (what == "stream" && args.size() == 3) {
            handle = audio.play_stream(std::uint32_t(std::stoul(args[2])), play);
            label = "stream " + args[2];
        } else if (what == "music" && args.size() >= 3) {
            audio.start_music_number(std::stoi(args[2]), args.size() > 3 ? std::uint32_t(std::stoul(args[3])) : 0);
            music = true;
            label = "music MFX_" + args[2];
        } else {
            throw std::runtime_error("bad arguments");
        }
        if (!music && !handle) throw std::runtime_error("effect did not start");
        std::printf("playing %s\n", label.c_str());

        double limit = opt.seconds > 0 ? opt.seconds : (music ? 30.0 : 60.0);
        auto pcm = run(audio, limit, realtime, opt.jumps, [&] {
            return music ? false : !audio.is_playing(handle);
        });
        if (music) {
            MusicStatus st = audio.music_status();
            std::printf("music: %s, section %lld, last jump %lld, pending %lld, position %.2f s\n",
                        st.playing ? "playing" : "finished", static_cast<long long>(st.now_playing),
                        static_cast<long long>(st.last_jump), static_cast<long long>(st.pending_jump), st.seconds);
        }
        if (!realtime) {
            if (!music) pcm.resize(pcm.size() + 48000 / 2 * 2, 0);  // half a second of tail
            write_wav(opt.wav, pcm, audio.output_rate(), 2);
            report(pcm, opt.wav);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
