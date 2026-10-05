# Network multiplayer

## Implementation status

The `nf_net` library provides bounded UDP datagrams, protocol-version rejection, SHA-256 game-data hashing, redundant input/snapshot codecs, world-state and projectile-page codecs, reliable replication-event codecs, server queries, selective acknowledgements, and outbound one-way latency/loss/jitter injection. `nfnet-test` exercises these codec boundaries and reliability behavior. `nfserver` runs the shared `ArenaSession` at 60 Hz by default (`--logic-hz 30` selects the PS2-comparison rate), validates hashes/input ticks, relays chat, and broadcasts 30 Hz authoritative player/match snapshots plus pickup/objective state, projectile pages, and gameplay events. Graphical clients use the same selected logic rate to predict local input, reconcile to acknowledged server state, interpolate remote players and bot rigs, and consume authoritative HUD/scoreboard and replicated world state. Input bounds scale with the logic rate; shot rewinds remain bounded to 200 ms (12 ticks at 60 Hz, six at 30 Hz).

**Online multiplayer remains in development and is not supported for general play.** Prediction, interpolation, authoritative HUD, replicated state, and lossy graphical/multiprocess localhost soaks have smoke coverage. An instrumented two-client moving-target shot registered a kill/death at 150 ms RTT: on the shot, server tick 1491 received view tick 1483, clamped the rewind to tick 1485, and selected history tick 1485. This exercises the bounded rewind path but does not establish general hit fidelity. The frontend now provides menu-driven Online hosting/joining and listen-server match transitions, but these development builds are not an Internet security boundary; do not expose them to untrusted networks.

## Running the development server

Build it with `cmake --build build-netcode --target nfserver`. A local server can be smoke-started with:

```sh
build-netcode/nfserver ~/Projects/nightfire-data/ps2/ --map 07000024.bin --mode arena --bots 2 --port 27500
```

Supported server options include `--config`, `--map`, `--mode`, `--ruleset ps2|gc-xbox|extended`, `--bots`, `--port`, `--name`, `--password`, `--master`, `--frag-limit`, `--time-limit`, `--net-sim-loss`, `--net-sim-latency`, `--net-sim-jitter`, repeated `--rotation map.bin,mode`, `--visibility-culling` / `--no-visibility-culling`, `--logic-hz 30|60`, and `--ticks` (bounded headless run for local tests; it cannot be combined with rotation). Jitter defaults to ±5 ms around the configured one-way delay when enabled. `--time-limit` is in minutes; zero disables the timer, matching the original MP rule that timed expiry runs only for a positive duration. Server-side visibility culling defaults on; an optional `--config server.cfg` file uses `key=value` lines for these settings, and command-line options override config values.
Direct clients must use the same `--logic-hz` value as their server (the network protocol does not advertise the simulation rate); both default to 60 Hz, or set both to 30 Hz for PS2 comparisons. Example:
```ini
map=07000024.bin
mode=arena
ruleset=ps2
bots=2
port=27500
visibility-culling=true
logic-hz=60
rotation=07000025.bin,team-arena
rotation=07000024.bin,arena
```

For `nfserver`, the configured map/mode is the first match; after each completed match, repeated `rotation` entries cycle in order. The CLI form is `--rotation map.bin,mode` and may be repeated. Each transition resets peer state and requires clients to handshake again (a map change also changes the game-data hash).

The server uses the data directory's `ACTION.ELF` and `FILES.BIN`; the handshake hash includes `ACTION.ELF` and the selected map bytes.

Run a scripted headless client with the same data and map:

```sh
build-netcode/nightfire ~/Projects/nightfire-data/ps2/ --connect 127.0.0.1:27500 --map 07000024.bin --name Test --chat "hello from Nightfire" --frames 300 --press "forward+r1,wait10"
```

The headless client prints received snapshot score records and chat; `--chat message` sends one message after joining (maximum 200 bytes). For a graphical capture, use `--connect ... --frames 30 --shot network.bmp`; `--connect` without a frame cap opens an interactive single-view preview. Both graphical and headless clients support outbound `--net-sim-loss 0..100`, `--net-sim-latency ms` and `--net-sim-jitter` (bounded symmetric ±5 ms) injection; configure the server's matching flags for both-way simulation.
The server binds UDP on the requested port (default **27500**). LAN access requires no router changes; Internet access requires forwarding that one UDP port. `--ticks` is for bounded headless tests and does not represent a full match.

