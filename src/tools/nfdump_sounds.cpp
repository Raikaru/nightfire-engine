#include "tools/nfdump_sounds.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "assets/bin_archive.hpp"
#include "assets/driving_audio.hpp"
#include "assets/map_file.hpp"
#include "assets/map_sounds.hpp"
#include "assets/sound_archive.hpp"

namespace nf {

namespace {

const char* tracking_name(Tracking t) {
    switch (t) {
        case Tracking::Flat: return "flat";
        case Tracking::Front: return "front";
        case Tracking::Positional: return "3d";
        case Tracking::HeadLocked: return "head";
    }
    return "?";
}

const char* marker_name(MarkerType t) {
    switch (t) {
        case MarkerType::Plain: return "plain";
        case MarkerType::EndAndFinish: return "end+finish";
        case MarkerType::LoopBack: return "loop";
        case MarkerType::LoopBackLocked: return "loop-locked";
        case MarkerType::End: return "end";
        case MarkerType::SectionStart: return "section";
    }
    return "?";
}

struct MapSoundSet {
    std::string name;
    std::vector<MapSound> sounds;
};

std::vector<MapSoundSet> read_map_sounds(GameFiles& files) {
    std::vector<MapSoundSet> out;
    for (const auto& f : files.files()) {
        if (!f.name.ends_with(".bin")) continue;
        auto data = files.read(f);
        for (const auto& entry : parse_bin_archive(Bytes(data))) {
            if (!is_map_chunk_file(entry.type)) continue;
            for (const auto& block : walk_blocks(entry.data)) {
                if (block.id != static_cast<std::uint8_t>(BlockId::MapSounds)) continue;
                out.push_back({f.name + "/" + entry.name, parse_map_sounds(block.data)});
            }
        }
    }
    return out;
}

// Decoded statistics of a signal, for sanity checking.
struct Signal {
    std::size_t samples = 0;
    double sum_sq = 0;
    int peak = 0;
    void add(const std::int16_t* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            sum_sq += double(p[i]) * p[i];
            peak = std::max(peak, std::abs(int(p[i])));
        }
        samples += n;
    }
    double rms() const { return samples ? std::sqrt(sum_sq / double(samples)) : 0.0; }
};

Signal decode_stereo(Bytes audio) {
    Signal s;
    AdpcmState l, r;
    std::int16_t buf[kStereoBlockFrames * 2];
    for (std::size_t off = 0; off + kStreamBlockBytes <= audio.size(); off += kStreamBlockBytes) {
        decode_stereo_block(audio.data() + off, l, r, buf);
        s.add(buf, kStereoBlockFrames * 2);
    }
    return s;
}

void print_bank_list(const SoundArchive& a) {
    std::printf("%-5s %-6s %8s %8s %10s %9s\n", "slot", "hash", "samples", "effects", "sbf bytes", "seconds");
    for (std::size_t slot = 0; slot < a.bank_count(); ++slot) {
        SoundBank bank = a.load_bank(int(slot));
        double seconds = 0;
        std::size_t bytes = 0;
        for (const auto& s : bank.samples()) {
            seconds += double(s.real_size / kAdpcmFrameBytes * kAdpcmFrameSamples) / s.sample_rate();
            bytes += s.size;
        }
        std::printf("%-5zu 0x%-4x %8zu %8zu %10zu %9.1f\n", slot, a.bank_hash(slot), bank.samples().size(),
                    bank.effects().size(), bytes, seconds);
    }
}

int print_bank(const SoundArchive& a, int slot) {
    SoundBank bank = a.load_bank(slot);
    std::printf("bank slot %d (hash 0x%x): %zu samples, %zu effects\n", slot, a.bank_hash(std::size_t(slot)),
                bank.samples().size(), bank.effects().size());
    for (std::size_t i = 0; i < bank.samples().size(); ++i) {
        const SampleHeader& s = bank.samples()[i];
        std::printf("  sample %3zu  %7u bytes  %7.0f Hz  %6.2fs  %s\n", i, s.size, s.sample_rate(),
                    double(s.real_size / kAdpcmFrameBytes * kAdpcmFrameSamples) / s.sample_rate(),
                    s.loops() ? "loop" : "");
    }
    for (std::size_t i = 0; i < bank.effects().size(); ++i) {
        const SfxEntry& e = bank.effects()[i];
        const SfxParams& p = e.params;
        std::string_view name = a.sfx_name(e.id);
        std::printf("  sfx %3zu id %-4u %-40.*s %-5s r %d..%d prio %d vol %d%s%s%s [", i, e.id, int(name.size()),
                    name.data(), tracking_name(p.tracking), p.inner_radius, p.outer_radius, p.priority,
                    p.master_volume, p.multi_sample ? (p.polyphonic ? " poly" : " seq") : "", p.loop ? " loop" : "",
                    p.max_voices ? " limited" : "");
        for (const auto& s : p.samples) {
            if (s.is_stream()) std::printf(" stream:%u", s.stream_index());
            else std::printf(" %d", s.file_ref);
        }
        std::printf(" ]\n");
    }
    return 0;
}

int print_music(const SoundArchive& a, int number) {
    MusicTrack t = a.load_music(number);
    std::printf("MFX_%d (hash %u): %.1fs, base volume %u, %zu sections, %zu markers\n", number,
                a.music_hash(std::size_t(number - 1)), t.seconds(), t.map.base_volume, t.map.sections.size(),
                t.map.markers.size());
    for (std::size_t i = 0; i < t.map.sections.size(); ++i) {
        const Section& s = t.map.sections[i];
        std::printf("  section %2zu  @%8u (%7.2fs)  marker %3u  %s\n", i, s.marker.pos, music_seconds(s.marker.pos),
                    s.marker_index, s.instant ? "instant" : "at next marker");
    }
    for (std::size_t i = 0; i < t.map.markers.size(); ++i) {
        const Marker& m = t.map.markers[i];
        if (m.type == MarkerType::Plain) continue;
        std::printf("  marker %3zu  @%8u  section %2d  %-11s", i, m.pos, int(m.section), marker_name(m.type));
        if (m.type == MarkerType::LoopBack || m.type == MarkerType::LoopBackLocked)
            std::printf("  -> marker %u @%u", m.loop_marker, m.loop_start);
        std::printf("\n");
    }
    return 0;
}

void print_music_list(const SoundArchive& a) {
    std::printf("%-6s %-6s %9s %9s %8s\n", "track", "hash", "seconds", "sections", "markers");
    for (std::size_t i = 0; i < a.music_count(); ++i) {
        MusicTrack t = a.load_music(int(i) + 1);
        std::printf("MFX_%-2zu %-6u %9.1f %9zu %8zu\n", i + 1, a.music_hash(i), t.seconds(), t.map.sections.size(),
                    t.map.markers.size());
    }
}

void print_streams(const SoundArchive& a) {
    double total = 0;
    for (std::uint32_t i = 0; i < a.stream_count(); ++i) total += a.load_stream(i).seconds();
    std::printf("%zu streams, %.1f s of mono 22050 Hz speech\n", a.stream_count(), total);
    for (std::uint32_t i = 0; i < a.stream_count(); ++i)
        if (i < 8) std::printf("  stream %u: %.2fs\n", i, a.load_stream(i).seconds());
}

void print_maps(GameFiles& files, const SoundArchive& a) {
    for (const auto& level : read_map_sounds(files)) {
        std::printf("%s: %zu map sounds\n", level.name.c_str(), level.sounds.size());
        for (const auto& s : level.sounds) {
            std::string_view name = a.sfx_name(s.sfx_id());
            std::printf("  sfx %-4u %-36.*s at (%.1f, %.1f, %.1f) vol %.0f radius %.1f/%.1f flags 0x%x\n", s.sfx_id(),
                        int(name.size()), name.data(), s.position[0], s.position[1], s.position[2], s.volume, s.radius,
                        s.inner_radius, s.raw_id >> 20);
        }
    }
}


// --- driving mission audio (DRIVING/*.VIV, *.MUS, *.SPE) ------------------------------------------------------

std::filesystem::path require_driving_dir(const std::filesystem::path& gamedir) {
    auto dir = find_driving_dir(gamedir);
    if (!dir) throw std::runtime_error(gamedir.string() + " has no DRIVING directory");
    return *dir;
}

std::string upper_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

void print_driving_missions(const std::filesystem::path& dir) {
    for (const auto& stem : list_driving_missions(dir)) {
        DrivingMission m(dir, stem);
        std::size_t sounds = 0, xa = 0;
        for (const auto& p : m.bank_paths()) {
            auto bank = m.bank(p);
            sounds += bank->sounds().size();
            for (const auto& s : bank->sounds()) xa += s.xa;
        }
        std::printf("%-7s %zu levels, %zu banks (%zu sounds, %zu EA-XA), %s music, speech:", stem.c_str(),
                    m.banks_ini().sections.size(), m.bank_paths().size(), sounds, xa, m.has_music() ? "with" : "no");
        for (const auto& l : m.speech_languages()) std::printf(" %s", l.c_str());
        std::printf("\n");
    }
}

void print_driving_mission(DrivingMission& m) {
    std::printf("%s\n", m.stem().c_str());
    for (const auto& [level, refs] : m.banks_ini().sections) {
        std::printf("  level %s\n", level.c_str());
        for (const auto& r : refs) std::printf("    %-12s %s\n", r.role.c_str(), r.path.c_str());
    }
    for (const auto& f : m.mix_files()) {
        auto presets = m.mix_presets(f);
        std::printf("  mix %s: %zu presets\n", f.c_str(), presets.size());
        for (const auto& p : presets) std::printf("    %-20s %.3f  group %d\n", p.name.c_str(), p.volume, p.group);
    }
}

void print_driving_bank(DrivingMission& m, const std::string& path) {
    auto bank = m.bank(path);
    auto index = m.bank_index(path);
    std::printf("%s: %zu slots, %zu sounds\n", path.c_str(), bank->slot_count(), bank->sounds().size());
    std::printf("%-5s %-28s %6s %-6s %8s %9s %9s %5s\n", "slot", "name", "rate", "codec", "samples", "loop from", "loop to", "vol%");
    for (const EaSound& s : bank->sounds()) {
        const auto* name = index->name_of(int(s.slot));
        std::printf("%-5zu %-28s %6u %-6s %8u ", s.slot, name ? name->name.c_str() : "-", s.sample_rate, s.xa ? "EA-XA" : "SPU", s.num_samples);
        if (s.loop_start) std::printf("%9u %9u", *s.loop_start, *s.loop_end);
        else std::printf("%9s %9s", "-", "-");
        std::printf(" %5u\n", s.volume_percent);
    }
}

void print_driving_streams(DrivingMission& m, const std::string& lang) {
    auto list = [&](const char* what, BigArchive& a) {
        for (const BigEntry& e : a.entries()) {
            EaStream s = DrivingMission::read_stream(a, e);
            std::printf("  %-6s %-28s %8.2f s  %u ch  %u blocks\n", what, e.path.c_str(), s.duration_seconds(), s.info().channels, s.info().block_count);
        }
    };
    if (m.has_music()) list("music", *m.music());
    auto langs = m.speech_languages();
    for (const auto& l : langs) {
        if (!lang.empty() && upper_copy(lang) != l) continue;
        auto sp = m.open_speech(l);
        list(l.c_str(), *sp);
    }
}

int cmd_driving_sounds(const std::filesystem::path& gamedir, const std::vector<std::string>& args) {
    // args[0] == "driving"
    const auto dir = require_driving_dir(gamedir);
    if (args.size() == 1) {
        print_driving_missions(dir);
        return 0;
    }
    DrivingMission m(dir, upper_copy(args[1]));
    if (args.size() == 2) print_driving_mission(m);
    else if (args.size() == 4 && args[2] == "bank") print_driving_bank(m, args[3]);
    else if (args.size() >= 3 && args[2] == "streams") print_driving_streams(m, args.size() > 3 ? args[3] : "");
    else {
        std::fprintf(stderr, "usage: nfdump <gamedir> sounds driving [MIS [bank <path>|streams [lang]]]\n");
        return 2;
    }
    return 0;
}

}  // namespace

