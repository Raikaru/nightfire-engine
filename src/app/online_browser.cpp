#include "app/online_browser.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "app/menu_background.hpp"
#include "app/server_browser.hpp"
#include "game/input.hpp"
#include "ui/menu_chrome.hpp"

namespace nf::app {
namespace {

using namespace ui::menu_style;

enum class BrowserPage { Servers, Address, Password, LocalPlayers };

struct KeyboardCursor {
    int row = 0;
    int column = 0;
};

// The codename keyboard (P_CNCREATE 0x40000020): rows of up to ten keys on a 40-unit pitch, shorter
// rows centred, then the delete / space / done buttons. The characters are those an IPv4 address,
// port or password needs; SDL text input types anything else.
constexpr std::array<std::string_view, 4> kKeyRows{"0123456789", "abcdefghij", "klmnopqrst", "uvwxyz.:"};
constexpr int kKeyColumns = 10;
constexpr int kActionRow = int(kKeyRows.size());
// The key-grid column each action button sits under (script x 204, 290..350, 404).
constexpr std::array<int, 3> kActionColumns{2, 4, 7};

int row_offset(int row) { return (kKeyColumns - int(kKeyRows[std::size_t(row)].size())) / 2; }

// The script's keyboard boxes moved up 33 units so the visible name field (the skin centres its 23-unit
// bar in the 42-unit box) starts on the content top, the server list panel's top edge; then right by
// `dx`. Keys move 5 more units right: the script's grid (x 120..512) sits 5 units left of its frame's
// centre (x 117..524).
constexpr int kKeyboardRise = 33, kKeyNudge = 5;
ui::Rect keyboard_box(int x, int y, int w, int h, float dx) {
    ui::Rect r = ui::MenuChrome::authored(x, y - kKeyboardRise, w, h);
    r.x += dx;
    return r;
}

std::string map_title(const AppContext& ctx, const std::string& bin) {
    const auto found = std::find_if(ctx.mp_data.maps.begin(), ctx.mp_data.maps.end(),
                                    [&](const MpMap& map) { return map.bin_name == bin; });
    return found == ctx.mp_data.maps.end() ? bin : std::string(ctx.assets.strings.label(found->item.name));
}

std::string mode_title(const AppContext& ctx, std::uint32_t mode) {
    const auto found = std::find_if(ctx.mp_data.scenarios.begin(), ctx.mp_data.scenarios.end(),
                                    [&](const MpScenario& scenario) { return scenario.item.value == mode; });
    return found == ctx.mp_data.scenarios.end() ? "Multiplayer" :
                                                  std::string(ctx.assets.strings.label(found->item.name));
}

std::string server_name(const ServerBrowserEntry& entry) {
    return entry.name.empty() ? "Nightfire Server" : entry.name;
}

// "2 of 4 agents, 1 bot, 15 ms" (+ ". Password required.").
std::string server_summary(const ServerBrowserEntry& entry) {
    std::string text = std::to_string(entry.players) + " of " + std::to_string(entry.max_players) + " agents, " +
                       std::to_string(entry.bots) + (entry.bots == 1 ? " bot, " : " bots, ") +
                       std::to_string(entry.ping_ms) + " ms";
    return entry.password_required ? text + ". Password required." : text;
}

// Two centred description lines in a box (the scenario page's description memo).
void description(ui::MenuChrome& page, ui::Rect box, std::string_view first, std::string_view second) {
    const float middle = box.y + box.h * 0.5f;
    page.label({box.x, middle - kRowPitch, box.w, kRowPitch}, first, 2, ui::Align::Center, kLabelColor);
    page.label({box.x, middle, box.w, kRowPitch}, second, 2, ui::Align::Center, kLabelColor);
}

// A P_MPJOIN panel with "key  value" rows: keys in the button colour, values in one column after the
// widest key (a key with no value is a full-width line of text).
void detail_panel(ui::MenuChrome& page, ui::Rect box, std::string_view header,
                  const std::vector<std::pair<std::string, std::string>>& rows) {
    page.panel(box, header);
    const float left = box.x + 20.0f, width = box.w - 40.0f;
    float key_width = 0;
    for (const auto& [key, value] : rows)
        if (!value.empty()) key_width = std::max(key_width, page.text_width(key, 2) + 16.0f);
    float y = box.y + ui::MenuChrome::panel_header_height() + 6.0f;
    for (const auto& [key, value] : rows) {
        if (value.empty()) {
            page.label({left, y, width, kRowPitch}, page.clip(key, 2, width), 2, ui::Align::Left, kLabelColor);
        } else {
            page.label({left, y, key_width, kRowPitch}, key, 2, ui::Align::Left, kItemColor);
            page.label({left + key_width, y, width - key_width, kRowPitch}, page.clip(value, 2, width - key_width), 2,
                       ui::Align::Left, kLabelColor);
        }
        y += kRowPitch;
    }
}

void draw_keyboard(ui::MenuChrome& page, float dx, std::string_view field, KeyboardCursor cursor) {
    page.skin(kKeyboardSkin, 0, keyboard_box(117, 161, 407, 191, dx));
    page.skin(kFieldSkin, 0, keyboard_box(117, 126, 406, 42, dx));
    page.label(keyboard_box(117, 135, 406, 16, dx), field, 3, ui::Align::Center, kLabelColor);
    for (int row = 0; row < kActionRow; ++row) {
        const std::string_view keys = kKeyRows[std::size_t(row)];
        for (int column = 0; column < int(keys.size()); ++column) {
            const int x = 120 + kKeyNudge + 40 * (column + row_offset(row));
            const bool current = cursor.row == row && cursor.column == column;
            page.label(keyboard_box(x, 161 + 40 * row, 32, 32, dx), keys.substr(std::size_t(column), 1), 1,
                       ui::Align::Center, current ? kLabelColor : kItemColor);
        }
    }
    // The script's action buttons: arrow (delete), long arrow (space) and tick (done) from the icon sheet.
    struct Action {
        int x, y, w, h;
        ui::Rect src;
    };
    constexpr std::array<Action, 3> kActions{{{204, 323, 25, 16, {103, 51, 19, 16}},
                                              {290, 323, 60, 16, {136, 51, 32, 16}},
                                              {404, 319, 25, 24, {177, 57, 17, 13}}}};
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        const Action& a = kActions[i];
        const bool current = cursor.row == kActionRow && cursor.column == int(i);
        page.sprite(kIcons, keyboard_box(a.x + kKeyNudge, a.y, a.w, a.h, dx), a.src,
                    current ? kLabelColor : kItemColor);
    }
}

void move_keyboard(KeyboardCursor& cursor, int dx, int dy) {
    // Vertical moves keep the on-screen column: rows are centred and the actions sit under given keys.
    const auto visual = [](int row, int column) {
        return row == kActionRow ? kActionColumns[std::size_t(column)] : column + row_offset(row);
    };
    const auto column_at = [](int row, int x) {
        if (row == kActionRow) return x <= 3 ? 0 : x <= 5 ? 1 : 2;
        return std::clamp(x - row_offset(row), 0, int(kKeyRows[std::size_t(row)].size()) - 1);
    };
    if (dy != 0) {
        const int x = visual(cursor.row, cursor.column);
        cursor.row = (cursor.row + dy + kActionRow + 1) % (kActionRow + 1);
        cursor.column = column_at(cursor.row, x);
    }
    const int columns = cursor.row == kActionRow ? int(kActionColumns.size())
                                                 : int(kKeyRows[std::size_t(cursor.row)].size());
    cursor.column = std::clamp(cursor.column + dx, 0, columns - 1);
}

// The server list: a full-width P_MPJOIN panel whose header strip carries the column titles, one row
// per server on the option box's 21-unit pitch, and the scenario page's description box below it.
void draw_servers(const AppContext& ctx, ui::MenuChrome& page, const std::vector<ServerBrowserEntry>& entries,
                  std::size_t selected) {
    page.title(ctx.assets.strings.label(0x29e));   // "Join Game"
    const ui::Rect panel = page.span(103, 187);
    page.panel(panel);
    const float header = ui::MenuChrome::panel_header_height();
    const float inner_x = panel.x + 20.0f, inner_w = panel.w - 40.0f;

    // Numeric columns are as wide as their widest text; the text columns share the rest. The 4:3 list
    // (500 units inside) leaves bots and access to the description box, wider lists show them too.
    constexpr float kGap = 16.0f;   // P_MPJOIN's gutter between panels (script x 312 -> 328)
    const bool wide = inner_w > 560.0f;
    struct Column {
        const char* title;
        ui::Align align;
        float width = 0, x = 0;
    };
    std::vector<Column> columns{{"SERVER", ui::Align::Left}, {"MAP", ui::Align::Left}, {"SCENARIO", ui::Align::Left},
                                {"PLAYERS", ui::Align::Right}};
    if (wide) columns.push_back({"BOTS", ui::Align::Right});
    columns.push_back({"PING", ui::Align::Right});
    if (wide) columns.push_back({"ACCESS", ui::Align::Right});
    std::vector<std::vector<std::string>> rows;
    for (const ServerBrowserEntry& entry : entries) {
        std::vector<std::string>& cells = rows.emplace_back();
        cells = {server_name(entry), map_title(ctx, entry.map), mode_title(ctx, entry.mode),
                 std::to_string(entry.players) + "/" + std::to_string(entry.max_players)};
        if (wide) cells.push_back(std::to_string(entry.bots));
        cells.push_back(std::to_string(entry.ping_ms) + " ms");
        if (wide) cells.push_back(entry.password_required ? "Locked" : "Open");
    }
    float numeric_w = 0;
    for (std::size_t c = 3; c < columns.size(); ++c) {
        columns[c].width = page.text_width(columns[c].title, 2);
        for (const auto& cells : rows) columns[c].width = std::max(columns[c].width, page.text_width(cells[c], 2));
        numeric_w += columns[c].width + kGap;
    }
    const float text_w = inner_w - numeric_w - 2.0f * kGap;
    columns[0].width = text_w * 0.46f;
    columns[1].width = text_w * 0.24f;
    columns[2].width = text_w * 0.30f;
    float x = inner_x;
    for (Column& c : columns) {
        c.x = x;
        x += c.width + kGap;
    }
    for (const Column& c : columns)
        page.label({c.x, panel.y + 2.0f, c.width, 14.0f}, c.title, 2, c.align, kLabelColor);

    const ui::Rect details = page.span(301, 113);
    const float body_top = panel.y + header + 4.0f;
    if (entries.empty()) {
        const float middle = body_top + (panel.y + panel.h - body_top) * 0.5f;
        page.label({panel.x, middle - kRowPitch * 0.5f, panel.w, kRowPitch}, "No servers found", 3,
                   ui::Align::Center, kLabelColor);
        description(page, details, "Servers on your network and the master list appear here.",
                    "Press ~B to search again or ~X to enter an address.");
        return;
    }
    const std::size_t visible = std::size_t((panel.y + panel.h - 4.0f - body_top) / kRowPitch);
    const std::size_t first = selected >= visible ? selected - visible + 1 : 0;
    const std::size_t last = std::min(entries.size(), first + visible);
    for (std::size_t i = first; i < last; ++i) {
        const float y = body_top + float(i - first) * kRowPitch;
        const bool current = i == selected;
        // The selection bar is inset into the panel body, clear of its border.
        if (current) page.selection({panel.x + 8.0f, y + 1.0f, panel.w - 16.0f, kRowPitch - 2.0f});
        const std::uint32_t color = current ? kLabelColor : kItemColor;
        for (std::size_t c = 0; c < columns.size(); ++c)
            page.label({columns[c].x, y, columns[c].width, kRowPitch}, page.clip(rows[i][c], 2, columns[c].width), 2,
                       columns[c].align, color);
    }
    const ServerBrowserEntry& entry = entries[selected];
    description(page, details, server_name(entry) + "  -  " + map_title(ctx, entry.map) + " / " + mode_title(ctx, entry.mode),
                server_summary(entry));
}

// Address and password entry: the codename keyboard on the content top. Wide pages put a detail panel
// beside it; on the 4:3 page the details become the description lines under the keyboard.
void draw_entry(const AppContext& ctx, ui::MenuChrome& page, BrowserPage kind, const std::string& value,
                const std::optional<ServerBrowserEntry>& server, KeyboardCursor cursor) {
    const bool password = kind == BrowserPage::Password;
    page.title(password ? "Enter Server Password" : "Enter Server Address");
    const ui::Rect frame = keyboard_box(117, 161, 407, 191, 0.0f);
    const float top = page.span(103, 187).y, bottom = frame.y + frame.h;
    constexpr float kGutter = 16.0f;   // between P_MPJOIN's side-by-side panels (script x 312 -> 328)
    // The side panel needs 300 units for its longest line ("Add :port when it is not 27500."); narrower
    // pages (4:3, 16:10) use the description lines under the keyboard.
    const bool wide = page.right() - page.left() >= frame.w + kGutter + 300.0f;
    const float dx = wide ? page.left() - frame.x : 0.0f;
    draw_keyboard(page, dx, (password ? std::string(value.size(), '*') : value) + "_", cursor);

    if (wide) {
        // P_MPJOIN pairs equal-width panels: the side panel is at most the keyboard frame's width, so
        // ultrawide pages leave the background open on the right like the ring pages do.
        const float x = frame.x + dx + frame.w + kGutter;
        const ui::Rect side{x, top, std::min(page.right() - x, frame.w), bottom - top};
        if (password && server)
            detail_panel(page, side, "SERVER",
                         {{"Name", server_name(*server)},
                          {"Map", map_title(ctx, server->map)},
                          {"Scenario", mode_title(ctx, server->mode)},
                          {"Players", std::to_string(server->players) + "/" + std::to_string(server->max_players)},
                          {"Bots", std::to_string(server->bots)},
                          {"Ping", std::to_string(server->ping_ms) + " ms"},
                          {"Access", "Password required"}});
        else
            detail_panel(page, side, "ADDRESS",
                         {{"Type the server's IPv4 address.", ""},
                          {"Add :port when it is not 27500.", ""},
                          {"Example", "192.168.1.20:27500"},
                          {"A keyboard can type directly.", ""}});
        return;
    }
    // Under the keyboard, centred between its frame and the prompt row.
    const float prompts = ui::MenuChrome::authored(50, 419, 540, 21).y;
    const ui::Rect below{page.left(), bottom, page.right() - page.left(), prompts - bottom};
    if (password && server)
        description(page, below,
                    server_name(*server) + "  -  " + map_title(ctx, server->map) + " / " + mode_title(ctx, server->mode),
                    server_summary(*server));
    else
        description(page, below, "Type the server's IPv4 address; add :port when it is not 27500.",
                    "Example: 192.168.1.20:27500");
}

bool draw_browser(const AppContext& ctx, const MenuBackground& background, Window& window, ui::Renderer& renderer,
                  ui::TextRenderer& text, const std::vector<ServerBrowserEntry>& entries, std::size_t selected,
                  BrowserPage page, const std::string& address, const std::string& password,
                  const std::optional<ServerBrowserEntry>& password_entry, KeyboardCursor cursor,
                  std::uint8_t local_players) {
    int width = 0, height = 0;
    window.begin_frame(width, height);
    renderer.begin(width, height);
    background.draw(renderer);
    ui::MenuChrome chrome(renderer, text, ctx.menu);
    chrome.logo();
    if (page == BrowserPage::Servers) {
        draw_servers(ctx, chrome, entries, selected);
        chrome.prompts("~A Join  ~V Scroll  ~B Refresh  ~X Direct IP  ~Y Back");
    } else if (page == BrowserPage::LocalPlayers) {
        chrome.title("Players Joining");
        const ui::Rect panel = chrome.span(103, 187);
        chrome.panel(panel);
        if (password_entry) {
            const std::string title = server_name(*password_entry) + "  -  " +
                                      map_title(ctx, password_entry->map) + " / " + mode_title(ctx, password_entry->mode);
            chrome.label({panel.x + 12, panel.y + 18, panel.w - 24, 20}, title, 2, ui::Align::Center, kLabelColor);
        }
        chrome.label({panel.x + 12, panel.y + 66, panel.w - 24, 24},
                     std::to_string(local_players) + (local_players == 1 ? " Local Player" : " Local Players"),
                     3, ui::Align::Center, kLabelColor);
        chrome.label({panel.x + 20, panel.y + 104, panel.w - 40, 40},
                     "Choose how many players on this console are joining the match.",
                     2, ui::Align::Center, kItemColor);
        chrome.prompts("~A Join  ~V Change Count  ~X Back");
    } else {
        draw_entry(ctx, chrome, page, page == BrowserPage::Password ? password : address, password_entry, cursor);
        chrome.prompts("~A Select  ~W Move  ~B Erase  ~X Back");
    }
    renderer.end();
    window.swap();
    return true;
}

std::optional<NetworkClientOptions> choose_entry(const ServerBrowserEntry& entry, const std::string& password,
                                                 const std::string& player_name, std::uint8_t local_players) {
    NetworkClientOptions options;
    options.endpoint = entry.endpoint;
    options.map = entry.map;
    options.password = password;
    options.name = player_name;
    options.local_players = local_players;
    return options;
}

}  // namespace