## Protocol foundation

- Protocol version is `7` (incompatible with version 6); datagrams retain the 20-byte little-endian header: magic (`NFNT`), version, message type, reserved byte, sequence, latest acknowledgement, and a 32-bit selective-ack mask. Packets over 1200 bytes and malformed headers are rejected.
- The `Hello` payload is a 32-byte data hash, one-byte UTF-8 player-name length (maximum 32), one-byte password length (maximum 64), one-byte local-player count (1–4), then the name and password bytes. `game_data_hash()` hashes a domain-separated, length-prefixed concatenation of loaded `ACTION.ELF` and selected level bytes; `nfserver` rejects mismatched data or passwords before assigning a contiguous block of human slots. `ServerInfo.players` and `ServerInfo.max_players` count connected humans and human capacity respectively (four for PS2/GC-Xbox; Extended uses `slot_count`); protocol 7 appends `bots` (0–16), total combatant capacity `slot_count` (8, 10, or 16), and `modified_rules`.
- Input batches carry the newest sample and up to two earlier samples **per local player** (up to 12 samples for four local players). Each sample includes its local-player index, raw DS2 buttons/sticks, input tick, and the newest server snapshot tick represented by that player's view. At 60 Hz the server admits samples up to 12 ticks ahead and 60 ticks old; the `--logic-hz 30` comparison rate uses six ahead and 30 old. Samples are de-duplicated and queued in tick order, and each local player's sample is applied per simulation tick. Snapshot acknowledgements advance only when a sample is consumed. View ticks may not increase toward older redundant samples; rewinds are limited to 200 ms (12 ticks at 60 Hz or six at 30 Hz), so a stale initial view timestamp does not discard player movement.
- Player snapshots cover up to sixteen MP slots and use two-player pages, with match-wide state repeated across pages. They include the 64-bit match revision, authoritative pose/health, visibility and radar coordinates, score/kills/deaths/points, team/out/name/character, substate, weapon/animation/ammo/aim state, match phase/state, score limit, clock/team scores, and the recipient's acknowledged input tick. Protocol 7 can carry a fixed 206-byte schema-version-2 movement checkpoint on each receiving human's own slot record; it includes movement/collision history, scoped-aim zoom state, aim ramps, timing, body basis, and all `WaterState` fields for local-player rollback, and is never attached to other players or bots. Unknown/old schemas, truncated blocks, non-human records, and multiple checkpoint blocks are rejected.
- `WorldState` carries pickup active/waiting/gone state and spin plus objective kind/team/carrier/state/visibility/position/health. Projectile state is split into pages of at most 32 entries and assembled by tick on clients. `Event` packets cover weapon sounds, impacts, explosions, match messages, and pickup notices; server events use selective-ack retransmission. `NetworkSession` bounds its event/chat queues and retains the latest world/projectile state.
- `Reliability` assigns sequence numbers, tracks the latest packet plus a 32-packet acknowledgement history, suppresses duplicate receive sequences, and requests reliable retransmission after 100 ms. Welcome, chat, and server gameplay events currently use reliable sends; input, snapshots, world-state and projectile pages are best effort. Acknowledgements piggyback on regular traffic.

`UdpSocket::simulate()` injects faults on outbound packets only. Loss is sampled per datagram; latency holds a datagram for the configured one-way delay and jitter selects an independent whole-millisecond offset in `[-jitter_ms,+jitter_ms]`, clamping the resulting delay at zero. Delayed datagrams are released by due time, so jitter can reorder them. For a two-sided simulated link, configure both endpoint sockets. This is a deterministic test aid, not a network quality estimator.

## Hosting, ports, and discovery

- `nfserver` binds UDP on port **27500** by default. It answers bounded server-info queries with the server name, map, mode, `players`/`max_players` human counts and capacity, total combatant capacity in `slot_count`, advertised bot count in `bots`, password-required flag, and match revision. `--password <text>` gates the initial handshake; clients send the password and requested local-player count as part of protocol-7 `Hello`. The stdin admin console accepts `status`, `kick <human-slot>`, `say <text>`, and `password <text>` (an empty password disables the gate). These development controls are not an Internet security boundary.

To publish a dedicated server, run `nfmaster` on a reachable UDP port (default **27501**) and start the game server with `--master <host:port>`. Registrations repeat every 20 seconds and expire after 60 seconds without a heartbeat. `nfmaster --list <host:port>` queries the registry and prints its current endpoint/name entries. The registry is optional: direct-IP and LAN discovery do not require it. The master wire format is a separate bounded registry protocol and is not part of the gameplay handshake.

