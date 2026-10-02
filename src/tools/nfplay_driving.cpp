#include "tools/nfplay_driving.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>

#include "audio/driving_mixer.hpp"
#include "audio/wav.hpp"

namespace nf {

using namespace nf::audio;

namespace {

constexpr std::size_t kFramesPerUpdate = 800;  // 48000 / 60

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool is_number(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }); }

std::filesystem::path driving_dir(const std::string& gamedir) {
    auto dir = find_driving_dir(gamedir);
    if (!dir) throw std::runtime_error(gamedir + " has no DRIVING directory");
    return *dir;
}

// The banks.ini section of a mission: the one whose name ends with the stem ("paris_mis01" for MIS01).
std::string default_level(DrivingMission& m) {
    const std::string tail = lower(m.stem());
    for (const auto& [name, refs] : m.banks_ini().sections) {
        const std::string l = lower(name);
        if (l == tail || (l.size() > tail.size() && l.ends_with("_" + tail))) return name;
    }
    if (m.banks_ini().sections.empty()) throw std::runtime_error(m.stem() + " has no banks.ini");
    return m.banks_ini().sections.front().first;
}

void report(const std::vector<std::int16_t>& pcm, const std::string& path) {
    double sum = 0;
    int peak = 0;
    for (auto s : pcm) {
        sum += double(s) * s;
        peak = std::max(peak, std::abs(int(s)));
    }
    std::printf("wrote %s: %.2f s, peak %d, rms %.0f\n", path.c_str(), double(pcm.size()) / 2 / DrivingAudio::kRate, peak,
                pcm.empty() ? 0.0 : std::sqrt(sum / double(pcm.size())));
}

// One game loop: `tick(t)` once per 1/60 s, then 800 frames of audio, until `done()` or `limit` seconds.
// Without --wav the device plays in real time; otherwise the audio is collected.
template <typename Tick, typename Done>
std::vector<std::int16_t> run_loop(DrivingAudio& audio, double limit, bool realtime, Tick tick, Done done) {
    std::vector<std::int16_t> out, block(kFramesPerUpdate * 2);
    for (double t = 0; t < limit && !done(); t += 1.0 / 60.0) {
        tick(t);
        if (realtime) {
            std::this_thread::sleep_for(std::chrono::microseconds(16667));
        } else {
            audio.render(block.data(), kFramesPerUpdate);
            out.insert(out.end(), block.begin(), block.end());
        }
    }
    return out;
}

void finish(const std::vector<std::int16_t>& pcm, const DrivingPlayOptions& opt, double tail_seconds) {
    if (opt.wav.empty()) return;
    auto out = pcm;
    out.resize(out.size() + std::size_t(tail_seconds * DrivingAudio::kRate) * 2, 0);
    write_wav(opt.wav, out, DrivingAudio::kRate, 2);
    report(out, opt.wav);
}

void open_output(DrivingAudio& audio, const DrivingPlayOptions& opt) {
    if (!opt.wav.empty()) return;
    if (!audio.open_device()) throw std::runtime_error("no audio device (" + audio.last_error() + "); use --wav out.wav");
}

int cmd_list(const std::vector<std::string>& args) {
    auto dir = driving_dir(args[0]);
    std::vector<std::string> stems;
    for (std::size_t i = 2; i < args.size(); ++i) stems.push_back(upper(args[i]));
    if (stems.empty()) stems = list_driving_missions(dir);
    for (const auto& stem : stems) {
        DrivingMission m(dir, stem);
        std::printf("%s\n", stem.c_str());
        for (const auto& [level, refs] : m.banks_ini().sections) {
            std::printf("  level %s:", level.c_str());
            for (const auto& r : refs) std::printf(" %s=%s", r.role.c_str(), r.path.c_str());
            std::printf("\n");
        }
        for (const auto& f : m.mix_files()) std::printf("  mix %s (%zu presets)\n", f.c_str(), m.mix_presets(f).size());
        for (const auto& p : m.bank_paths()) {
            auto bank = m.bank(p);
            std::printf("  bank %s: %zu sounds\n", p.c_str(), bank->sounds().size());
        }
        if (m.has_music())
            for (const auto& e : m.music()->entries()) {
                EaStream s = DrivingMission::read_stream(*m.music(), e);
                std::printf("  music %s: %.2f s, %u ch\n", e.path.c_str(), s.duration_seconds(), s.info().channels);
            }
        for (const auto& lang : m.speech_languages()) {
            auto sp = m.open_speech(lang);
            std::printf("  speech %s: %zu clips\n", lang.c_str(), sp->entries().size());
        }
    }
    return 0;
}

int cmd_sfx(const std::vector<std::string>& args, const DrivingPlayOptions& opt) {
    if (args.size() != 5) throw std::runtime_error("usage: nfplay <gamedir> driving-sfx <MIS> <role> <name|slot>");
    DrivingMission m(driving_dir(args[0]), upper(args[2]));
    const std::string level = opt.level.empty() ? default_level(m) : opt.level;
    DrivingAudio audio;
    audio.load_level(m, level);
    const std::string& role = args[3];
    auto info = is_number(args[4]) ? audio.find_slot(role, std::stoi(args[4])) : audio.find_sample(role, args[4]);
    if (!info) throw std::runtime_error("no such sound in role " + role + " of level " + level);
    std::printf("level %s, %s slot %d: %u Hz, %s, %s, first pass %.2f s\n", level.c_str(), info->bank.c_str(), info->slot,
                info->rate, info->xa ? "EA-XA (IOP mixer)" : "SPU ADPCM", info->loops ? "loops" : "one shot", info->first_pass_seconds);
    open_output(audio, opt);
    DrivingAudio::PlayParams p{opt.volume, opt.pitch, opt.pan, true};
    auto id = audio.play_slot(role, info->slot, p);
    if (!id) throw std::runtime_error("sound did not start");
    const double limit = opt.seconds > 0 ? opt.seconds : (info->loops ? 4.0 : info->first_pass_seconds / std::max(opt.pitch, 0.01f) + 0.1);
    auto pcm = run_loop(audio, limit, opt.wav.empty(), [](double) {}, [&] { return !audio.voice_playing(id); });
    finish(pcm, opt, 0.25);
    return 0;
}