std::optional<NetworkClientOptions> run_online_browser(const AppContext& ctx, Window& window,
                                                        ui::Renderer& renderer, ui::TextRenderer& text,
                                                        const std::string& press, const std::string& shot,
                                                        const std::string& player_name,
                                                        const std::string& master_endpoint) {
    // The front end's looping background movie, a second into its loop like the pages it follows.
    MenuBackground background(ctx.gamedir);
    for (int i = 0; i < 30; ++i) background.advance();
    Uint64 movie_clock = SDL_GetTicksNS();
    ServerBrowser browser;
    KeyboardCursor keyboard;
    std::string master_host;
    std::uint16_t master_port = 27501;
    if (!master_endpoint.empty()) {
        const std::size_t colon = master_endpoint.rfind(':');
        master_host = master_endpoint.substr(0, colon);
        if (master_host.empty()) throw std::runtime_error("--online-master has an empty host");
        if (colon != std::string::npos) {
            char* end = nullptr;
            const long value = std::strtol(master_endpoint.c_str() + colon + 1, &end, 10);
            if (!end || *end || value < 1 || value > 65535)
                throw std::runtime_error("--online-master has an invalid port");
            master_port = std::uint16_t(value);
        }
    }
    std::vector<ServerBrowserEntry> entries;
    auto refresh = [&] {
        entries = browser.lan();
        if (master_host.empty()) return;
        for (auto& entry : browser.master(master_host, master_port)) {
            const auto found = std::find_if(entries.begin(), entries.end(), [&](const ServerBrowserEntry& item) {
                return item.endpoint == entry.endpoint;
            });
            if (found == entries.end()) entries.push_back(std::move(entry));
            else *found = std::move(entry);
        }
    };
    refresh();
    std::size_t selected = 0;
    BrowserPage page = BrowserPage::Servers;
    std::string address, password;
    std::optional<ServerBrowserEntry> password_entry;
    std::uint8_t local_players = 1;
    if (!entries.empty()) std::printf("online browser: found %zu LAN server(s)\n", entries.size());

    draw_browser(ctx, background, window, renderer, text, entries, selected, page, address, password,
                 password_entry, keyboard, local_players);

    auto begin_join = [&](const ServerBrowserEntry& entry, std::string value) {
        password_entry = entry;
        password = std::move(value);
        local_players = 1;
        page = BrowserPage::LocalPlayers;
    };

    auto handle = [&](std::string_view token) -> std::optional<NetworkClientOptions> {
        if (token == "net-up" || token == "up") {
            if (page == BrowserPage::Servers && !entries.empty())
                selected = selected == 0 ? entries.size() - 1 : selected - 1;
            else if (page == BrowserPage::LocalPlayers)
                local_players = std::min<std::uint8_t>(nf::net::kMaxLocalPlayers, local_players + 1);
            else if (page != BrowserPage::Servers) move_keyboard(keyboard, 0, -1);
        } else if (token == "net-down" || token == "down") {
            if (page == BrowserPage::Servers && !entries.empty()) selected = (selected + 1) % entries.size();
            else if (page == BrowserPage::LocalPlayers)
                local_players = std::max<std::uint8_t>(1, local_players - 1);
            else if (page != BrowserPage::Servers) move_keyboard(keyboard, 0, 1);
        } else if (token == "net-left" || token == "left") {
            if (page != BrowserPage::Servers && page != BrowserPage::LocalPlayers) move_keyboard(keyboard, -1, 0);
        } else if (token == "net-right" || token == "right") {
            if (page != BrowserPage::Servers && page != BrowserPage::LocalPlayers) move_keyboard(keyboard, 1, 0);
        } else if (token == "net-triangle" || token == "triangle") {
            if (page == BrowserPage::Servers) {
                refresh();
                selected = 0;
            } else if (page == BrowserPage::Password || page == BrowserPage::Address) {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (!value.empty()) value.pop_back();
            }
        } else if (token == "net-circle" || token == "circle") {
            if (page == BrowserPage::Servers) {
                page = BrowserPage::Address;
                address.clear();
                keyboard = {};
            } else {
                page = password_entry && password_entry->password_required ? BrowserPage::Password : BrowserPage::Servers;
                if (page == BrowserPage::Servers) password_entry.reset();
                address.clear();
                password.clear();
                keyboard = {};
            }
        } else if (token == "net-square" || token == "square") {
            return NetworkClientOptions{};
        } else if (token == "net-backspace" && (page == BrowserPage::Password || page == BrowserPage::Address)) {
            std::string& value = page == BrowserPage::Password ? password : address;
            if (!value.empty()) value.pop_back();
        } else if (token.rfind("net-text:", 0) == 0 &&
                   (page == BrowserPage::Password || page == BrowserPage::Address)) {
            std::string& value = page == BrowserPage::Password ? password : address;
            const std::string_view typed = token.substr(9);
            if (value.size() + typed.size() <= 64) value.append(typed);
        } else if (token == "net-cross" || token == "cross") {
            if (page == BrowserPage::LocalPlayers && password_entry) {
                return choose_entry(*password_entry, password, player_name, local_players);
            }
            if (page == BrowserPage::Servers) {
                if (selected >= entries.size()) return std::nullopt;
                const ServerBrowserEntry entry = entries[selected];
                if (entry.password_required) {
                    password_entry = entry;
                    password.clear();
                    page = BrowserPage::Password;
                    keyboard = {};
                } else {
                    begin_join(entry, {});
                }
            } else if (page == BrowserPage::Address) {
                const std::vector<ServerBrowserEntry> found = browser.direct(address);
                if (found.empty()) std::printf("online browser: no server at %s\n", address.c_str());
                else if (found.front().password_required) {
                    password_entry = found.front();
                    password.clear();
                    page = BrowserPage::Password;
                    keyboard = {};
                } else {
                    begin_join(found.front(), {});
                }
            } else if (page == BrowserPage::Password && password_entry) {
                begin_join(*password_entry, password);
            }
            return std::nullopt;
        }
        return std::nullopt;
    };

    auto type_keyboard_key = [&]() -> std::optional<NetworkClientOptions> {
        if (keyboard.row == int(kKeyRows.size())) {
            if (keyboard.column == 0) {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (!value.empty()) value.pop_back();
            } else if (keyboard.column == 1) {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (value.size() < 64) value.push_back(' ');
            } else {
                return handle("net-cross");
            }
        } else {
            std::string& value = page == BrowserPage::Password ? password : address;
            const char c = kKeyRows[std::size_t(keyboard.row)][std::size_t(keyboard.column)];
            if (value.size() < 64) value.push_back(c);
        }
        return std::nullopt;
    };

    if (!press.empty()) {
        for (std::size_t start = 0; start < press.size();) {
            const std::size_t comma = press.find(',', start);
            const std::string_view token(press.data() + start,
                                         (comma == std::string::npos ? press.size() : comma) - start);
            if (token.rfind("net-", 0) != 0) {
                start = comma == std::string::npos ? press.size() : comma + 1;
                continue;
            }
            const bool keyboard_page = page == BrowserPage::Address || page == BrowserPage::Password;
            const auto result = token == "net-key" && keyboard_page ? type_keyboard_key() : handle(token);
            if (result) {
                if (result->endpoint.empty()) return std::nullopt;
                return result;
            }
            start = comma == std::string::npos ? press.size() : comma + 1;
        }
        draw_browser(ctx, background, window, renderer, text, entries, selected, page, address, password,
                     password_entry, keyboard, local_players);
        if (!shot.empty() && !window.save_bmp(shot)) throw std::runtime_error("could not save online browser screenshot");
        return std::nullopt;
    }

    SDL_StartTextInput(window.sdl());
    SDL_Gamepad* gamepad = nullptr;
    int gamepad_count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&gamepad_count)) {
        if (gamepad_count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    PadHistory gamepad_history;
    bool quit = false;
    while (!quit) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            else if (event.type == SDL_EVENT_TEXT_INPUT &&
                     (page == BrowserPage::Address || page == BrowserPage::Password)) {
                std::string& value = page == BrowserPage::Password ? password : address;
                const std::string add = event.text.text;
                if (value.size() + add.size() <= 64) value += add;
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                std::optional<NetworkClientOptions> result;
                if (page == BrowserPage::Servers) {
                    switch (event.key.key) {
                        case SDLK_UP: result = handle("net-up"); break;
                        case SDLK_DOWN: result = handle("net-down"); break;
                        case SDLK_LEFT: result = handle("net-left"); break;
                        case SDLK_RIGHT: result = handle("net-right"); break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_X: result = handle("net-cross"); break;
                        case SDLK_ESCAPE: case SDLK_C: result = handle("net-circle"); break;
                        case SDLK_T: result = handle("net-triangle"); break;
                        case SDLK_S: result = handle("net-square"); break;
                        default: break;
                    }
                } else {
                    switch (event.key.key) {
                        case SDLK_UP: result = handle("net-up"); break;
                        case SDLK_DOWN: result = handle("net-down"); break;
                        case SDLK_LEFT: result = handle("net-left"); break;
                        case SDLK_RIGHT: result = handle("net-right"); break;
                        case SDLK_RETURN: case SDLK_KP_ENTER: result = handle("net-cross"); break;
                        case SDLK_ESCAPE: result = handle("net-circle"); break;
                        case SDLK_BACKSPACE: result = handle("net-backspace"); break;
                        default: break;
                    }
                }
                if (result) {
                    SDL_StopTextInput(window.sdl());
                    if (gamepad) SDL_CloseGamepad(gamepad);
                    return result->endpoint.empty() ? std::nullopt : result;
                }
            }
        }
        if (gamepad) {
            const PadState pad = [&] {
                PadState state;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP)) state.buttons |= kPadUp;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) state.buttons |= kPadDown;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) state.buttons |= kPadLeft;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) state.buttons |= kPadRight;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH)) state.buttons |= kPadCross;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST)) state.buttons |= kPadCircle;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_NORTH)) state.buttons |= kPadTriangle;
                if (SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)) state.buttons |= kPadSquare;
                return state;
            }();
            gamepad_history.push(pad);
            std::optional<NetworkClientOptions> result;
            if (gamepad_history.pressed(kPadUp)) result = handle("net-up");
            else if (gamepad_history.pressed(kPadDown)) result = handle("net-down");
            else if (gamepad_history.pressed(kPadLeft)) result = handle("net-left");
            else if (gamepad_history.pressed(kPadRight)) result = handle("net-right");
            else if (gamepad_history.pressed(kPadCross))
                result = (page == BrowserPage::Address || page == BrowserPage::Password)
                             ? type_keyboard_key() : handle("net-cross");
            else if (gamepad_history.pressed(kPadCircle)) result = handle("net-circle");
            else if (gamepad_history.pressed(kPadTriangle)) result = handle("net-triangle");
            else if (gamepad_history.pressed(kPadSquare)) result = handle("net-square");
            if (result) {
                SDL_StopTextInput(window.sdl());
                SDL_CloseGamepad(gamepad);
                return result->endpoint.empty() ? std::nullopt : result;
            }
        }
        // The movie runs at 30 frames per second whatever the redraw rate; discovery stalls are skipped.
        constexpr Uint64 kMovieFrame = SDL_NS_PER_SECOND / 30;
        const Uint64 now = SDL_GetTicksNS();
        if (now - movie_clock > 8 * kMovieFrame) movie_clock = now - kMovieFrame;
        for (; now - movie_clock >= kMovieFrame; movie_clock += kMovieFrame) background.advance();
        draw_browser(ctx, background, window, renderer, text, entries, selected, page, address, password,
                     password_entry, keyboard, local_players);
        SDL_Delay(16);
    }
    SDL_StopTextInput(window.sdl());
    if (gamepad) SDL_CloseGamepad(gamepad);
    return std::nullopt;
}

}  // namespace nf::app