`nightfire --browse-lan`, `--browse-master <IPv4[:port]>`, and `--browse-ip <IPv4[:port]>` remain command-line discovery tools; `--connect <IPv4[:port]> --password <text> --local-players 1..4` joins directly with the requested number of local pads. The main-menu Multiplayer entry now offers Online Host or Join while retaining the existing local multiplayer flow. The embedded listen host runs the authoritative simulation in-process and joins it with a local network client. Internet access requires forwarding the server's UDP game port. `nf_net::UdpSocket` binds only when application code calls `bind()`; it does not implicitly claim a port.


## In-process listen-server lifecycle

`nf::net::ServerRuntime` embeds the same authoritative UDP server used by `nfserver`. `start()` waits for data loading and socket bind; `stop()` joins the server worker. `current_match()` reports its map, mode, match index, Over state, and monotonically increasing revision. `enqueue_match()` queues a next match; queued entries take precedence over the configured rotation, which cycles when the queue is empty. With neither configured, the current match repeats.

When a match reaches `Over`, the runtime continues sending final snapshots for three seconds so clients can show the result screen. It then destroys the match and peer/reliability state, loads the next map/mode, and rebinds the same UDP port. Clients must establish a fresh handshake for each match; a map change also changes the game-data hash. ServerInfo and snapshots carry the 64-bit match revision, so remote and local clients can detect repeated identical map/mode entries and reconnect.

## Frontend online host and join

On the main menu, selecting **Multiplayer** with **Cross** opens the game's native Local/Online choice list. Local continues to the original split-screen setup; Online opens a second native list for Host or Join. **Circle** remains a shortcut directly to the Online Host/Join list. Online Host then uses the original multiplayer setup pages (scenario, map, player setup, options, bots, and confirmation) before launching an in-process `ServerRuntime`; the local host participates through the same `NetworkSession` transport as remote clients. The server binds UDP port 27500 by default; `--listen-port` changes it. `--password` gates both local and remote handshakes, `--name` supplies the advertised/player name, and `--online-master <IPv4[:port]>` registers the host with the master and adds it to the join browser.

Online Join opens a server browser over the original multiplayer background art and panel/list styling. Rows show the server name, localized map and mode names, human players/max, bot count, ping, and password lock. LAN discovery is combined with the optional `--online-master` registry; **Up/Down** selects, **Cross** joins, **Triangle** refreshes, **Circle** opens direct-IP entry, and **Square** returns to the frontend. Password-protected servers show their password prompt. Direct IP and password pages provide a gamepad-navigable character grid plus normal keyboard/SDL text input; **Cross** selects/finishes, **Triangle** erases, and **Circle** backs out. Map/mode changes after match completion are advertised by the server; clients reconnect with a fresh handshake to the next match.

After selecting an open server (or submitting its direct IP/password), the Join flow asks how many local players on this console are joining. **Up/Down** chooses one through four, **Cross** connects with that contiguous local-player block, and **Circle** returns to the server list or password prompt. The server rejects a request if the requested players do not fit its remaining human slots.

The online screens are built from the multiplayer pages of the menu script (`07000048.bin`). The Local/Online and Host/Join lists use the Select Scenario composition (ring, picture, wheel rows, description box and glyph prompt row) over the message box's list input; **Triangle** backs out of them (Host/Join returns to Local/Online, which returns to the main menu). The browser, direct-IP and password pages reuse the Join Game agent panel, the codename keyboard (frame, name field, 40-unit key pitch, delete/space/done icons; the selected key turns from orange to the label colour) and the description box. `ui::MenuChrome` (`src/ui/menu_chrome.*`) draws this furniture from the script's authored boxes through `fixup_resolution`, with the page margins (script x 50 and 590) anchored to the screen edges: at 4:3 the layout is the original page grid, and wider aspects extend the panels instead of centring a 4:3 column. From 16:10 up the server list adds bot and access columns; from 16:9 up the keyboard pages show the address help or server details in a side panel at most as wide as the keyboard frame (narrower pages show them as description lines under the keyboard). Prompt rows use the icon escapes `~A` cross, `~B` triangle, `~X` circle and `~Y` square.

