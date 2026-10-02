#pragma once

#include <cstdint>
#include <vector>

#include "assets/menu_file.hpp"

namespace nf {

// Message ids of the front-end control protocol (`Manager_SendMessage`, `Page_SendMessage`, the
// `*_SendMessage` of each control type). Numbers are the original's; the comment names the sender's
// intent, see docs/ui.md "Front-end runtime" for the per-type behaviour.
namespace menu_msg {
constexpr std::uint32_t kAddItem = 0x10;         // radio/list: add a row {text a, value b}
constexpr std::uint32_t kAddItemLabel = 0x11;    // same with a label hash (Txt_BindLabel)
constexpr std::uint32_t kScrollGrow = 0x12;      // scroll: max += a
constexpr std::uint32_t kListRemove = 0x13;      // list: remove row a
constexpr std::uint32_t kExitLoop = 0x14;        // script: leave the loop keyframe
constexpr std::uint32_t kScrollWrap = 0x15;      // scroll: no wrap flag
constexpr std::uint32_t kPlayScript = 0x16;      // play script a
constexpr std::uint32_t kClear = 0x17;           // radio/list: remove every row
constexpr std::uint32_t kSetText = 0x18;         // label: a = 0 -> label hash b; else literal a
constexpr std::uint32_t kListSetCell = 0x19;     // list: cell text
constexpr std::uint32_t kSetColor = 0x1a;        // label: both colours
constexpr std::uint32_t kSetSelectedColor = 0x1b;
constexpr std::uint32_t kSetNormalColor = 0x1c;
constexpr std::uint32_t kSetIndex = 0x1d;        // radio/list: select row a
constexpr std::uint32_t kSelectValue = 0x1e;     // radio/list: select the row whose value is a
constexpr std::uint32_t kSetRowValue = 0x1f;     // list: value of the current row
constexpr std::uint32_t kSetUserIndex = 0x20;    // button: control+0x20; radio: value of the current row
constexpr std::uint32_t kAttachScript = 0x21;    // slot a := script b
constexpr std::uint32_t kSelectControl = 0x22;   // manager: move the cursor onto control a
constexpr std::uint32_t kSetFormat = 0x23;       // text format string
constexpr std::uint32_t kSetSprite = 0x24;       // label: image sprite hash a
constexpr std::uint32_t kScrollMax = 0x25;
constexpr std::uint32_t kScrollMin = 0x26;
constexpr std::uint32_t kScrollRange = 0x27;     // a = min, b = max
constexpr std::uint32_t kListColumnWidth = 0x28;
constexpr std::uint32_t kSetOutlineColor = 0x29;
constexpr std::uint32_t kListColumns = 0x2a;
constexpr std::uint32_t kSetFlags = 0x2b;        // control state byte (bit0 hidden)
constexpr std::uint32_t kListScrollTop = 0x2c;
constexpr std::uint32_t kSetUv = 0x2d;           // label: a = u<<16|v, b = (w-1)<<16|(h-1)
constexpr std::uint32_t kScrollSet = 0x2e;       // scroll: value a (notifies the parent with 0x54)
constexpr std::uint32_t kScrollSlider = 0x69;    // scroll: slider mode (no wrap, arrow stepping)
constexpr std::uint32_t kGetText = 0x2f;
constexpr std::uint32_t kGetColor = 0x30;
constexpr std::uint32_t kGetCount = 0x33;
constexpr std::uint32_t kGetIndex = 0x34;
constexpr std::uint32_t kGetValue = 0x35;
constexpr std::uint32_t kScrollGetMax = 0x38;
constexpr std::uint32_t kGetControl = 0x39;
constexpr std::uint32_t kScrollGetMin = 0x3a;
constexpr std::uint32_t kGetUserIndex = 0x3d;
constexpr std::uint32_t kScrollGet = 0x40;
constexpr std::uint32_t kCreateControl = 0x42;
constexpr std::uint32_t kCreatePage = 0x43;
constexpr std::uint32_t kChangePage = 0x44;      // manager: a = page id, b = flags (1 no push, 2 overlay, 4 keep history, 8 silent)
constexpr std::uint32_t kSetOverlayPage = 0x45;
constexpr std::uint32_t kDelete = 0x47;
constexpr std::uint32_t kValueChanged = 0x49;    // radio/scroll/list -> page -> handler (b = 1 by the user)
constexpr std::uint32_t kPageShown = 0x4c;
constexpr std::uint32_t kPageHidden = 0x4d;
constexpr std::uint32_t kSelected = 0x4e;        // control became the cursor's
constexpr std::uint32_t kDeselected = 0x4f;
constexpr std::uint32_t kIdle = 0x50;            // page: every update while it is current
constexpr std::uint32_t kAccept = 0x4b;
constexpr std::uint32_t kControlDone = 0x51;
constexpr std::uint32_t kMemoBack = 0x52;
constexpr std::uint32_t kScrollChanged = 0x54;
constexpr std::uint32_t kSetNavigation = 0x56;   // page: 1 both axes, 2 vertical, 3 horizontal
constexpr std::uint32_t kScriptEvent = 0x5b;
constexpr std::uint32_t kBack = 0x5d;
constexpr std::uint32_t kAlt = 0x5e;
constexpr std::uint32_t kPageBack = 0x5f;        // manager: pop the history
constexpr std::uint32_t kRepeat = 0x61;
constexpr std::uint32_t kPageBackScript = 0x63;
constexpr std::uint32_t kOutline = 0x62;
constexpr std::uint32_t kInputLock = 0x68;       // manager: a != 0 disables input
constexpr std::uint32_t kBackVeto = 0x6b;        // handlers may cancel a triangle "back"
constexpr std::uint32_t kLabelYOffset = 0x6c;
constexpr std::uint32_t kAltOnControl = 0x6d;
constexpr std::uint32_t kFadeInPage = 0x6e;
constexpr std::uint32_t kPageMinDelay = 0x6f;    // page: control+0xcc, frames before input is accepted
constexpr std::uint32_t kPulse = 0x70;
constexpr std::uint32_t kGetPageId = 0x74;
constexpr std::uint32_t kLineHeight = 0x75;      // memo: percent
constexpr std::uint32_t kProcessFade = 0xE1;     // keyframe/process message: fade the colour to a over the process duration
}  // namespace menu_msg

// Whether the runtime implements message `type` for a control of `control_type` (used by
// validate_menu to report unknown ids in the menu script). 0x1D is dropped by MenuManager_Create.
bool menu_message_understood(std::uint8_t control_type, std::uint32_t type);
// Messages a keyframe may address to the manager (target 0xFFFFFFFD).
bool menu_manager_message_understood(std::uint32_t type);

// Page and control ids the original dispatches to a handler (Handler_HandleMessage) and that the runtime
// implements, with the original function name. The frontend registers exactly these.
struct MenuHandlerId {
    std::uint32_t id;
    const char* name;
};
const std::vector<MenuHandlerId>& menu_handler_ids();
const char* menu_handler_name(std::uint32_t id);   // nullptr when not implemented

}  // namespace nf
