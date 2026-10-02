// Page/control animation scripts: keyframes on the control rectangle plus messages
// (Script_Play*, Script_RunFrame, Script_InterpolateLine/Spline, Process_*).
#include <algorithm>
#include <cmath>

#include "ui/menu.hpp"

namespace nf::ui {

namespace {

// Script slots (control+0x7c + 4*slot) and the events that start them (Script_PlayDefault).
int slot_for_event(std::uint32_t ev) {
    switch (ev) {
        case 0x49: return 3;
        case 0x4B: case 0x63: return 2;
        case 0x4C: case 0x51: return 0;
        case 0x4D: case 0x4E: return 1;
        case 0x4F: return 4;
        case 0x5B: return 5;
        default: return -1;
    }
}

// Catmull-Rom through the four keyframe values around segment i (Script_CalculateSpline).
float catmull(float t, float p0, float p1, float p2, float p3) {
    const float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2 * p1) + (-p0 + p2) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 + (-p0 + 3 * p1 - 3 * p2 + p3) * t3);
}

}  // namespace

bool MenuManager::script_start(Control& c, std::uint32_t script_id, bool fast_forward) {
    const MenuScript* s = nullptr;
    for (const MenuScript& sc : c.def->scripts)
        if (sc.id == script_id) s = &sc;
    if (!s) return false;
    ScriptRun& r = c.run;
    r = ScriptRun{};
    r.script = s;
    r.flags = s->param;
    if (s->keyframes.empty()) return true;
    r.frame = 0;
    r.index = -1;
    r.next = 0;
    r.loop_keyframe = 0;
    if (fast_forward) {   // Script_Play with the fast-forward flag: jump to the last keyframe
        r.next = int(s->keyframes.size()) - 1;
        r.loop_keyframe = r.next;
        r.index = int(s->keyframes.size()) - 1;
        r.frame = s->keyframes.back().v[0];
    }
    return true;
}

bool MenuManager::script_play_default(Control& c, std::uint32_t event) {
    const int slot = slot_for_event(event);
    if (slot < 0) return false;
    const bool page_event = event == 0x4C || event == 0x4D || event == 0x63;
    Control* target = &c;
    if (page_event) {
        if (c.type != 8) return false;
        target = c.page.window();
        if (!target) return false;
    } else if (c.type == 13) {
        return false;
    }
    const MenuScript* s = target->slots[std::size_t(slot)];
    if (!s) return false;
    script_start(*target, s->id, false);
    return true;
}

void MenuManager::interpolate(Control& c, const ScriptRun& run, float t) {
    const auto& kf = run.script->keyframes;
    const int n = int(kf.size());
    if (n == 0) return;
    float ft = std::max(0.0f, std::min(t, float(n - 1)));
    const int i0 = int(ft);
    const float f = ft - float(i0);
    auto clampi = [&](int i) { return std::size_t(std::clamp(i, 0, n - 1)); };
    const std::size_t a = clampi(i0 - 1), b = clampi(i0), cc = clampi(i0 + 1), d = clampi(i0 + 2);
    const bool spline = (run.flags & 1) != 0;
    auto val = [&](int comp) {
        const float p1 = kf[b].v[comp], p2 = kf[cc].v[comp];
        if (!spline) return p1 + (p2 - p1) * f;
        return catmull(f, kf[a].v[comp], p1, p2, kf[d].v[comp]);
    };
    c.x = int(val(1)), c.y = int(val(2)), c.w = int(val(3)), c.h = int(val(4));
}

void MenuManager::send_keyframe_message(Control& c, ScriptRun& run, const MenuKeyframeMessage& m, int frames_left) {
    std::uint32_t a = m.a;
    if (a == 0xFFFFFFFC) a = run.result;
    const std::uint32_t target = m.target;
    if (m.type >= 0xE1) {
        Control* t = target == 0xFFFFFFFE ? &c : find(target);
        if (t) fade(*t, a, frames_left, m.b != 0);
        return;
    }
    int r = 0;
    if (target == 0xFFFFFFFD) {
        r = send_manager(m.type, a, m.b);
    } else if (target == 0xFFFFFFFE) {
        r = send_to(c, Msg{m.type, a, m.b});
    } else {
        r = send(target, m.type, a, m.b);
    }
    run.result = std::uint32_t(r);
}

void MenuManager::fade(Control& c, std::uint32_t target, int frames, bool flag) {
    Process p;
    p.target = target;
    p.start = c.label.color;
    p.duration = std::max(frames, 1);
    p.flag = flag;
    c.processes.push_back(p);
}

void MenuManager::run_processes(Control& c) {
    for (std::size_t i = 0; i < c.processes.size();) {
        Process& p = c.processes[i];
        auto mix = [&](int shift) {
            const float s = float((p.start >> shift) & 0xFF), t = float((p.target >> shift) & 0xFF);
            return std::uint32_t(int(s + (t - s) / float(p.duration) * float(p.elapsed))) & 0xFF;
        };
        c.label.color = mix(24) << 24 | mix(16) << 16 | mix(8) << 8 | mix(0);
        ++p.elapsed;
        if (p.elapsed >= p.duration) {
            c.label.color = p.target;
            if (!p.flag) {
                c.label.color_selected = c.label.color_normal = p.target;
            }
            c.processes.erase(c.processes.begin() + long(i));
        } else {
            ++i;
        }
    }
}

// Script_RunFrame: one 30 Hz step of the control's current script.
void MenuManager::run_script_frame(Control& c) {
    run_processes(c);
    ScriptRun& r = c.run;
    if (!r.script || r.next < 0) return;
    const auto& kfs = r.script->keyframes;
    const MenuKeyframe& kf = kfs[std::size_t(r.next)];
    const MenuKeyframe& prev = r.next > 0 ? kfs[std::size_t(r.next) - 1] : kf;
    float t;
    if (kf.v[0] == prev.v[0]) {
        t = float(r.index + 1);
    } else {
        t = float(r.index) + float(r.frame - prev.v[0]) / float(kf.v[0] - prev.v[0]);
    }
    interpolate(c, r, t);
    if (r.frame < kf.v[0]) {
        ++r.frame;
        return;
    }
    const std::uint32_t kf_flags = kf.tail;
    if (kf_flags & 2) {
        r.loop_keyframe = r.next;
        r.loop_index = r.index;
        r.flags |= 8;
    }
    const int reached = r.next;
    if ((kf_flags & 4) && (r.flags & 8)) {
        r.index = r.loop_index;
        r.next = r.loop_keyframe;
        r.frame = kfs[std::size_t(r.next)].v[0];
    } else {
        r.next = reached + 1 < int(kfs.size()) ? reached + 1 : -1;
        r.index = r.next < 0 ? 0 : r.index + 1;
        const int frames_left = r.next >= 0 ? kfs[std::size_t(r.next)].v[0] - r.frame - 1 : 0xF;
        for (const MenuKeyframeMessage& m : kf.messages) send_keyframe_message(c, r, m, frames_left);
    }
    if (r.script) ++r.frame;
}

}  // namespace nf::ui