For short localhost rotation checks, `--host-time-limit <minutes>` overrides the match timer on a listen host. A positive value also prepends one replay of the selected map/mode to the rotation, exercising revision-based reconnect when identifiers repeat. Scripted browser controls use `net-up`, `net-down`, `net-left`, `net-right`, `net-cross`, `net-circle`, `net-square`, `net-triangle`, `net-key` (select the on-screen character), `net-text:<text>`, and `net-backspace` in `--press`. Direct command-line network sessions use `--inputs <path>`.
The network `--inputs` file accepts `start x y z yaw [pitch [ground-normal-y]]` to seed the local player's initial render pose, `hold <first> <last> <Sony-pad-word-hex> <rx> <ry> <lx> <ly>` for inclusive frame ranges, `give <logic-frame> <weapon-id> <rounds>` for a local scripted loadout grant, and `shot <logic-frame> <path.bmp>` for deterministic screenshots. Ordinary input lines are `<logic-frame> <Sony-pad-word-hex> <rx> <ry> <lx> <ly>`. `start` affects only the joining client's initial local pose and remains subject to authoritative server correction.



## Compatibility and gameplay constraints

- The default **PS2** ruleset remains four local human players plus up to four bots (eight total MP slots; human slots 0–3 and bot slots 4–7). **GC/Xbox** compatibility remains four local humans plus up to six bots (ten slots). The explicit **Extended** rewrite mode has sixteen combatant slots: up to sixteen human players may join across network connections, each supplying one to four local players, and bots fill only unoccupied slots and are removed when a human claims a bot slot. The local split-screen/controller limit remains four; the slot capacity configures replicated world storage and snapshot pages. Duplicate character selections are allowed and participant identity is carried by slot. `modified_rules` marks changes to the default score limit (10), time limit (600 s), friendly-fire setting, weapon set (0), spawn selection (Random), or handicap (0); the scenario, bot count, and selected ruleset do not set this…

- The original multiplayer radar shows all participants. `radar_x/y/z` remain transmitted for every present participant by default (radar gating is off), even when the authoritative world pose is culled. Rendering must consume `visible` for world entities and use the separate radar coordinates for the radar. `--no-visibility-culling` disables pose culling; neither mode changes authoritative simulation.

## Verification

Protocol-7 capacity verification used `TMPDIR="$HOME/.cache/MpSlots-tmp" ninja -C build-MpSlots nfserver nightfire nfnet-test`; the scoped targets built without warnings and `./build-MpSlots/nfnet-test` passed. A live `nfserver ... --ruleset extended --bots 12 --port 27642` accepted the frontend's direct-IP join with two local players; the client reported both owner snapshots (slots 0 and 1) and a 12-bot match. The scripted UI path was `--page 0x40000002 --press wait60,down,cross,wait20,down,cross,wait20,down,cross,wait10,net-circle,net-text:127.0.0.1:27642,net-cross,net-up,net-cross --frames 30 --name MpSlotsBrowser --mute`. The protocol test also round-trips two-player redundant input samples and a 16-slot/12-bot server advertisement.

The final maximum-capacity smoke joined with `--local-players 4` to an Extended 12-bot server; each received snapshot consistently contained human slots 0–3 and bot slots 4–15.

A clean concurrent Extended 16-human smoke launched eight clients with `--local-players 2` against an Extended server with 16 bot reservations; all eight were assigned adjacent slot pairs covering 0–15 and each completed 600 input frames with 300 complete 16-player snapshots. The clients shared 293 snapshot ticks, with identical scoreboard rows at every shared tick, and all exited 0; the bounded server exited 0 after assigning all pairs. This caught and fixed the snapshot codec's legacy owner-movement slot-0–3 bound: per-client movement state is now encoded and decoded for any slot within the snapshot's `slot_count`.
A post-interpolation Xvfb client joined as two local players and saved a 640×480 split-screen gameplay/HUD frame, which was visually inspected.
An additional Extended run used `--time-limit 2 --ticks 9000` with eight headless clients supplying two local players each. All eight clients and the server exited 0; every client finished 5,400 input frames with complete snapshots containing all 16 human slots, and the clients' final ticks were 8498–8514. The 640×480 final debrief capture `build-MpSlots/mpslots-16-debrief-final.bmp` was visually inspected and showed all 16 rows.


A scoped `nfgame --mp --frag-limit 12 --frames 30` run reported the configured 12-frag match limit; `nfserver --frag-limit 12 --time-limit 1 --ticks 1 --port 27644` also started successfully, and server rule advertisements mark non-default limits as modified.

