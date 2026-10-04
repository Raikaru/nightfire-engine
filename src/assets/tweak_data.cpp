// Developer menu tables: TWEAKS/TWEAKS2 captions + scroll boot values and the C_NIS list.
// Control ids and scales come from P_TWEAKS/P_TWEAKS2/C_NIS_Handler; every string and float is
// read from ACTION.ELF (nm-verified symbols), so a data change throws instead of mis-showing.
#include "assets/tweak_data.hpp"

#include <cstring>
#include <cmath>

namespace nf {

namespace {

// P_TWEAKS 0x4c label texts (control -> .rodata address of the caption). Control 0x1b8 reads the
// RAM scratch 0x30d320 whose boot content is unknowable, so it keeps the script text.
constexpr std::pair<std::uint32_t, std::uint32_t> kTweakLabels[] = {
    {0x100001DE, 0x308CB0}, {0x100001A6, 0x308CD8}, {0x100001A7, 0x308CE8}, {0x100001A8, 0x308CF8},
    {0x100001A9, 0x308D08}, {0x100001AA, 0x308D18}, {0x100001AB, 0x308D28}, {0x100001AC, 0x308D38},
    {0x100001AD, 0x308D48}, {0x100001AE, 0x308D58}, {0x100001AF, 0x308D68}, {0x100001B0, 0x308D78},
    {0x100001B2, 0x308D88}, {0x100001B3, 0x308D98}, {0x100001B4, 0x308DA8}, {0x100001B5, 0x308DB8},
    {0x100001B6, 0x308DC8}, {0x100001B7, 0x308DD8},
    // P_TWEAKS2 0x4c captions.
    {0x100001EA, 0x308DE8}, {0x100001DF, 0x308E08}, {0x100001E0, 0x308E18}, {0x100001E1, 0x308E30},
    {0x100001E2, 0x308E40}, {0x100001E3, 0x308E50}, {0x100001FE, 0x308E60}, {0x10000200, 0x308E70},
    {0x10000201, 0x308E80}, {0x10000205, 0x308E90}, {0x10000206, 0x308EA8}, {0x10000207, 0x308EC0},
    {0x10000208, 0x308ED8}, {0x1000020F, 0x308EF0}, {0x10000210, 0x308F08},
};

// P_TWEAKS/P_TWEAKS2 scrolls (control, boot global, display scale: shown = int(value * scale),
// stored back as shown / scale on accept). Store targets are the live copies the game owns
// (fGpffff8440.. for damage, DroneFiring_* directly for the rest); see docs/ui.md.
struct TweakScroll {
    std::uint32_t control;
    const char* symbol;
    float scale;
};
constexpr TweakScroll kTweakScrolls[] = {
    {0x100001A6, "DroneDamage_Easy", 2},   {0x100001A7, "DroneDamage_Normal", 2},
    {0x100001A8, "DroneDamage_Hard", 2},   {0x100001A9, "DroneDamage_Head", 4},
    {0x100001AA, "DroneDamage_Arms", 4},   {0x100001AB, "DroneDamage_Legs", 4},
    {0x100001AC, "DroneDamage_Torso", 4},  {0x100001AD, "DroneArmour_Helmet", 5},
    {0x100001AE, "DroneArmour_Combat", 5}, {0x100001AF, "DroneArmour_Jacket", 5},
    {0x100001B0, "DroneArmour_Vest", 5},   {0x100001B2, "Plr_DMod_Multi", 10},
    {0x100001B8, "Plr_DMod_Head", 10},     {0x100001B3, "Plr_DMod_LowerLimb", 10},
    {0x100001B4, "Plr_DMod_UpperLimb", 10}, {0x100001B5, "Plr_DMod_Easy", 20},
    {0x100001B6, "Plr_DMod_Normal", 20},   {0x100001B7, "Plr_DMod_Hard", 20},
    {0x100001DF, "DroneFiring_BurstDelay_Min", 1},     {0x100001E0, "DroneFiring_BurstDelay_Normal", 1},
    {0x100001E1, "DroneFiring_BurstDelay_Max", 1},     {0x100001E2, "DroneFiring_BurstDelay_MinDist", 1},
    {0x100001E3, "DroneFiring_BurstDelay_MaxDist", 1}, {0x100001FE, "DroneFiring_Accuracy_Easy", 4},
    {0x10000200, "DroneFiring_Accuracy_Normal", 4},    {0x10000201, "DroneFiring_Accuracy_Hard", 4},
    {0x10000205, "DroneFiring_NewSighting_TimeToHit", 4},
    {0x10000206, "DroneFiring_TargetFirstMoved_TimeToHit", 4},
    {0x10000207, "DroneFiring_TargetFirstMoved_Accuracy", 4},
    {0x10000208, "DroneFiring_TargetMoving_Accuracy", 4},
    {0x1000020F, "DroneFiring_TooClose_Distance", 4},  {0x10000210, "DroneFiring_TooClose_Accuracy", 4},
};

// P_TWEAKS/P_TWEAKS2 value readouts (scroll -> label): the 0x50 tick prints shown / scale.
// Order matches the handlers' read/write sequences.
constexpr std::pair<std::uint32_t, std::uint32_t> kTweakValues[] = {
    {0x100001A6, 0x100001B9}, {0x100001A7, 0x100001BA}, {0x100001A8, 0x100001CA},
    {0x100001A9, 0x100001BB}, {0x100001AA, 0x100001BD}, {0x100001AB, 0x100001BC},
    {0x100001AC, 0x100001BE}, {0x100001AD, 0x100001BF}, {0x100001AE, 0x100001C0},
    {0x100001AF, 0x100001C1}, {0x100001B0, 0x100001C2}, {0x100001B2, 0x100001C3},
    {0x100001B8, 0x100001C4}, {0x100001B3, 0x100001C5}, {0x100001B4, 0x100001C6},
    {0x100001B5, 0x100001C7}, {0x100001B6, 0x100001C8}, {0x100001B7, 0x100001C9},
    {0x100001DF, 0x100001E4}, {0x100001E0, 0x100001E6}, {0x100001E1, 0x100001E7},
    {0x100001E2, 0x100001E8}, {0x100001E3, 0x100001E9}, {0x100001FE, 0x100001FF},
    {0x10000200, 0x10000202}, {0x10000201, 0x10000203}, {0x10000205, 0x10000209},
    {0x10000206, 0x1000020A}, {0x10000207, 0x1000020B}, {0x10000208, 0x1000020C},
    {0x1000020F, 0x10000211}, {0x10000210, 0x10000212},
};

// C_NIS_Handler 0x51 rows in order (.rodata name address, script hash; 0 = header row).
constexpr std::pair<std::uint32_t, std::uint32_t> kNisRows[] = {
    {0x308840, 0}, {0x308850, 0x6000074}, {0x308860, 0x60008C8}, {0x308870, 0x6000952}, {0x308880, 0},
    {0x308898, 0x6000071}, {0x3088B0, 0}, {0x3088C8, 0x6000073}, {0x3088D8, 0x6000072}, {0x3088E8, 0},
    {0x308900, 0x6000077}, {0x308918, 0x6000078}, {0x308928, 0x6000076}, {0x308938, 0x6000991},
    {0x308948, 0}, {0x308958, 0x6000087}, {0x308968, 0x6000086}, {0x308978, 0x600065D}, {0x308990, 0},
    {0x3089A0, 0x600007F}, {0x3089B0, 0x6000084}, {0x3089C8, 0}, {0x3089D8, 0x6000080}, {0x3089E8, 0},
    {0x3089F8, 0x6000081}, {0x308A08, 0x6000082}, {0x308A18, 0x6000083}, {0x308978, 0x600065D},
    {0x308A28, 0x60004F8}, {0x308A40, 0}, {0x308A58, 0x600098A}, {0x308A28, 0x60004F8},
    {0x308A70, 0x60004FA}, {0x308A88, 0x60004F9}, {0x308AA0, 0x60004FB}, {0x308AB8, 0},
    {0x308AC8, 0x6000726}, {0x308AE0, 0}, {0x308AF0, 0x600068F}, {0x308B00, 0x600073A}, {0x308B10, 0},
    {0x308A58, 0x600098A}, {0x308B20, 0x6000655}, {0x308B38, 0}, {0x308A58, 0x600098A},
    {0x308B48, 0}, {0x308B58, 0}, {0x308B68, 0}, {0x308B78, 0x6000646}, {0x308B88, 0x60006D7},
    {0x308B98, 0}, {0x308BA8, 0x60006CF}, {0x308BB8, 0x60006D3}, {0x308BD0, 0}, {0x308BE0, 0x600062F},
    {0x308BF8, 0x6000833}, {0x308C08, 0x6000848}, {0x308C18, 0}, {0x308C28, 0}, {0x308C38, 0x6000631},
    {0x308C48, 0x6000632}, {0x308C58, 0x6000633}, {0x308C68, 0}, {0x308C78, 0x600085C},
    {0x308C88, 0x600085F}, {0x308CA0, 0x600098F},
};

std::string elf_cstr(const Elf32& elf, std::uint32_t addr) {
    const Bytes bytes = elf.at(addr, 256);
    std::string out;
    for (std::uint8_t b : bytes) {
        if (b == 0) break;
        out.push_back(char(b));
    }
    if (out.size() == 256) throw FormatError("unterminated ELF string");
    return out;
}

}  // namespace

TweakData load_tweak_data(const Elf32& action_elf) {
    TweakData data;
    for (const auto& [control, addr] : kTweakLabels) data.labels[control] = elf_cstr(action_elf, addr);
    for (const auto& s : kTweakScrolls) {
        const auto sym = action_elf.symbol(s.symbol);
        if (!sym) throw FormatError(std::string("ACTION.ELF has no ") + s.symbol);
        const Bytes bytes = action_elf.at(sym->value, 4);
        float value = 0;
        std::memcpy(&value, bytes.data(), sizeof(value));
        data.scrolls[s.control] = int(std::lround(value * s.scale));
        data.scales[s.control] = s.scale;
    }
    for (const auto& [addr, script] : kNisRows) data.nis.push_back({elf_cstr(action_elf, addr), script});
    for (const auto& [scroll, readout] : kTweakValues) data.values[scroll] = readout;
    return data;
}

}  // namespace nf
