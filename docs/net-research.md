# Online multiplayer: netcode research

Date: 2026-10-02. Question: which networking model should `nightfire-engine`
use for online multiplayer (new feature; the original is split-screen only),
given our assets: a **deterministic fixed-step sim** (`World::tick`,
`ArenaSession::tick`), now 60 Hz by default with a 30-Hz comparison option,
tiny inputs (one `PadState` = 6 bytes per tick per player), small arena maps,
a 16-participant extended roster, deterministic bots, and a re-simulation cost measured below.

> The architecture in the work brief (server-authoritative snapshots +
> prediction/interpolation + lag compensation over UDP) was treated as a
> hypothesis. Verdict up front: **it is the right one**. Details and the
> measured evidence follow.

## 0. Measured facts about the prior 30-Hz baseline (2026-10-02)

The numbers below were measured before the logic-rate change, at 30 ticks/s;
they remain useful as a historical CPU-cost baseline, not as current tick counts.
Headless `nfgame` match on Skyrail (`07000024.bin`, 28 pickups), default
`RelWithDebInfo` build, `SDL_VIDEODRIVER=dummy`:
| Match | Ticks (5 min) | Wall time | Ticks/s | × realtime |
|---|---|---|---|---|
| 1 idle human + 4 bots | 9000 | 3.46 s | ~2600 | ~86× |
| 4 idle humans + 4 bots | 9000 | 5.21 s | ~1730 | ~58× |
| 4 humans + 4 bots, 100 ticks (startup baseline) | 100 | 0.72 s | — | — |

Subtracting ~0.7 s startup (ELF parsing, level load), steady-state full-match
cost is **≈ 0.50 ms per tick** for a full 8-slot match *including bot brains*
(bots walked 350–500 m, fired, picked up items — they do real work every tick).

Consequences:

- **Rollback re-simulation is CPU-cheap**: 200 ms of misprediction at 30 Hz =
  6 ticks ≈ **3 ms**; even a full second (30 ticks) is ≈ 15 ms. CPU does not
  rule out rollback.
- **The blocker for rollback is state save/restore, not CPU.** Our sim was
  never built for it: the RNG is process-global mutable state
  (`nf::game_rng()`, `src/core/rng.hpp` — every consumer draws in tick order),
  bot brains hold internal state (`src/game/bot_*`), the arena holds timers,
  pickup respawns, objective state. Snapshotting all of that per tick (or
  serializing it for late join) is a large, invasive, bug-prone project — and
  every new sim feature must maintain save/load forever.
- The sim *is* cheap enough that **client-side prediction re-simulation**
  (re-running 6–30 ticks of *one* player's movement with the real `Player`
  code) costs well under a millisecond.

Float determinism note: our EE model does double-exact intermediates with
toward-zero truncation (`GameRng::truncf`, same idiom in gameplay float
paths). That is deterministic on x86-64/SSE2 and ARM-NEON hosts as long as
every peer uses the same code — fine for same-binary client/server, but
lockstep across *different* compilers/architectures would need ongoing
differential proof. Our CI differential story (`nfmips diff-*`, oracle
compares) covers same-host determinism (recorded-input replay reproduces
identical results — the acceptance test for server determinism), not
cross-arch bit-exactness. Another quiet vote against lockstep.

## 1. Candidate models and tradeoffs for THIS game

### A. Server-authoritative snapshots + prediction + interpolation + lag compensation

How it works (the Quake 3 → Source lineage): the server runs the one true
sim at 30 Hz. Clients send inputs; the server sends periodic world snapshots
(delta-compressed against the last client-acked state). Clients predict their
own movement locally and reconcile when the server state arrives; remote
entities are interpolated from a ~100 ms buffer; the server rewinds player
positions to the shooter's view time when resolving hitscan (lag
compensation). Authoritative references:

- Valve, "Source Multiplayer Networking":
  <https://developer.valvesoftware.com/wiki/Source_Multiplayer_Networking>
- Valve, "Lag Compensation":
  <https://developer.valvesoftware.com/wiki/Lag_Compensation>
- Valve, "Prediction":
  <https://developer.valvesoftware.com/wiki/Prediction>
- Fabien Sanglard, "Quake 3 Source Code Review: Network Model":
  <https://fabiensanglard.net/quake3/network.php>