`TMPDIR=$HOME/.cache/netcode-tmp ninja -C build-netcode nfserver nfnet-test nightfire` built the scoped targets without warnings; `./build-netcode/nfnet-test` passed codec/reliability checks. Real-data smokes passed for peer-to-peer chat delivery under 5% loss / 100 ms one-way latency (port 27634), config loading and command-line precedence (config selected port 27625, overridden to 27626), and a two-client/two-bot culling-enabled session using the config path (port 27637). A five-minute lossy soak on the reliable transport ran four headless clients plus four server bots: server and all clients exited 0 after 9000 ticks/inputs with 5% outbound loss and 100 ms one-way latency configured on both ends. All four clients received the same tick-8994 scoreboard; among ticks seen by at least two clients, 2,991 shared snapshots showed no scoreboard mismatch. Bots ended at S/K/D 1/1/4, 5/5/1, 4/4/2, and 2/2/2; humans had 0 score and deaths 0, 1, 0, and 2. The reusable `NetworkSession` transport/protocol-2 update passed a scoped rebuild and codec tests; two real-data clients each completed 60 inputs with both endpoints configured for 5% loss and 100 ms latency (port 27652), received snapshots, and relayed peer chat. Two graphical clients connected simultaneously to a bot server on port 27654 (`NetViewA` slot 0 and `NetViewB` slot 1), each saved a 1280x720 BMP under `build-netcode/network-client-{a,b}.bmp` after 30 inputs; both captures were visually inspected. The five-minute soak predates protocol 2 and does not establish smooth visible movement, PCSX2 equivalence, graphical client consistency, a rendered culling proof, or Internet hosting.

Protocol-3 prediction/interpolation changes were built with `TMPDIR=$HOME/.cache/netpredict-tmp ninja -C build-netpredict nfserver nightfire nfnet-test` without warnings; `./build-netpredict/nfnet-test` passed. Four graphical clients (`xvfb-run -a ... --frames 9000 --shot ...`) joined a four-bot server on port 28755 with 5% loss and 100 ms one-way latency configured on both ends, completed all 9,000 input frames with exit status 0, and saved 1280x720 screenshots. The server ran for 5m37s; one capture was visually inspected. A separate two-client shot fixture with shooter slot 0 and target slot 1 produced one authoritative kill/death at 75 ms one-way latency on both endpoints (150 ms RTT, no injected loss); the target began strafing at input frame 1193 and the shooter pulsed R1 at frame 1200. Both final HUD captures showed the score change. Exact requested/clamped hit-history ticks were not logged, so this verifies moving-target damage over simulated latency but does not independently prove the historical hit volume selected.


The protocol-six hosting slice was built warning-free with `TMPDIR=$HOME/.cache/netlobby-tmp ninja -j1 -C build-netlobby nightfire nfserver`, followed by `TMPDIR=$HOME/.cache/netlobby-tmp ninja -j1 -C build-netlobby nfmaster`. A protocol-six server registered with `nfmaster`; master list, LAN query, and direct-IP query all returned the expected server info. Frontend browser smokes joined via both the LAN result and master-listed endpoint; a password-protected server was also reached by direct IP and joined after entering its password.

Protocol-six admin verification used `status`, changed a live server password, confirmed the old password was rejected and the updated password joined, then issued `kick 0`; the connected client received `nightfire network: kicked by server admin`. The bounded listen-host rotation smoke reached its match-time limit, advanced the configured same-map entry with a new match revision, and completed the reconnect.

The Xvfb frontend sequences used for LAN and master-row joins were:

```sh
TMPDIR="$HOME/.cache/netlobby-tmp" xvfb-run -a ./build-netlobby/nightfire ../nightfire-data/ps2 \
  --page 0x40000002 --press wait60,down,cross,wait20,down,cross,wait20,down,cross,wait10,net-cross \
  --frames 30 --shot build-netlobby/netlobby-lan-join-v6.bmp --name LanLobbyClient --mute
TMPDIR="$HOME/.cache/netlobby-tmp" xvfb-run -a ./build-netlobby/nightfire ../nightfire-data/ps2 \
  --online-master 127.0.0.1:27901 --page 0x40000002 \
  --press wait60,down,cross,wait20,down,cross,wait20,down,cross,wait10,net-down,net-cross \
  --frames 30 --shot build-netlobby/netlobby-master-join-v6.bmp --name MasterLobbyClient --mute
TMPDIR="$HOME/.cache/netlobby-tmp" xvfb-run -a ./build-netlobby/nightfire ../nightfire-data/ps2 \
  --page 0x40000002 \
  --press wait60,down,cross,wait20,down,cross,wait20,down,cross,wait10,net-circle,net-text:127.0.0.1:27501,net-cross,net-text:secret,net-cross \
  --frames 30 --shot build-netlobby/netlobby-direct-password-join-v6.bmp --name DirectPasswordClient --mute
```