int cmd_sounds(GameFiles& files, const std::filesystem::path& gamedir, const std::vector<std::string>& args) {
    if (!args.empty() && args[0] == "driving") return cmd_driving_sounds(gamedir, args);
    SoundArchive a(gamedir);
    std::string what = args.empty() ? "banks" : args[0];
    if (what == "banks") print_bank_list(a);
    else if (what == "bank" && args.size() == 2) return print_bank(a, std::stoi(args[1]));
    else if (what == "music" && args.size() == 2) return print_music(a, std::stoi(args[1]));
    else if (what == "music") print_music_list(a);
    else if (what == "streams") print_streams(a);
    else if (what == "maps") print_maps(files, a);
    else {
        std::fprintf(stderr, "usage: nfdump <gamedir> sounds [banks|bank <slot>|music [n]|streams|maps|driving ...]\n");
        return 2;
    }
    return 0;
}

static std::size_t validate_driving_audio(const std::filesystem::path& gamedir);

static std::size_t validate_action_sounds(GameFiles& files, const std::filesystem::path& gamedir) {
    std::size_t failures = 0;
    auto fail = [&](const std::string& what, const std::exception& e) {
        std::printf("FAIL %s: %s\n", what.c_str(), e.what());
        ++failures;
    };

    if (!std::filesystem::exists(gamedir / "PS2" / "SBINFO.SBI")) {
        std::printf("sound archive: no PS2/ directory in the game directory, skipped\n");
        return 0;
    }
    SoundArchive a(gamedir);
    std::printf("sound archive: %zu banks, %zu music tracks, %zu streams, %zu SFX names\n", a.bank_count(),
                a.music_count(), a.stream_count(), a.sfx_names().size());

    // Banks: every sample decodes to its end frame, every effect references valid samples/streams.
    std::size_t samples = 0, looping = 0, silent = 0, effects = 0, unnamed = 0, stream_refs = 0, seam_clicks = 0;
    double sample_seconds = 0;
    std::set<std::uint32_t> ids;
    for (std::size_t slot = 0; slot < a.bank_count(); ++slot) {
        try {
            SoundBank bank = a.load_bank(int(slot));
            for (std::size_t i = 0; i < bank.samples().size(); ++i) {
                const SampleHeader& h = bank.samples()[i];
                DecodedSample d = bank.decode(i);
                Signal sig;
                sig.add(d.pcm.data(), d.pcm.size());
                if (sig.peak == 0) ++silent;
                // The end frame is the last real frame: everything after it is padding.
                if (d.pcm.size() / kAdpcmFrameSamples * kAdpcmFrameBytes > h.real_size)
                    throw FormatError("sample " + std::to_string(i) + " decodes past its real size");
                if (d.loops != h.loops()) throw FormatError("sample " + std::to_string(i) + " loop flag disagrees with header");
                if (d.loops && d.loop_start >= d.pcm.size()) throw FormatError("bad loop point");
                looping += d.loops;
                if (d.loops) {
                    // The wrap from the last sample to the loop start should be no rougher than the
                    // signal itself; a wrong loop point shows up as a step there.
                    double rough = 0;
                    for (std::size_t k = 1; k < d.pcm.size(); ++k) rough += std::abs(d.pcm[k] - d.pcm[k - 1]);
                    rough /= double(d.pcm.size() - 1);
                    if (std::abs(d.pcm.back() - d.pcm[d.loop_start]) > 8 * rough + 64) ++seam_clicks;
                }
                sample_seconds += double(d.pcm.size()) / h.sample_rate();
                ++samples;
            }
            for (const auto& e : bank.effects()) {
                ++effects;
                ids.insert(e.id);
                if (a.sfx_name(e.id).empty()) ++unnamed;
                if (!a.sfx_defaults(e.id)) throw FormatError("SFX id " + std::to_string(e.id) + " outside SFXOutputData");
                for (const auto& s : e.params.samples) {
                    if (!s.is_stream()) continue;
                    ++stream_refs;
                    if (s.stream_index() >= a.stream_count())
                        throw FormatError("SFX " + std::to_string(e.id) + " references missing stream");
                }
            }
        } catch (const std::exception& e) {
            fail("bank " + std::to_string(slot), e);
        }
    }
    std::printf("banks: %zu samples (%zu looping, %zu with a step at the loop seam, %zu silent), %.0f s; %zu effects "
                "(%zu distinct ids, %zu without a name), %zu stream references\n",
                samples, looping, seam_clicks, silent, sample_seconds, effects, ids.size(), unnamed, stream_refs);

    // Music: markers, and every block of both channels.
    double music_seconds_total = 0;
    std::size_t sections = 0, markers = 0;
    for (std::size_t i = 0; i < a.music_count(); ++i) {
        try {
            MusicTrack t = a.load_music(int(i) + 1);
            Signal sig = decode_stereo(t.audio);
            if (sig.peak < 1000) throw FormatError("music decodes to near silence");
            music_seconds_total += t.seconds();
            sections += t.map.sections.size();
            markers += t.map.markers.size();
        } catch (const std::exception& e) {
            fail("music " + std::to_string(i + 1), e);
        }
    }
    std::printf("music: %zu tracks, %.0f s, %zu sections, %zu markers\n", a.music_count(), music_seconds_total, sections,
                markers);

    // Streams.
    double stream_seconds_total = 0;
    for (std::uint32_t i = 0; i < a.stream_count(); ++i) {
        try {
            StreamClip c = a.load_stream(i);
            DecodedSample d = decode_spu_sample(Bytes(c.audio));
            if (d.loops || d.pcm.size() / kAdpcmFrameSamples * kAdpcmFrameBytes + kAdpcmFrameBytes != c.audio.size())
                throw FormatError("stream does not end with one end frame and one silent frame");
            // The end marker sits just before the end frame.
            std::size_t end_frame = c.audio.size() - 2 * kAdpcmFrameBytes;
            if (c.map.markers.back().type != MarkerType::End || c.map.markers.back().pos > end_frame ||
                c.map.markers.back().pos + kAdpcmFrameBytes < end_frame)
                throw FormatError("end marker does not match the end frame");
            stream_seconds_total += c.seconds();
        } catch (const std::exception& e) {
            fail("stream " + std::to_string(i), e);
        }
    }
    std::printf("streams: %zu clips, %.0f s\n", a.stream_count(), stream_seconds_total);

    // In-level sound emitters. A single all-zero record is the empty placeholder.
    std::size_t levels = 0, emitters = 0, placeholders = 0;
    try {
        for (const auto& level : read_map_sounds(files)) {
            ++levels;
            for (const auto& s : level.sounds) {
                if (s.raw_id == 0 && s.radius == 0) {
                    ++placeholders;
                    continue;
                }
                ++emitters;
                if (a.sfx_name(s.sfx_id()).empty() || s.inner_radius > s.radius || s.radius <= 0)
                    throw FormatError(level.name + ": bad map sound for SFX " + std::to_string(s.sfx_id()));
            }
        }
    } catch (const std::exception& e) {
        fail("map sounds", e);
    }
    std::printf("map sounds: %zu emitters in %zu chunk files (+%zu empty placeholders)\n", emitters, levels, placeholders);
    return failures;
}

