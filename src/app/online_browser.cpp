#include "app/online_browser.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "app/app.hpp"
#include "app/menu_background.hpp"
#include "app/server_browser.hpp"
#include "game/input.hpp"
#include "ui/art_sheet.hpp"
#include "ui/menu_chrome.hpp"
#include "ui/prompts.hpp"

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

// Where a listed server was found (online.png source_* icons); a favourite also keeps its discovery source.
enum SourceBits : std::uint8_t { kFromLan = 1, kFromMaster = 2, kFavourite = 4 };

const char* rules_name(const ServerBrowserEntry& entry) {
    return entry.slot_count >= 16 ? "Extended" : entry.slot_count == 10 ? "GC/Xbox" : "PS2";
}
const char* rules_badge(const ServerBrowserEntry& entry) {
    return entry.slot_count >= 16 ? "badge_extended" : entry.slot_count == 10 ? "badge_gcxbox" : "badge_ps2";
}
// Signal bars for a round trip: four under 50 ms, none from 250 ms.
int ping_bars(std::uint32_t ms) { return ms < 50 ? 4 : ms < 100 ? 3 : ms < 150 ? 2 : ms < 250 ? 1 : 0; }

// "PS2 rules, modified. 2 of 4 agents, 1 bot, 15 ms." (+ " Password required.").
std::string server_summary(const ServerBrowserEntry& entry) {
    std::string text = std::string(rules_name(entry)) + (entry.modified_rules ? " rules, modified. " : " rules. ") +
                       std::to_string(entry.players) + " of " + std::to_string(entry.max_players) + " agents, " +
                       std::to_string(entry.bots) + (entry.bots == 1 ? " bot, " : " bots, ") +
                       std::to_string(entry.ping_ms) + " ms.";
    return entry.password_required ? text + " Password required." : text;
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

// Everything a browser frame shows.
struct BrowserView {
    const std::vector<ServerBrowserEntry>* entries = nullptr;
    const std::vector<std::uint8_t>* sources = nullptr;   // SourceBits per entry
    std::size_t selected = 0;
    BrowserPage page = BrowserPage::Servers;
    const std::string* address = nullptr;
    const std::string* password = nullptr;
    const std::optional<ServerBrowserEntry>* server = nullptr;   // the server being joined
    KeyboardCursor cursor;
    std::uint8_t local_players = 1;
    bool searching = false;     // a LAN / master / favourites query is running
    bool connecting = false;    // a join was chosen; the session connects next
    unsigned frame = 0;         // 30 Hz frames, for the spinner
};

constexpr float kArtAspect = 7.5f / 7.0f;   // art texels stay square on the canvas

// An online.png icon vertically centred in a list row at x; returns its width.
float row_icon(ui::MenuChrome& page, std::string_view name, float x, float row_y, bool current) {
    const ui::ArtSprite* a = ui::art_sprite("online", name);
    if (!a) return 0;
    page.art("online", name, x, row_y + (kRowPitch - a->src.h) * 0.5f, current ? 0x7F7F7FFF : 0x7F7F7FC0);
    return a->src.w * kArtAspect;
}

std::string source_name(std::uint8_t source) {
    const char* found = source & kFromLan ? "Local network" : source & kFromMaster ? "Master list" : nullptr;
    if (!found) return "Favourite";
    return source & kFavourite ? std::string(found) + ", favourite" : std::string(found);
}

// The server list: a full-width P_MPJOIN panel whose header strip carries the column titles, one row
// per server on the option box's 21-unit pitch, and the scenario page's description box below it.
// Rows start with where the server was found and its lock, the RULES column holds the rule-set badge
// (and a gear when the host changed the rules), PING shows signal bars.
void draw_servers(const AppContext& ctx, ui::MenuChrome& page, const BrowserView& v) {
    const std::vector<ServerBrowserEntry>& entries = *v.entries;
    page.title(ctx.assets.strings.label(0x29e));   // "Join Game"
    const ui::Rect panel = page.span(103, 187);
    page.panel(panel);
    const float header = ui::MenuChrome::panel_header_height();
    const float inner_x = panel.x + 20.0f, inner_w = panel.w - 40.0f;

    // Fixed-content columns are as wide as their widest content; the text columns share the rest. The 4:3
    // list (500 units inside) leaves scenario and bots to the description box, wider lists show them too.
    constexpr float kGap = 16.0f;    // P_MPJOIN's gutter between panels (script x 312 -> 328)
    constexpr float kIcons = 30.0f;  // source icon + lock before the name
    const bool wide = inner_w > 560.0f;
    enum Kind { Name, Map, Scenario, Rules, Players, Bots, Ping };
    struct Column {
        Kind kind;
        const char* title;
        ui::Align align;
        float weight = 0;   // share of the text width (text columns)
        float width = 0, x = 0;
    };
    const Column all[] = {{Name, "SERVER", ui::Align::Left, 0.46f},  {Map, "MAP", ui::Align::Left, 0.24f},
                          {Scenario, "SCENARIO", ui::Align::Left, 0.30f}, {Rules, "RULES", ui::Align::Left},
                          {Players, "PLAYERS", ui::Align::Right}, {Bots, "BOTS", ui::Align::Right},
                          {Ping, "PING", ui::Align::Right}};
    std::vector<Column> columns;
    for (const Column& c : all)
        if (wide || (c.kind != Scenario && c.kind != Bots)) columns.push_back(c);
    const auto text_of = [&](const ServerBrowserEntry& e, Kind kind) -> std::string {
        switch (kind) {
            case Name: return server_name(e);
            case Map: return map_title(ctx, e.map);
            case Scenario: return mode_title(ctx, e.mode);
            case Players: return std::to_string(e.players) + "/" + std::to_string(e.max_players);
            case Bots: return std::to_string(e.bots);
            case Ping: return std::to_string(e.ping_ms) + " ms";
            case Rules: return {};
        }
        return {};
    };
    const ui::ArtSprite* widest_badge = ui::art_sprite("online", "badge_gcxbox");
    const ui::ArtSprite* gear = ui::art_sprite("online", "modified");
    const ui::ArtSprite* bars = ui::art_sprite("online", "ping_4");
    const float rules_w = ((widest_badge ? widest_badge->src.w : 0) + (gear ? gear->src.w : 0)) * kArtAspect + 3.0f;
    const float bars_w = (bars ? bars->src.w : 0) * kArtAspect + 4.0f;
    float fixed = 0, weights = 0;
    for (Column& c : columns) {
        if (c.weight > 0) {
            weights += c.weight;
            continue;
        }
        c.width = page.text_width(c.title, 2);
        if (c.kind == Rules) c.width = std::max(c.width, rules_w);
        else
            for (const ServerBrowserEntry& e : entries)
                c.width = std::max(c.width, page.text_width(text_of(e, c.kind), 2) + (c.kind == Ping ? bars_w : 0.0f));
        fixed += c.width;
    }
    const float text_w = inner_w - kIcons - fixed - kGap * float(columns.size() - 1);
    float x = inner_x;
    for (Column& c : columns) {
        if (c.kind == Name) x += kIcons;
        if (c.weight > 0) c.width = text_w * c.weight / weights;
        c.x = x;
        x += c.width + kGap;
    }
    for (const Column& c : columns)
        page.label({c.x, panel.y + 2.0f, c.width, 14.0f}, c.title, 2, c.align, kLabelColor);

    const ui::Rect details = page.span(301, 113);
    const float body_top = panel.y + header + 4.0f;
    const float middle = body_top + (panel.y + panel.h - body_top) * 0.5f;
    if (v.searching && entries.empty()) {
        // The busy spinner (reused by every wait on the network) over the empty list.
        page.spinner(panel.x + panel.w * 0.5f, middle - 20.0f, v.frame, 1.5f);
        page.label({panel.x, middle + 6.0f, panel.w, kRowPitch}, "Searching for servers", 3, ui::Align::Center,
                   kLabelColor);
        description(page, details, "Looking on your network, the master list and your favourites.",
                    "Press ~X to enter an address instead.");
        return;
    }
    // A refresh over a listed page: a small spinner in the icon gutter of the title strip.
    if (v.searching) page.spinner(inner_x + kIcons * 0.5f - 2.0f, panel.y + header * 0.5f, v.frame, 0.6f);
    if (entries.empty()) {
        page.label({panel.x, middle - kRowPitch * 0.5f, panel.w, kRowPitch}, "No servers found", 3,
                   ui::Align::Center, kLabelColor);
        description(page, details, "Servers on your network, the master list and your favourites appear here.",
                    "Press ~B to search again or ~X to enter an address.");
        return;
    }
    const std::size_t visible = std::size_t((panel.y + panel.h - 4.0f - body_top) / kRowPitch);
    const std::size_t first = v.selected >= visible ? v.selected - visible + 1 : 0;
    const std::size_t last = std::min(entries.size(), first + visible);
    for (std::size_t i = first; i < last; ++i) {
        const ServerBrowserEntry& e = entries[i];
        const std::uint8_t source = (*v.sources)[i];
        const float y = body_top + float(i - first) * kRowPitch;
        const bool current = i == v.selected;
        // The selection bar is inset into the panel body, clear of its border.
        if (current) page.selection({panel.x + 8.0f, y + 1.0f, panel.w - 16.0f, kRowPitch - 2.0f});
        const std::uint32_t color = current ? kLabelColor : kItemColor;
        row_icon(page, source & kFavourite ? "source_favourite" : source & kFromLan ? "source_lan" : "source_master",
                 inner_x, y, current);
        if (e.password_required) row_icon(page, "lock", inner_x + 16.0f, y, current);
        for (const Column& c : columns) {
            if (c.kind == Rules) {
                const float bw = row_icon(page, rules_badge(e), c.x, y, current);
                if (e.modified_rules) row_icon(page, "modified", c.x + bw + 3.0f, y, current);
                continue;
            }
            const std::string text = text_of(e, c.kind);
            if (c.kind == Ping)
                row_icon(page, "ping_" + std::to_string(ping_bars(e.ping_ms)),
                         c.x + c.width - page.text_width(text, 2) - bars_w, y, current);
            page.label({c.x, y, c.width, kRowPitch}, page.clip(text, 2, c.width), 2, c.align, color);
        }
    }
    const ServerBrowserEntry& entry = entries[v.selected];
    description(page, details,
                server_name(entry) + "  -  " + map_title(ctx, entry.map) + " / " + mode_title(ctx, entry.mode) + "  (" +
                    source_name((*v.sources)[v.selected]) + ")",
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

void draw_browser(const AppContext& ctx, const MenuBackground& background, Window& window, ui::Renderer& renderer,
                  ui::TextRenderer& text, const BrowserView& v) {
    int width = 0, height = 0;
    window.begin_frame(width, height);
    renderer.begin(width, height);
    // Address/password pages take typed text, so their prompts never show text keys.
    ui::select_prompts(0, InputContext::Browser, v.page == BrowserPage::Address || v.page == BrowserPage::Password);
    background.draw(renderer);
    ui::MenuChrome chrome(renderer, text, ctx.menu);
    chrome.logo();
    const std::optional<ServerBrowserEntry>& server = *v.server;
    if (v.connecting) {
        // The wait while the session connects: the busy spinner in the list panel.
        chrome.title("Connecting");
        const ui::Rect panel = chrome.span(103, 187);
        chrome.panel(panel);
        const float middle = panel.y + panel.h * 0.5f;
        chrome.spinner(panel.x + panel.w * 0.5f, middle - 20.0f, v.frame, 1.5f);
        chrome.label({panel.x, middle + 6.0f, panel.w, kRowPitch}, server ? server_name(*server) : "Server", 3,
                     ui::Align::Center, kLabelColor);
        if (server)
            description(chrome, chrome.span(301, 113), map_title(ctx, server->map) + " / " + mode_title(ctx, server->mode),
                        server_summary(*server));
    } else if (v.page == BrowserPage::Servers) {
        draw_servers(ctx, chrome, v);
        chrome.prompts("~A Join  ~V Scroll  ~B Refresh  ~R Favourite  ~X Direct IP  ~Y Back");
    } else if (v.page == BrowserPage::LocalPlayers) {
        chrome.title("Players Joining");
        const ui::Rect panel = chrome.span(103, 187);
        chrome.panel(panel);
        if (server) {
            const std::string title = server_name(*server) + "  -  " +
                                      map_title(ctx, server->map) + " / " + mode_title(ctx, server->mode);
            chrome.label({panel.x + 12, panel.y + 18, panel.w - 24, 20}, title, 2, ui::Align::Center, kLabelColor);
        }
        chrome.label({panel.x + 12, panel.y + 66, panel.w - 24, 24},
                     std::to_string(v.local_players) + (v.local_players == 1 ? " Local Player" : " Local Players"),
                     3, ui::Align::Center, kLabelColor);
        chrome.label({panel.x + 20, panel.y + 104, panel.w - 40, 40},
                     "Choose how many players on this console are joining the match.",
                     2, ui::Align::Center, kItemColor);
        chrome.prompts("~A Join  ~V Change Count  ~X Back");
    } else {
        draw_entry(ctx, chrome, v.page, v.page == BrowserPage::Password ? *v.password : *v.address, server, v.cursor);
        chrome.prompts("~A Select  ~W Move  ~B Erase  ~X Back");
    }
    renderer.end();
    window.swap();
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
                                                        AppConfig& cfg, const std::string& press,
                                                        const std::string& shot, const std::string& player_name,
                                                        const std::string& master_endpoint) {
    // The front end's looping background movie, a second into its loop like the pages it follows.
    MenuBackground background(ctx.gamedir);
    for (int i = 0; i < 30; ++i) background.advance();
    Uint64 movie_clock = SDL_GetTicksNS();
    unsigned frame = 0;
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

    // Discovery runs on its own thread (its own socket) so the page keeps drawing the busy spinner: LAN
    // broadcast, then the master list, then each favourite queried directly.
    struct SearchResult {
        std::vector<ServerBrowserEntry> entries;
        std::vector<std::uint8_t> sources;
    };
    const auto search = [master_host, master_port](std::vector<std::string> favourites) {
        ServerBrowser lookup;
        SearchResult r;
        const auto add = [&](ServerBrowserEntry entry, std::uint8_t source) {
            const auto found = std::find_if(r.entries.begin(), r.entries.end(),
                                            [&](const ServerBrowserEntry& item) { return item.endpoint == entry.endpoint; });
            if (found == r.entries.end()) {
                r.entries.push_back(std::move(entry));
                r.sources.push_back(source);
            } else {
                r.sources[std::size_t(found - r.entries.begin())] |= source;
            }
        };
        for (ServerBrowserEntry& entry : lookup.lan()) add(std::move(entry), kFromLan);
        if (!master_host.empty())
            for (ServerBrowserEntry& entry : lookup.master(master_host, master_port)) add(std::move(entry), kFromMaster);
        for (const std::string& endpoint : favourites)
            for (ServerBrowserEntry& entry : lookup.direct(endpoint)) add(std::move(entry), kFavourite);
        for (std::size_t i = 0; i < r.entries.size(); ++i)
            if (std::find(favourites.begin(), favourites.end(), r.entries[i].endpoint) != favourites.end())
                r.sources[i] |= kFavourite;
        return r;
    };
    std::vector<ServerBrowserEntry> entries;
    std::vector<std::uint8_t> sources;
    std::future<SearchResult> pending;
    std::size_t selected = 0;
    const auto start_search = [&] {
        if (!pending.valid()) pending = std::async(std::launch::async, search, cfg.favourite_servers);
    };
    // Takes a finished search (or waits for it); keeps the selection on the same server when it is still listed.
    const auto take_search = [&](bool wait) {
        if (!pending.valid()) return;
        if (!wait && pending.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const std::string current = selected < entries.size() ? entries[selected].endpoint : std::string();
        SearchResult r = pending.get();
        entries = std::move(r.entries);
        sources = std::move(r.sources);
        const auto found = std::find_if(entries.begin(), entries.end(),
                                        [&](const ServerBrowserEntry& e) { return e.endpoint == current; });
        selected = found == entries.end() ? 0 : std::size_t(found - entries.begin());
        std::printf("online browser: found %zu server(s)\n", entries.size());
    };
    start_search();
    if (!press.empty()) take_search(true);
    BrowserPage page = BrowserPage::Servers;
    std::string address, password;
    std::optional<ServerBrowserEntry> password_entry;
    std::uint8_t local_players = 1;
    bool connecting = false;

    const auto redraw = [&] {
        BrowserView v;
        v.entries = &entries;
        v.sources = &sources;
        v.selected = selected;
        v.page = page;
        v.address = &address;
        v.password = &password;
        v.server = &password_entry;
        v.cursor = keyboard;
        v.local_players = local_players;
        v.searching = pending.valid();
        v.connecting = connecting;
        v.frame = frame;
        draw_browser(ctx, background, window, renderer, text, v);
    };
    // A chosen server: the connecting page is up while the caller opens the session.
    const auto connect = [&](std::optional<NetworkClientOptions> options) {
        connecting = true;
        redraw();
        if (!press.empty() && !shot.empty() && !window.save_bmp(shot))
            throw std::runtime_error("could not save online browser screenshot");
        return options;
    };
    redraw();

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
                start_search();
            } else if (page == BrowserPage::Password || page == BrowserPage::Address) {
                std::string& value = page == BrowserPage::Password ? password : address;
                if (!value.empty()) value.pop_back();
            }
        } else if (token == "net-favourite" || token == "r1") {
            // R1 adds or removes the selected server from the favourites (nightfire.cfg favourite_server lines).
            if (page == BrowserPage::Servers && selected < entries.size()) {
                const std::string& endpoint = entries[selected].endpoint;
                auto& list = cfg.favourite_servers;
                const auto found = std::find(list.begin(), list.end(), endpoint);
                if (found == list.end()) list.push_back(endpoint);
                else list.erase(found);
                sources[selected] ^= kFavourite;
                if (!sources[selected]) sources[selected] = kFavourite;   // keep a listed row its icon
                save_config(config_path(), cfg);
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
                return connect(choose_entry(*password_entry, password, player_name, local_players));
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
            // A scripted refresh is left running only when it is the last token (the shot shows the search).
            take_search(true);
            const bool keyboard_page = page == BrowserPage::Address || page == BrowserPage::Password;
            const auto result = token == "net-key" && keyboard_page ? type_keyboard_key() : handle(token);
            if (result) {
                if (result->endpoint.empty()) return std::nullopt;
                return result;
            }
            start = comma == std::string::npos ? press.size() : comma + 1;
        }
        frame = 9;   // a mid-turn spinner frame
        redraw();
        if (!shot.empty() && !window.save_bmp(shot)) throw std::runtime_error("could not save online browser screenshot");
        return std::nullopt;
    }

    SDL_StartTextInput(window.sdl());
    // The browser's buttons (InputContext::Browser) as the handler tokens the --press replay also uses.
    const auto token_for = [](std::uint16_t button) -> std::string_view {
        switch (button) {
            case kPadUp: return "net-up";
            case kPadDown: return "net-down";
            case kPadLeft: return "net-left";
            case kPadRight: return "net-right";
            case kPadCross: return "net-cross";
            case kPadCircle: return "net-circle";
            case kPadTriangle: return "net-triangle";
            case kPadSquare: return "net-square";
            case kPadR1: return "net-favourite";
            default: return {};
        }
    };
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
                const bool entry = page == BrowserPage::Address || page == BrowserPage::Password;
                const std::string_view token =
                    token_for(input_bindings().button_for_key(InputContext::Browser, event.key.scancode, entry));
                std::optional<NetworkClientOptions> result;
                if (!token.empty()) result = handle(token);
                if (result) {
                    SDL_StopTextInput(window.sdl());
                    return result->endpoint.empty() ? std::nullopt : result;
                }
            }
        }
        {
            std::uint16_t buttons = 0;
            for (SDL_Gamepad* g : input_devices().gamepads())
                buttons |= input_bindings().gamepad_buttons(InputContext::Browser, g);
            gamepad_history.push({buttons});
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
            else if (gamepad_history.pressed(kPadR1)) result = handle("net-favourite");
            if (result) {
                SDL_StopTextInput(window.sdl());
                return result->endpoint.empty() ? std::nullopt : result;
            }
        }
        // The movie runs at 30 frames per second whatever the redraw rate; discovery stalls are skipped.
        constexpr Uint64 kMovieFrame = SDL_NS_PER_SECOND / 30;
        const Uint64 now = SDL_GetTicksNS();
        if (now - movie_clock > 8 * kMovieFrame) movie_clock = now - kMovieFrame;
        for (; now - movie_clock >= kMovieFrame; movie_clock += kMovieFrame) {
            background.advance();
            ++frame;
        }
        take_search(false);
        redraw();
        SDL_Delay(16);
    }
    SDL_StopTextInput(window.sdl());
    return std::nullopt;
}

}  // namespace nf::app