Gallery captures (1280x720) were taken against three local servers: `nfmaster --port 27901`, `nfserver <data> --port 27500 --name "Nightfire LAN Party" --bots 2`, and two `--master 127.0.0.1:27901` servers on ports 27520 (`--password secret`) and 27530. Each screen is one `--size WxH --online-master 127.0.0.1:27901 --page 0x40000002` run with the presses `wait60,down,cross,wait20` (Local/Online), then `,down,cross,wait20` (Host/Join), then `,cross,wait60` (host setup) or `,down,cross,wait10` (browser), followed by `net-down` (browser), `net-circle,net-text:192.168.1.20:27500` (direct IP) or `net-circle,net-text:127.0.0.1:27520,net-cross,net-text:secr` (password). The same screens were also checked at 4:3 (1024x768), 16:9 (1920x1080) and 21:9 (2560x1080) beside the original PCSX2 multiplayer pages. Screenshots render disc-derived art, so they are not kept in the repository; regenerate them locally with these commands.

The saved 1280x720 visual checks covered the native Local/Online and Host/Join choices, the original host scenario setup page, the populated LAN/master browser (server name, Skyrail/Arena, players, bots, ping, and lock state), direct-IP entry, and a protected-server password prompt. The host/join PNGs are intentionally not checked into the public repository because they show disc-derived art; raw Xvfb captures remain local under `build-netlobby/`.




The current protocol-4 graphical soak ran two `nightfire --connect` clients against one `nfserver` with four server bots on map `07000024.bin`. Each client completed 9,000 input frames with 5% outbound loss and 50 ms one-way latency on both client and server; each saved a 1280x720 screenshot (inspected locally, not committed). Client A reported 2,850 prediction corrections (mean 10.74 cm, p99 0.00 cm) and 127,821 remote-position samples (p99 jitter 0.000 cm); client B reported 2,866 corrections (mean 4.80 cm, p99 0.00 cm) and 69,271 samples (p99 jitter 0.000 cm).

This soak proves sustained graphical client/server connectivity and rendered networked match state under the injected conditions; it does not prove PCSX2 equivalence or lag-compensated damage. A separate current-protocol headless two-client/four-bot localhost match completed 300 inputs per client; their shared tick-456 snapshot had identical scoreboard records (bot slot 7 at 1 kill/1 score and human slot 3 at 1 death), and matching records continued through tick 612. An earlier protocol-2 headless four-client soak recorded 2,991 shared snapshots without scoreboard mismatches.

For an active-match visual sample, two additional clients joined the same four-bot server on port 27721, each rendered 300 frames and saved an inspected 1280x720 screenshot showing live gameplay rather than the result screen at the end of the soak.