static std::size_t validate_driving_audio(const std::filesystem::path& gamedir) {
    auto dir = find_driving_dir(gamedir);
    if (!dir) {
        std::printf("driving audio: no DRIVING/ directory in the game directory, skipped\n");
        return 0;
    }
    std::size_t failures = 0;
    auto fail = [&](const std::string& what, const std::exception& e) {
        std::printf("FAIL %s: %s\n", what.c_str(), e.what());
        ++failures;
    };

    std::size_t missions = 0, banks = 0, sounds = 0, spu = 0, xa = 0, looping = 0, silent = 0, seam_clicks = 0, names = 0,
                dangling_names = 0, flag_offset_ok = 0, flag_offset_other = 0, presets = 0, engines = 0;
    double bank_seconds = 0;
    std::set<std::string> seen_banks;
    std::size_t streams = 0, music = 0, speech = 0, stream_blocks = 0, stream_block_seams = 0, stream_block_clicks = 0;
    double music_seconds = 0, speech_seconds = 0;

    // banks.ini is one global file: every mission's copy lists all levels, but each VIV ships only
    // its own mission's banks (MIS01 the paris set, MIS11 the isap set, ...). A referenced bank must
    // therefore ship in at least one mission VIV, not necessarily this one.
    std::set<std::string> shipped;
    auto normalise = [](std::string s) {
        for (char& c : s) c = c == '/' ? '\\' : (c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c);
        return s;
    };
    for (const auto& stem : list_driving_missions(*dir)) {
        DrivingMission m(*dir, stem);
        for (const auto& p : m.bank_paths()) shipped.insert(normalise(std::string("data\\audio\\") + p));
    }

    for (const auto& stem : list_driving_missions(*dir)) {
        try {
            DrivingMission m(*dir, stem);
            ++missions;
            // Every bank of every level ships on the disc, its index names real sounds, and every sound decodes.
            for (const auto& [level, refs] : m.banks_ini().sections) {
                for (const auto& r : refs)
                    if (!shipped.contains(normalise(std::string("data\\audio\\") + r.path)))
                        throw FormatError("level " + level + " lists missing bank " + r.path);
            }
            for (const auto& p : m.bank_paths()) {
                const auto bank = m.bank(p);
                const auto index = m.bank_index(p);
                // The same bank ships in several missions; decode each distinct file once.
                std::string key = p + ":" + std::to_string(bank->sounds().size()) + ":" +
                                  std::to_string(bank->sounds().empty() ? 0 : bank->sounds().back().data_offset);
                if (!seen_banks.insert(key).second) continue;
                ++banks;
                for (const EaSound& snd : bank->sounds()) {
                    try {
                        const EaSample d = bank->decode(snd);
                        ++sounds;
                        (snd.xa ? xa : spu)++;
                        Signal sig;
                        sig.add(d.head.data(), d.head.size());
                        sig.add(d.body.data(), d.body.size());
                        if (sig.peak == 0) ++silent;
                        if (d.head.empty() && d.body.empty()) throw FormatError("no samples");
                        if (snd.xa && d.loops() != snd.loop_start.has_value()) throw FormatError("XA loop disagrees with the loop tags");
                        if (!snd.xa && snd.loop_start && !d.loops()) throw FormatError("loop tags but the frame flags do not loop");
                        bank_seconds += double(d.first_pass()) / snd.sample_rate;
                        if (d.loops()) {
                            ++looping;
                            // A wrong loop point shows as a step at the wrap that the signal itself never makes.
                            const auto& tail = snd.xa ? d.head : d.body;
                            const auto& start = d.body;
                            double rough = 0;
                            for (std::size_t k = 1; k < start.size(); ++k) rough += std::abs(start[k] - start[k - 1]);
                            rough /= double(std::max<std::size_t>(start.size() - 1, 1));
                            if (std::abs(tail.back() - start.front()) > 8 * rough + 64) ++seam_clicks;
                            if (!snd.xa && snd.loop_start) {
                                // The SPU loop follows the frame flags; the encoder's tags sit one frame earlier.
                                (d.head.size() == *snd.loop_start + kAdpcmFrameSamples ? flag_offset_ok : flag_offset_other)++;
                            }
                        }
                    } catch (const std::exception& e) {
                        fail(stem + " " + p + " slot " + std::to_string(snd.slot), e);
                    }
                }
                for (const auto& e : index->entries()) {
                    ++names;
                    if (!bank->find(std::size_t(e.slot))) ++dangling_names;
                }
            }
            for (const auto& f : m.mix_files()) presets += m.mix_presets(f).size();
            // The engine layers of every level resolve in its Engine bank (a bank may lack some layers).
            // Levels of other missions (whose banks this VIV does not ship) are skipped: the union check
            // above already proved the bank exists on the disc.
            for (const auto& [level, refs] : m.banks_ini().sections)
                for (const auto& r : refs) {
                    if (r.role != "Engine") continue;
                    if (!m.viv().find(std::string("data\\audio\\") + r.path)) continue;
                    auto index = m.bank_index(r.path);
                    std::size_t found = 0;
                    for (const char* name : {"SFX_IN-idle", "SFX_OUT-idle", "SFX_IN-lowload", "SFX_OUT-lowload", "SFX_IN-hiload",
                                             "SFX_OUT-hiload", "SFX_IN-cruz", "SFX_OUT-cruz"})
                        found += index->lookup(name).has_value();
                    // Special vehicles (the uw_mis11 submarine, the snowmobile) carry their own layer
                    // names instead of the car set; the mixer stays silent on the missing voices.
                    // A partial car set means a broken bank.
                    if (found == 0) continue;
                    if (found != 8) throw FormatError("level " + level + ": engine bank " + r.path + " lacks idle/lowload/hiload/cruz layers");
                    ++engines;
                }
        } catch (const std::exception& e) {
            fail("driving " + stem, e);
        }
        // Streams: block chain, frame counts, waveform sanity.
        try {
            DrivingMission m(*dir, stem);
            auto check_archive = [&](BigArchive& a, bool is_music) {
                for (const BigEntry& e : a.entries()) {
                    try {
                        const EaStream s = DrivingMission::read_stream(a, e);
                        Signal sig;
                        std::vector<std::int16_t> buf;
                        std::int32_t prev = 0;
                        double rough = 0;
                        std::vector<std::int32_t> first_of_block;
                        std::vector<std::int32_t> last_of_block;
                        for (std::size_t b = 0; b < s.block_count(); ++b) {
                            buf.clear();
                            s.decode_block(b, buf);
                            sig.add(buf.data(), buf.size());
                            const std::size_t ch = s.info().channels;
                            for (std::size_t k = ch; k < buf.size(); k += ch) rough += std::abs(buf[k] - buf[k - ch]);
                            if (b > 0) {
                                ++stream_block_seams;
                                first_of_block.push_back(buf[0]);
                                last_of_block.push_back(prev);
                            }
                            prev = buf[buf.size() - ch];
                        }
                        rough /= double(std::max<std::size_t>(sig.samples / s.info().channels, 2) - 1);
                        for (std::size_t k = 0; k < first_of_block.size(); ++k)
                            if (std::abs(first_of_block[k] - last_of_block[k]) > 8 * rough + 64) ++stream_block_clicks;
                        stream_blocks += s.block_count();
                        const bool quiet_by_design = e.path.find("silence") != std::string::npos;
                        if (!quiet_by_design && sig.peak < 100) throw FormatError("decodes to near silence");
                        ++streams;
                        (is_music ? music : speech)++;
                        (is_music ? music_seconds : speech_seconds) += s.duration_seconds();
                    } catch (const std::exception& ex) {
                        fail(stem + " " + e.path, ex);
                    }
                }
            };
            if (m.has_music()) check_archive(*m.music(), true);
            for (const auto& lang : m.speech_languages()) {
                auto sp = m.open_speech(lang);
                check_archive(*sp, false);
            }
        } catch (const std::exception& e) {
            fail("driving streams " + stem, e);
        }
    }
    std::printf("driving audio: %zu missions, %zu distinct banks with %zu sounds (%zu SPU ADPCM, %zu EA-XA; %zu looping, "
                "%zu with a step at the loop seam, %zu silent), %.0f s\n",
                missions, banks, sounds, spu, xa, looping, seam_clicks, silent, bank_seconds);
    std::printf("driving audio: SPU loop frames sit one frame after the tag loop start in %zu of %zu SPU loops; %zu names in the "
                ".h indices (%zu without a sound), %zu mix presets, %zu engine banks with all layers\n",
                flag_offset_ok, flag_offset_ok + flag_offset_other, names, dangling_names, presets, engines);
    std::printf("driving audio: %zu streams (%zu music %.0f s, %zu speech/NIS %.0f s), %zu data blocks, %zu block seams of which %zu "
                "with a step above the local roughness\n",
                streams, music, music_seconds, speech, speech_seconds, stream_blocks, stream_block_seams, stream_block_clicks);
    return failures;
}

std::size_t validate_sounds(GameFiles& files, const std::filesystem::path& gamedir) {
    return validate_action_sounds(files, gamedir) + validate_driving_audio(gamedir);
}

}  // namespace nf