- Gabriel Gambetta's series (client-server architecture, entity
  interpolation, prediction/reconciliation, lag compensation), with playable
  demos: <https://www.gabrielgambetta.com/client-server-game-architecture.html>,
  <https://www.gabrielgambetta.com/entity-interpolation.html>,
  <https://www.gabrielgambetta.com/client-side-prediction-server-reconciliation.html>,
  <https://www.gabrielgambetta.com/lag-compensation.html>
- Glenn Fiedler, "Snapshot Interpolation" and "State Synchronization":
  <https://gafferongames.com/post/snapshot_interpolation/>,
  <https://gafferongames.com/post/state_synchronization/>
- Blizzard, "'Overwatch' Gameplay Architecture and Netcode" (GDC talk —
  high-tick-rate servers, "favor the shooter", server authority with client
  prediction): <https://gdcvault.com/play/1024001> (video mirror:
  <https://www.youtube.com/watch?v=W3aieHjyNvw>)

Fit for Nightfire:

- **Bots live on the server** and never cross the wire as inputs — they are
  just part of the snapshot. No bot protocol needed.
- **Inputs are tiny** (6 bytes/tick), snapshots are small (Section 4) — the
  model with the most wire traffic is still trivially affordable here.
- **Late join / rejoin is natural**: the server sends full state + match
  config; no history needed. (Lockstep and rollback both need a full state
  snapshot mechanism for late join — which, per Section 0, is the thing we
  don't have and don't want to build.)
- **Per-client interest management is possible** — the *only* model of the
  four that allows server-side visibility culling (Section 5).
- Cost: must implement delta snapshots, prediction/reconciliation,
  interpolation, lag compensation. All standard, well-documented, and our
  sim's determinism makes prediction re-simulation exact rather than
  approximate.

### B. Deterministic lockstep (RTS style)

Every peer runs the sim; peers exchange inputs for tick N, and nobody
simulates tick N until everyone's input arrived. Reference: Glenn Fiedler,
"Deterministic Lockstep":
<https://gafferongames.com/post/deterministic_lockstep/>.

- Bandwidth is minimal (Section 4) and every peer sees the exact original
  behavior with zero protocol approximation.
- But: **one 200 ms peer (or one lost packet) stalls the whole match** unless
  input delay is added — and input delay *is* latency applied to everyone,
  permanently. At 50–200 ms RTT plus 5% loss, the match either stutters on
  every loss or runs everything 200+ ms delayed. RTS games accept this
  (hundreds of units, 2–10 Hz command rate); a twitch FPS cannot.
- Requires cross-machine bit-exactness forever (see float note in Section 0).
- **No visibility culling possible**: every client needs every input to
  simulate, so every client can know every position. Wallhacks become
  unfixable by design (Section 5).