After matching `nfserver`'s bot weapon-bank and drone-system wiring to the offline MP setup, a four-client/four-bot lossy soak completed with 9,000 inputs per client. The server and all clients used 5% packet loss and 100 ms one-way latency; clients exited successfully after about 301–302 seconds. Their final snapshots (ticks 10965, 10977, 10989, and 11001) had identical scoreboards: bots in slots 5, 6, and 7 finished with 5, 1, and 5 kills respectively, while all four human clients remained idle. This validates sustained transport and consistent authoritative score replication during bot combat, not active human combat or prediction-correction accuracy.
An additional graphical prediction soak used one idle local player with 100 ms one-way latency configured on both client and server, using protocol-4 `--frames 9000 --shot` runs. With no loss, 2,997 snapshot reconciliations averaged 0.00 cm and had p99 0.00 cm; the initial authoritative baseline was 47.18 m from the locally spawned pose, followed by three startup corrections of 2.47, 2.47, and 1.65 cm. With 5% loss at both endpoints, 2,843 reconciliations averaged 0.00 cm and had p99 0.00 cm; after the initial 30.61 m baseline, no correction over 0.01 cm was logged. The metric excludes the first baseline and measures displacement after authoritative state application and unacknowledged-input replay. These are idle-input results, not evidence that active local movement meets the same correction bound; an earlier 1,800-frame moving-input diagnostic before the baseline reset measured p99 20.86 cm. The captures are `build-netpredict/predict-zero-metric.bmp` and `build-netpredict/predict-loss-metric.bmp`.
The first-authoritative-snapshot reset was exercised in a linked 300-frame graphical smoke at 75 ms one-way latency (run by Netcode-2): baseline tick 1089 had a 10.96 m local-pose delta, then startup corrections were 3.02 and 3.02 cm; 97 reconciliations averaged 0.06 cm with p99 3.02 cm. The 9,000-frame idle p99 runs above predate this reset; the active 1,800-frame runs below cover the current reset and input-admission behavior.
After the server decoupled movement-input admission from the client’s view tick, the active 1,800-input script (`build-netpredict/netpredict-active.inputs`: walk, strafe, jump, crouch, wall push) ran in graphical `xvfb-run` clients with 100 ms one-way latency configured on both ends. In the final schema-2 no-loss run, 597 reconciliations averaged 0.01 cm (p99 0.00 cm); remote-position jitter p99 was 0.000 cm. Baseline tick 321 differed by 321.95 cm; startup corrections were 3.02 cm at tick 324 (ack 320, 10 pending), 3.02 cm at tick 327 (ack 320, 12 pending), and 1.01 cm at tick 330 (ack 322, 14 pending). In the final schema-2 5% loss run, 566 reconciliations averaged 0.01 cm (p99 0.00 cm), with remote-position jitter p99 0.000 cm. Baseline tick 1965 differed by 1,874.59 cm; startup corrections were 3.02 cm at tick 1968 (ack 1963, 10 pending) and 2.01 cm at tick 1971 (ack 1964, 13 pending). The earlier post-admission 5% run recorded an additional 16.00 cm startup/warm-up correction at tick 351 (ack 343, 13 pending), after 3.02 cm corrections at ticks 345 and 348; none of these runs logged corrections over 0.01 cm after startup. Metrics exclude the first baseline and measure displacement after authoritative state application and unacknowledged-input replay. No per-datagram loss trace is available, so results are consistent with loss-only corrections after warm-up but do not directly correlate corrections to specific drops.

The offline differential command `TMPDIR=$HOME/.cache/netpredict-tmp ./build-netpredict/nfnet-predict-diff <game-dir> 07000024.bin scenario:combined <checkpoint-tick>` exercised an 1,800-tick walk/strafe/jump/crouch/wall-slide fixture. At checkpoints 0, 720 and 1,080, the owner-only movement checkpoint replay was bit-identical through the remaining ticks (`mismatch_ticks=0`); the public-pose-only replay diverged. Coverage was contact_ticks=1,626, jump_ticks=61, crouch_ticks=720 and wall_slide_ticks=360. The movement DTO includes collision/motion history, aim/timing state and `WaterState`; animation body playback remains render-only in the MP path.
The 60 Hz jitter audit found and fixed two input-timeline failures: the graphical client's local acknowledgement counter was never marked initialized, so every predicted sample reused the Welcome tick and the server de-duplicated later samples; and a delayed batch could be drained into one simulation step. The counter now advances once per logic step, and the server consumes at most one queued sample per local player per simulation step, keeping acknowledgements aligned with consumed movement. With three bots and `--net-sim-latency 12 --net-sim-jitter` on both local endpoints, the active 1,800-input script completed 898 reconciliations at p99 0.00 cm (mean 0.03 cm); one 22.32 cm startup correction was logged at tick 612 (ack 606, seven pending inputs), with no later correction above 0.01 cm. The initial spawn baseline is excluded from the correction metric. The `build-MpSlots/vps-active.inputs` fixture was not present in this checkout, so this run used `build-netpredict/netpredict-active.inputs`.
The same 1,800-input script against the rebuilt public server at `96.30.204.141:27500` completed 898 reconciliations (mean 0.05 cm, p99 0.00 cm; remote-position jitter p99 0.000 cm). After the initial 14,140.70 cm spawn baseline, five warm-up corrections occurred at ticks 1800–1808 (4.00–12.31 cm); none were logged during the remainder of the run. Public endpoint queries reported 27–40 ms. Packet-drop events are not currently exposed by the client, so these runs establish stable post-warm-up behavior but cannot attribute each startup correction to a particular lost datagram.



## Test deployment

