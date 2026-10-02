#include "assets/menu_messages.hpp"

#include <algorithm>
#include <initializer_list>

namespace nf {

namespace {

using namespace menu_msg;

bool in(std::uint32_t v, std::initializer_list<std::uint32_t> set) { return std::find(set.begin(), set.end(), v) != set.end(); }

// Messages every control type handles (Script_* and the common getters).
bool common(std::uint32_t t) { return in(t, {kExitLoop, kPlayScript, kAttachScript, kGetControl, kGetUserIndex}); }

// Label_SendMessage, shared by labels, buttons and radios.
bool label_message(std::uint32_t t) {
    return in(t, {kSetText, kSetColor, kSetSelectedColor, kSetNormalColor, kSetFormat, kSetSprite, kSetOutlineColor, kSetUv,
                  kOutline, kPulse, 0x76, kGetColor});
}

}  // namespace

bool menu_message_understood(std::uint8_t type, std::uint32_t m) {
    if (m == 0x1D) return true;   // ignored while the script loads
    if (common(m)) return true;
    if (m == kSetFlags) return ControlType(type) != ControlType::Window;
    switch (ControlType(type)) {
        case ControlType::Label: return label_message(m);
        case ControlType::Button: return label_message(m) || m == kSetUserIndex;
        case ControlType::Radio:
            return label_message(m) || in(m, {kAddItem, kAddItemLabel, kClear, kSetIndex, kSelectValue, kSetUserIndex});
        case ControlType::Scroll:
            return in(m, {kScrollGrow, kScrollWrap, kScrollMax, kScrollMin, kScrollRange, kScrollSet, 0x69, kSetUserIndex});
        case ControlType::Memo:
            return in(m, {kSetText, kSetSelectedColor, kSetNormalColor, kSetFormat, 100, 0x66, kLineHeight});
        case ControlType::List:
            return in(m, {kAddItem, kAddItemLabel, kClear, kSetIndex, kSelectValue, kSetRowValue, kSetSelectedColor,
                          kSetNormalColor, kSetOutlineColor, kSetFormat, 0x0E, kListColumnWidth, 0x59, kListSetCell, kListScrollTop,
                          kListColumns, kLineHeight});
        case ControlType::Window: return false;
        default: return false;
    }
}

bool menu_manager_message_understood(std::uint32_t m) {
    return in(m, {kSelectControl, kChangePage, kPageBack, kInputLock, kSetNavigation});
}

const std::vector<MenuHandlerId>& menu_handler_ids() {
    static const std::vector<MenuHandlerId> ids = {
        {0x40000002, "P_MAIN"},         {0x40000009, "P_START"},        {0x40000012, "P_MPOPTIONS"},
        {0x40000013, "P_MPMAP"},        {0x40000014, "P_MPRULES"},      {0x40000017, "P_MPPLAYERMODS"},
        {0x40000019, "P_MPJOIN"},       {0x4000001A, "P_MPSCENARIO"},   {0x40000027, "P_MPBOTS"},
        {0x40000028, "P_MPENVIROMODS"}, {0x4000002C, "P_MPBOTSETUP"},   {0x40000032, "P_INTRO"},
        {0x40000034, "P_LANGUAGE"},     {0x4000003F, "P_MPBOTCHOOSE"},  {0x40000043, "P_ESTHERO"},
        {0x40000048, "P_PS2MEMCARDINIT"}, {0x40000049, "P_MPCONFIRM"},  {0x4000004A, "P_PARISENUM"},
        {0x4000004B, "P_PAUSE"},        {0x40000051, "P_MPSETUP"},      {0x10000002, "C_GONIGHTFIRE"},
        {0x10000003, "C_GOMULTIPLAYER"}, {0x10000004, "C_GOCODENAMES"}, {0x10000006, "C_SBMPMAP"},
        {0x10000028, "C_GCPAUSE"},      {0x1000009C, "C_SBMPSCEN"},     {0x100000F4, "C_SBMPOPTIONS"},
        {0x10000114, "C_SBBOTS"},       {0x10000120, "C_LBMSGOPTIONS"}, {0x1000015E, "C_LANGUAGE"},
        {0x10000193, "C_SBMPBTCHOOSE"}, {0x1000019F, "C_RBMPSTART"},    {0x100001A1, "C_RBMPSETUP"},
        {0x1000022A, "C_RBMPFINISH"},   {0x1000022B, "C_RBMPCNAME"},
    };
    return ids;
}

const char* menu_handler_name(std::uint32_t id) {
    for (const MenuHandlerId& h : menu_handler_ids())
        if (h.id == id) return h.name;
    return nullptr;
}

}  // namespace nf