- Late join requires shipping full sim state + RNG + bot brains (Section 0:
  the thing we don't have).
- Verdict: wrong model for an FPS on the open internet.

### C. Rollback / GGPO style (fighting games → small-count shooters)

Peers exchange inputs, simulate immediately, and re-simulate when a remote
input arrives late/wrong. References: GGPO developer guide
(<https://github.com/pond3r/ggpo/blob/master/doc/DeveloperGuide.md>, overview
<https://www.ggpo.net/>).

- Best-in-class *feel* for 1v1–2v2 with a good local opponent: no added
  delay, corrections are rare and small.
- Against it here: **P2P mesh** (8 players = 7 connections per client, NAT
  traversal per pair — Section 3), **no dedicated server / bots host problem**
  (whose machine runs the deterministic bots? whoever does can cheat), **no
  visibility culling** (same information-theoretic reason as lockstep), and
  the state save/restore burden from Section 0 — rollback needs a full sim
  snapshot *every tick*, the heaviest version of the thing we don't have.
- Visual corrections ("teleporting" remote players when mispredicted) get
  worse with player count and with 100–200 ms latency; fighting games get
  away with it at 2 players and small stages.
- Verdict: excellent for 1v1 fighting games, poor fit for 4–8-player FPS
  with bots, dedicated servers, and anti-cheat ambitions.

### D. Hybrid: deterministic input relay through a server

Clients send inputs to a server that only relays them (optionally delaying to
smooth jitter); all clients simulate deterministically. Used by some
mobile/RTS hybrids.

- Keeps lockstep's bandwidth and exactness while fixing P2P NAT (clients need
  only one connection).
- Keeps lockstep's *other* flaws: global stall/delay coupling, cross-arch
  determinism burden, no culling, late-join state transfer. The relay is a
  single point of failure that does none of the authoritative work a server
  could be doing.
- Verdict: the worst of both — all of lockstep's fragility with an extra
  hop. Not recommended.

## 2. Authenticity: which model preserves the original feel

What "accurate" means here: the original's 30 Hz timing, auto-aim behavior,
hit registration, damage rules, spawn logic, and bot behavior. Our server
runs the **unmodified deterministic sim**, so the *rules* are exact in model
A. What differs per model is only what remote players *perceive* under
latency:

- **Model A changes perception, not rules.** Local firing stays responsive
  (prediction); remote bodies render ~100 ms late (interpolation); hits are
  judged at the shooter's view time (lag compensation). Known artifact:
  **"shot around corners"** — you can be killed just after reaching cover
  because on the shooter's screen you were still exposed. This is inherent
  to favor-the-shooter compensation (Valve documents exactly this tradeoff
  in "Lag Compensation"; Overwatch's talk discusses the same "favor the
  shooter" choice). Mitigations, all standard: cap rewind (e.g. 200 ms —
  Source/TF2 precedent), rewind only player positions (never pickups,
  objectives, or RNG), keep the original's auto-aim inside its original
  server-side code path (see below), never extrapolate beyond the cap.
- **Auto-aim specifically**: ours is server-side data (`AutoaimTuning`:
  25 m range, yaw/pitch half-windows 0.12/0.22 rad scaled by target weight,
  MP forces the Easy 1.9× multiplier — `src/game/damage.hpp`). Under model A
  it resolves against lag-compensated positions, i.e. against what the
  shooter saw — which is the *correct* generalization of the original
  behavior, not a rule change. Under lockstep/rollback it would resolve
  against delayed/stalled inputs instead — strictly less faithful at real
  latencies.
- **30 Hz timing is preserved exactly** in model A (server ticks the real
  sim at `World::kTickHz`). Interpolation renders at display rate from
  snapshots but never alters tick outcomes.
- Lockstep preserves exactness only below ~50 ms with zero loss; past that
  it degrades into stutter-or-delay, which changes feel *more* than model
  A's smooth approximations. Rollback preserves local feel best for 2
  players but its corrections *are* feel changes (visible snaps), growing
  with player count.

Conclusion: **model A is the most faithful at 50–200 ms + loss**, because it
confines latency's effects to well-understood, bounded, standard perception
mechanisms while the rules stay bit-exact on the server.

## 3. Practical concerns

### NAT traversal: servers vs P2P

- **Client–server (model A): only the server needs a reachable address.**
  Clients make outbound UDP to the server's port — no NAT traversal on the
  client side. A user hosting behind NAT forwards one UDP port (proposed
  default 27500). This is the standard FPS arrangement (Quake/Source/
  Overwatch all assume reachable servers).
- **P2P (rollback C, lockstep-without-relay): every pair of players needs
  connectivity** — UDP hole punching per pair, which fails on symmetric NATs
  and needs a rendezvous server anyway, or TURN-style relays (bandwidth the
  project would have to host). The classical reference is Ford et al.,
  "Peer-to-Peer Communication Across Network Address Translators"
  (<https://bford.info/pub/net/p2pnat.pdf>, RFC 4787/5128 behavior discussion
  <https://www.rfc-editor.org/rfc/rfc5128.html>). Adopting P2P would make
  "playable online" depend on the hardest part of the problem. Model A
  sidesteps it entirely.
- Master server (`nfmaster`): a tiny self-hostable registry (servers
  heartbeat, clients query) plus LAN broadcast discovery and direct-IP
  connect. No NAT help needed beyond telling the server its external address.

### Cheating / trust

- **Model A (authoritative server)**: clients are dumb terminals with guns.
  The server validates everything: hits (range, line of sight, ammo state,
  fire-rate limits from `WeaponTable`), movement plausibility (speed caps
  from `PlayerParams`), input rate (one `PadState` per tick, no future
  ticks). Never trust client state — positions, health, ammo, and scores
  all come from the server snapshot. This is the Source/Overwatch posture.
- **Lockstep/rollback/relay**: every client runs the sim and (in rollback)
  every client sees every input — aimbots aside, wallhacks and sim tampering
  are unfixable in principle (Section 5). Bots must run *somewhere*; on a
  client means that client owns truth for 4 armed players.
- Our threat model is modest (cooperative community, self-hosted servers),
  but model A is the only one where server admins *can* enforce fairness.

### Hosting

- **Dedicated server** (`nfserver`, headless, map/mode rotation, admin
  console, config file): the primary path. Runs the exact sim + bots.
- **Listen server** (host from the MP setup menu): same binary logic
  embedded in `nightfire`, one local player + remote clients.
- **Browser**: LAN discovery (UDP broadcast) + direct IP + `nfmaster` list.
  All three are cheap over the Section-4 protocol.

### Scale

- Original PS2/GC-Xbox rulesets retain the original **8 participant slots**
  (four local humans plus four bots). The non-original Extended ruleset uses
  all 16 `kMpSlots`: any human/bot mix up to 16, with bots placed after the
  highest present human slot. Local split-screen setup and controller joins
  remain capped at four; online participants use the remaining slot records.
  Extended capacity is a project addition, not original-game behavior.
- Bots cost server CPU only (~0.5 ms/tick total for 8 slots — a dedicated
  server uses ~2% of one core for the sim; snapshots × clients dominate).

### Bandwidth (estimates, Skyrail-class map)

- **Model A upstream** (client→server): tick id (4 B) + current + 2 redundant
  `PadState`s (3 × 6 B) + ack mask ≈ 26 B + UDP/IPv4 overhead 28 B ≈ 54 B @
  30 Hz ≈ **1.6 KB/s per client**. Loss redundancy is the "last-N" scheme
  from the brief — right call at 5% loss.
- **Model A downstream** (server→client, 10–20 Hz snapshots): 8 players ×
  ~60 B (pos 12 + vel 12 + yaw/pitch 8 + health/armor 8 + weapon/ammo 8 +
  stance/anim/misc ~12) ≈ 480 B + pickups (28 × state+timer ≈ 60 B) +
  projectiles (usually 0; worst case ~16 × 24 B ≈ 384 B) + scores/match
  clock ≈ 64 B + events (sounds, kills, messages — variable, small) ≈
  **0.6–1.0 KB/snapshot → 6–20 KB/s per client**. Eight clients ≈ 160 KB/s
  server egress worst case. Home-hostable.
- **Lockstep/relay**: 8 × 6 B inputs + headers @ 30 Hz ≈ **2–3 KB/s** per
  client symmetric. Smallest wire cost, highest fragility cost.
- **Rollback P2P**: each client sends inputs to 7 peers ≈ 7 × upstream of
  model A, plus no server to amortize. Fine on wire, bad everywhere else.

### Reconnection / late join

- **Model A**: join = handshake (protocol version + game-data hash, Section
  "protocol" in the brief) → server sends full state (slots, scores, match
  clock, config, RNG-independent — the client's RNG only drives cosmetics)
  → client starts interpolating. Mid-match join works; rejoin works. No
  sim-state serialization required.
- **Lockstep/rollback**: late join needs the full sim snapshot (players,
  bots, arena timers, objectives, global RNG words) — Section 0's missing
  mechanism — plus input history from the join tick. Strictly harder.

## 4. The measured prototype that settles it

Section 0 *is* the prototype: the existing deterministic sim re-simulates a
full 8-slot match at ~2000 ticks/s. That single number decides two things:

1. **Prediction is free.** Re-simulating the local player's movement for
   reconciliation (a handful of ticks of one `Player`, no bots — bots stay
   server-side) is microseconds. Model A's only per-frame CPU risk is
   nonexistent.
2. **Rollback's CPU case is proven but irrelevant.** 6 ticks ≈ 3 ms says
   rollback *could* afford re-simulation — but rollback's true costs are
   per-tick full-sim snapshots, P2P NAT, no culling, and bot hosting, none
   of which CPU solves. The measurement removes the one argument that could
   have favored rollback on performance grounds and leaves all the
   structural arguments standing.

Server determinism acceptance (recorded inputs reproduce identical results)
falls out of the existing architecture: same binary + same input stream +
same tick order = same `game_rng()` draws = same match. Record inputs
server-side per match; replay through `nfgame --mp --inputs*` to verify.

## 5. Anti-wallhack interest management (server-side visibility culling)

### What the industry does (primary sources)

- **Valorant "Fog of War"** (Riot Games tech blog — the canonical writeup):
  the server only replicates an enemy's position if that enemy could be seen
  by (or affect) the client; everything else is never sent, so wallhacks
  have nothing to read:
  <https://www.riotgames.com/en/news/demolishing-wallhacks-valorants-fog-war>
  (follow-up story: <https://www.riotgames.com/en/news/story-fog-and-war>).
- **Counter-Strike (Source lineage)**: the engine's `CheckTransmit`/PVS
  system decides per-client entity transmission; CS2 community server work
  demonstrates the same "don't transmit occluded players" behavior via
  `sv_occlude_players`-style culling plugins (e.g.
  <https://github.com/njyeung/CS2FOW-pvs>). Caveat: Valve has published no
  CS2-specific tech blog on this; the mechanism is documented engine
  behavior plus observable cvar/plugin surface, not a first-party paper.
  AlliedModders threads (e.g. <https://forums.alliedmods.net/showthread.php?t=296194>)
  show the long community history of the same idea on CS:GO servers.
- **Call of Duty "server culling"**: announced for Modern Warfare 4-era
  anti-cheat as making wallhacks useless by not sending data the player
  can't see — reported via secondary press
  (<https://www.dexerto.com/call-of-duty/modern-warzone-server-culling-feature-will-make-wall-hacks-useless-3415049>,
  <https://www.destructoid.com/server-culling-anti-cheat-mw4-aims-to-kill-wallhacks/>);
  Activision's first-party Ricochet blogs document the anti-cheat program
  and its server-side direction generally
  (<https://www.callofduty.com/blog/2025/05/call-of-duty-black-ops-6-warzone-ricochet-anti-cheat-season-three-recap>,
  <https://www.callofduty.com/blog/2024/12/call-of-duty-ricochet-anti-cheat-update-december>)
  but (as of this writing) contain no dedicated "server culling" technical
  specification. Cite the feature as press-announced, the program direction
  as first-party.

Common principle (Riot states it explicitly): **never send what the client
can't legitimately use — can't hack what you never receive.** The rule is
"can see *or* can affect/hear": you must still send enemies whose gunfire,
footsteps, or abilities reach the player.

### What it costs us

- **Per-client LOS tests per tick**: worst case 8 clients × 7 others = 56
  segment-vs-world tests per tick. Our `CollisionWorld::ray()` runs against
  a BVH with per-query counters (`rays`, `triangles_tested` —
  `src/game/collision_world.hpp`); a single segment cast is microseconds.
  56/tick at 30 Hz is noise next to the 0.5 ms sim tick. Cost: negligible.
- **Conservative margins** (the hard part, gameplay not CPU): with 100–200 ms
  latency, a strict "visible now" test pops enemies in late around corners.
  Standard answers: expand the test (capsule radius around the enemy, test
  from both eye corners), and add a **grace window** — once visible, keep
  transmitting for ~0.25–0.5 s after occlusion (Riot describes similar
  hysteresis; CS plugins hold transmission briefly). Tune on Skyrail corners
  with the latency simulator, not by theory.
- **The side channels that also leak position** — culling positions while
  broadcasting these is theater:
  - **Radar**: our `MP_GetRadarObjects` reports *everything* in camera space
    (`src/game/arena.cpp:534`) plus names (`radar_names`). The original's
    radar is generous by design. Networked radar must be gated by the same
    visibility/audibility decision — a gameplay decision to document, not a
    bug: either gate blips by visibility (deviates from original, kills
    wallhack-via-radar) or keep original radar (faithful, leaks). Default
    recommendation: gate by visibility; note the deviation in `docs/net.md`.
  - **Footstep/gunfire audio**: positional `MatchSound`/`SoundEvent`s locate
    by construction. The correct rule (Riot's "or hear"): audibility
    *grants* transmission — if you can hear them you get their position
    anyway, so culling must treat "audible" as visible. Footsteps already
    have range attenuation semantics (`DroneAnim` footstep events,
    `Sound_Alertness` 50 m world-noise in `drone_system.hpp`); reuse those
    radii as the audibility gate.
  - **Kill feed / messages**: broadcast-after-death is standard and leaks
    only the (already known) killer/victim pair. Keep, but send
    killer/victim-targeted details first, public feed on the existing delay.
  - **Muzzle flash lights / tracers / viewmodels**: visual effects driven
    from *transmitted* state inherit culling automatically if the effect
    system only renders what the snapshot contains. Enforce that ordering.

### Interaction with each netcode model

- **Lockstep (B) and rollback (C)**: every client receives every input and
  simulates every player — **culling is information-theoretically
  impossible**. Any client can read all positions out of its own sim. This
  is the strongest structural argument for model A: only server-authoritative
  snapshots let the server withhold what a client must not know.
- **Input relay (D)**: same as lockstep — the relay forwards all inputs to
  all clients. No culling.
- **Model A**: culling is a per-client filter on snapshot construction
  (plus event gating). It composes with delta compression (cull first, then
  delta vs. acked) and with lag compensation (rewind uses *server* truth,
  unaffected by what any one client was sent).

### Authoritative validation (beyond culling)

Culling stops wallhacks; the rest of cheating needs the standard
authoritative posture, all cheap here:

- **Hit validation**: hitscan/melee/projectile hits resolve on the server
  from server state; validate range (weapon def), LOS at fire time, ammo
  consumed, fire-interval respected. Clients send *intents* (fire button in
  `PadState`), never outcomes.
- **Input sanity**: one input per tick per client, ticks monotonic, no
  future ticks beyond the jitter window; stick/button values are raw DS2
  ranges (0–255 / bitmask) so range-checks are trivial; sustained
  inhuman rates (e.g. 30 Hz perfect trigger oscillation) can flag/log.
- **Never trust client state**: health, armor, ammo, position, score, match
  clock, and RNG all originate server-side. The client's predicted position
  is a rendering convenience the server re-derives and may override.

## 6. Recommendation and concrete plan

**Recommendation: build model A** — server-authoritative 30 Hz snapshots
(delta-compressed vs. last acked) + redundant input packets + client-side
prediction (local movement only, re-simulated with the real `Player` code)
+ entity interpolation (~100 ms) + server-side lag-compensated hit
resolution (rewind capped ~200 ms, players only, original auto-aim/damage
rules untouched) + per-client visibility culling (phase 2) + full
authoritative validation — over UDP with a small reliability/ordering layer
for events (ENet via FetchContent, or hand-rolled; ENet preferred — boring,
proven, Quake-derived). This matches the brief's architecture. The brief
stands: implement it.

Why it wins *for this game*: exact rules on the server (bots included, no
bot protocol); best feel at 50–200 ms + 5% loss (smooth, bounded artifacts
vs. lockstep's stalls or rollback's snaps); trivial NAT/hosting story (one
port forward); natural late join; the only model compatible with culling;
prediction proven free by measurement; determinism retained and reused
(record/replay, exact re-sim) without being load-bearing across machines.

**Concrete plan** (matches the brief's change list):

1. **Phase 0 — `nf_net` transport + protocol**: versioned handshake,
   game-data hash check (hash `ACTION.ELF` + level `.bin`s so peers share
   disc data), connection/timeouts, UDP reliability for events, text chat,
   `--net-sim-loss` / `--net-sim-latency` fault injection from day one.
2. **Phase 1 — playable LAN match**: `nfserver` headless dedicated server
   (map/mode rotation, match end → next, stdin admin: kick/map/mode/bots/
   say/status, config file) + `nightfire --connect` client; snapshots of
   players/bots/pickups/projectiles/match/events; no prediction yet —
   prove consistency first (multi-process localhost scoreboard equality).
3. **Phase 2 — feel**: prediction/reconciliation, interpolation, lag
   compensation; latency/loss soak (2–4 clients + bots, 5 min, 100–200 ms,
   5% loss); screenshots of a networked match.
4. **Phase 3 — hardening**: visibility culling with grace window (tuned on
   real corners under simulated latency), radar gating decision documented,
   audibility grants transmission, hit/input validation.
5. **Phase 4 — lobby**: listen server in the MP setup menu, server browser
   (LAN broadcast + direct IP + self-hostable `nfmaster`), join flow,
   scoreboard/chat HUD from server state.
6. **Docs**: `docs/net.md` (hosting, ports, master server, slot-limit notes,
   radar deviation note). This file (`docs/net-research.md`) is the design
   record.

Slot honesty: legacy rulesets ship 4 local humans + up to 4 bots. Extended
snapshots carry the 16-slot roster and support up to 16 online humans/bots;
this capacity is non-original behavior.