A public development deployment was started on `nih-vps` at `96.30.204.141`. The dedicated server is named **Nightfire VPS Public Test**, serves Skyrail Arena (`07000024.bin`, Arena, PS2 ruleset) with three bots, no password, and 60 Hz logic. Its config also rotates to Subpen (`07000025.bin`, Team Arena) and back after a match ends. A short-timer rotation smoke with the current VPS binary on UDP 27676 exercised the transition to Subpen and back to Skyrail; that test port was not opened in the firewall. The public server uses an unlimited time limit, so its live match stayed on the initial map during the soak.

The service reads `~/nightfire/server.cfg`:

```ini
map=07000024.bin
mode=arena
ruleset=ps2
bots=3
port=27500
name=Nightfire VPS Public Test
master=96.30.204.141:27900
frag-limit=32767
time-limit=0
logic-hz=60
rotation=07000025.bin,team-arena
rotation=07000024.bin,arena
visibility-culling=true
```


The current working tree was copied to `~/nightfire/repo/` with `rsync` (excluding `.git`, `.omp`, and local build directories); PS2 runtime data is at `~/nightfire/data/ps2/`. On Fedora 44, the release build used:

```sh
cmake -S ~/nightfire/repo -B ~/nightfire/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build ~/nightfire/build --target nfserver nfmaster nightfire -j 8
```

After syncing the input-queue fix, the VPS server was incrementally rebuilt with `TMPDIR=~/nightfire/tmp cmake --build ~/nightfire/build --target nfserver -j 8` and restarted with `systemctl --user restart nfserver.service` before the post-fix active-input verification.


The firewall added only the authorized game and registry UDP ports, `27500/udp` and `27900/udp` (the existing SSH management service was unchanged). `nfmaster` and `nfserver` run as enabled root user services with linger; their logs are `~/nightfire/logs/nfmaster.log` and `~/nightfire/logs/nfserver.log`. Check them with `systemctl --user status nfmaster.service nfserver.service`; stop the deployment with `systemctl --user stop nfserver.service nfmaster.service` (and prevent user-session restart with `systemctl --user disable nfserver.service nfmaster.service`).

Before the final input-queue fix, a graphical client completed 1,800 input frames against the public endpoint and saved an inspected 1280×720 capture of the live Skyrail match. That earlier session logged 359 prediction corrections (mean 7.68 cm, maximum 81.55 cm) after a 101.64 m initial authoritative baseline; the post-fix active-input measurements above supersede those correction figures.

A VPS-side Extended capacity smoke used an `nfserver --ruleset extended --bots 0 --logic-hz 60 --port 27676 --ticks 1800` server and eight headless clients, each joining with `--local-players 2 --frames 600`. The clients were assigned adjacent slot pairs covering 0–15; all eight clients and the bounded server exited 0. Each client received 300 complete 16-slot snapshots, with 300 shared ticks and no scoreboard differences.

Before the final input-queue fix, the public service completed a 1,801-second Internet soak with four headless clients (`--frames 108000`, scripted `forward+r1,wait10`) and three server bots. All four clients exited 0 after the full input run. They received 53,847, 53,871, 53,852, and 53,872 snapshots; 53,839 common ticks had identical complete scoreboards and there were no mismatches. Their final snapshots agreed: human slots 0–3 had kills/deaths 0/2, 0/2, 1/11, and 0/4; bot slots 4–6 had 6, 8, and 4 kills. On the VPS, `nfserver` stayed at the same PID and used 17.60 CPU seconds (0.98% of one core) during the run. The `enp1s0` counters increased by 46,997,530 RX bytes and 371,757,002 TX bytes, averaging 208.76 kbit/s inbound and 1,651.34 kbit/s outbound.

During that pre-fix soak, 1,798 of 1,800 direct UDP server-info queries received responses (0.11% loss); the 1,798 measured RTTs ranged from 21 to 335 ms, averaged 36.03 ms, and had a 45 ms p95. Concurrent ICMP monitoring reported 1,793 replies to 1,796 packets (0.167% loss), with 19.910/23.967/273.809 ms min/average/max RTT and 19.840 ms mdev. These are observations from this route and run, not service guarantees.

The services are intentionally left running for public testing. The bounded capacity server, four soak clients, ICMP monitor, and UDP probes all exited at test completion; no temporary test listener remains on port 27676. Test-service stop commands and deployment logs are listed above; only the two deployment UDP ports were added to the firewall.