int cmd_stream(const std::vector<std::string>& args, const DrivingPlayOptions& opt, bool speech) {
    if (args.size() != 4) throw std::runtime_error(std::string("usage: nfplay <gamedir> ") + args[1] + " <MIS> <name>");
    DrivingMission m(driving_dir(args[0]), upper(args[2]));
    std::unique_ptr<BigArchive> owned;
    BigArchive* archive;
    if (speech) {
        owned = m.open_speech(upper(opt.lang));
        archive = owned.get();
    } else {
        archive = m.music();
        if (!archive) throw std::runtime_error(m.stem() + " has no music file");
    }
    std::string name = args[3];
    if (lower(name).find(".asf") == std::string::npos) name += ".asf";
    const BigEntry* entry = archive->find(name);
    if (!entry) throw std::runtime_error("no stream " + name + " in " + m.stem() + (speech ? " speech" : " music"));
    auto stream = std::make_shared<const EaStream>(DrivingMission::read_stream(*archive, *entry));
    std::printf("%s: %.2f s, %u Hz, %u ch, %u blocks\n", entry->path.c_str(), stream->duration_seconds(), stream->info().sample_rate,
                stream->info().channels, stream->info().block_count);
    DrivingAudio audio;
    open_output(audio, opt);
    const auto channel = speech ? DrivingAudio::Channel::Speech : DrivingAudio::Channel::Music;
    audio.play_stream(channel, stream, {opt.volume, opt.pan, opt.loop, 0.0f});
    const double limit = opt.seconds > 0 ? opt.seconds : (opt.loop ? 30.0 : stream->duration_seconds() + 0.1);
    auto pcm = run_loop(audio, limit, opt.wav.empty(), [](double) {}, [&] { return !audio.stream_playing(channel); });
    finish(pcm, opt, 0.0);
    return 0;
}

int cmd_engine(const std::vector<std::string>& args, const DrivingPlayOptions& opt) {
    if (args.size() != 3) throw std::runtime_error("usage: nfplay <gamedir> engine <MIS> [--rpm A:B] [--gas G] [--cabin C] [--seconds S]");
    DrivingMission m(driving_dir(args[0]), upper(args[2]));
    const std::string level = opt.level.empty() ? default_level(m) : opt.level;
    DrivingAudio audio;
    audio.load_level(m, level);
    audio.start_vehicle();
    open_output(audio, opt);
    const double limit = opt.seconds > 0 ? opt.seconds : 12.0;
    std::printf("level %s: rpm %.0f -> %.0f over %.1f s, gas %.2f, cabin %.2f\n", level.c_str(), opt.rpm_from, opt.rpm_to, limit,
                opt.gas, opt.cabin);
    std::printf("   t    rpm(in) rpm(smooth)  pitch  idle-out  low-out  hi-out cruz-out   [hz of the layer samples = rate * pitch]\n");
    double next_print = 0;
    auto tick = [&](double t) {
        VehicleState st;
        st.rpm = opt.rpm_from + (opt.rpm_to - opt.rpm_from) * float(std::min(t / limit, 1.0));
        st.gas = opt.gas;
        st.speed = opt.speed;
        SoundPath path;
        path.cabin = opt.cabin;
        audio.update_vehicle(st, path);
        if (t >= next_print) {
            next_print += 1.0;
            const VehicleSound& v = *audio.vehicle();
            std::printf("%5.1f  %8.0f  %8.0f  %7.3f  %7.3f  %7.3f  %7.3f  %7.3f\n", t, st.rpm, v.engine().rpm, v.engine().pitch,
                        v.voice(VehicleVoice::IdleOut).params.volume, v.voice(VehicleVoice::LowloadOut).params.volume,
                        v.voice(VehicleVoice::HiloadOut).params.volume, v.voice(VehicleVoice::CruzOut).params.volume);
        }
    };
    auto pcm = run_loop(audio, limit, opt.wav.empty(), tick, [] { return false; });
    finish(pcm, opt, 0.5);
    return 0;
}

}  // namespace

bool is_driving_command(const std::string& cmd) {
    return cmd == "driving-list" || cmd == "driving-sfx" || cmd == "driving-music" || cmd == "driving-speech" || cmd == "engine";
}

int run_driving(const std::vector<std::string>& args, const DrivingPlayOptions& opt) {
    const std::string& cmd = args[1];
    if (cmd == "driving-list") return cmd_list(args);
    if (args.size() < 3) throw std::runtime_error("missing mission name (MIS01, MIS3, RACE, ...)");
    if (cmd == "driving-sfx") return cmd_sfx(args, opt);
    if (cmd == "driving-music") return cmd_stream(args, opt, false);
    if (cmd == "driving-speech") return cmd_stream(args, opt, true);
    return cmd_engine(args, opt);
}

}  // namespace nf
