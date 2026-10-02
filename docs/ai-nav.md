# Navigation (AINetwork) — `nav_data`, `NavNetwork`, `NavAgent`

Reimplementation of the original's `AINetwork` (ACTION.ELF @0x2c7940): the AI path / boundary
network of map block `0x05`, the room ("cel") binding, MoveTest, A*, route following with link creep,
patrol/mission routes and the per-node distance fields ("emitters"). Spec:
`docs/spec-arena-ai.md` Part 2B; this file records what was verified against the pseudocode
(`build/{ida,ghidra}/action`), where the spec was corrected, and how to run the proofs.

| file | contents |
|---|---|
| `src/assets/nav_data.{hpp,cpp}` | on-disk parsers: block 0x05 (`AiNetworkData`), 0x19 (`PathTrack`, `StaticPathRef`), 0x21 portals, model boxes |
| `src/game/nav.{hpp,cpp}` | `NavNetwork`: cels, bounds, MoveTest, node search, A*, emitters, door/kick hooks |
| `src/game/nav_route.cpp` | routes: link creep, `SetupNextNode`, `CalcRoute`, `OptimiseRoute`, `NavAgent` (per-drone state), patrol/mission routes |
| `src/tools/nfdump_nav.{hpp,cpp}` | `nfdump validate` (nav part) and `nfdump <gamedir> nav <level.bin> [map.bmp]` |

## Using it

```cpp
NavNetwork nav(level, world.collision(), NavLimits::for_level(level_id_from_name("07000024.bin")));
nav.begin_frame(tick);                       // once per 30 Hz tick (Drone_InitComms: link flags + used-counts reset)
NavAgent a(nav); a.set_path_for(feet);       // drone+0x954
a.set_goal_position(goal, radius);           // AINetwork_SetupGoalPosition (radius 0 -> 2.0)
NavMove m = a.move_to_goal(feet);            // NDrone2_MoveToGoalPosition; m.status, m.waypoint, m.distance
```

`feet` is `NDrone2_FeetPos` (obj pos − (stand_height − 0.4)); node heights already include the +0.4 skin.
`NavMove::status` is Following / Arrived / CreepFailed for a running route, or the failure status of the route
calculation (NoTargetNodes 4, NoStartNode 5, Exhausted 6, NoPath 7, CannotCalc 8). Patrol and mission routes:
`assign_ai_path(feet, cel, pathflag::kPatrol|kMission)` then `follow_ai_path(feet)` (events for wait nodes are in
`NavMove::reached_special/reached_node`; `set_near_pos_callback` answers `NDrone2_DroneNearPos`). Pickup /
objective emitters: `init_emitter` + `emit_path`, then `NavAgent::distance_to_emitter` (with the nearest-node cache
that `BOTSTATE_setNearestNavNode` invalidates every tick via `invalidate_nearest_node`) and
`emitter_node_at_distance` (flee search). Doors / kicks are data hooks: `bind_door_nodes`, `bind_kick_nodes`,
`set_door_locked_callback`, `set_link_flags_in_circle` (dynamic danger areas, flag 0x100).

Nodes with several routes: `begin_frame` zeroes every link's used-count each tick and `follow_route` adds the
remaining route, so A* (cost `length * (used + 1)`) spreads simultaneous routes over parallel links (crowd cost).

## Cels (rooms) — `build_FindCel`

`build_FindCel(pos, glb_world)` @0x1c8ff0 walks the world cel list (newest first) for cels whose `cel+0x3c & 0x40000`,
and keeps those whose box (`cel+0x40..0x58`) contains the point. The flag is set by `parseentity_fixup_entity`
for statics of class **0xc023 and 0xc047** (`0x804c023`, `0x804c047`); the box is copied unchanged from the model
(`entity_params` file offsets `+0x1c..+0x24` min, `+0x28..+0x30` max — only the *sphere centre* is transformed by
`parseentity_transform_bounding_box`, the box is not) and room cels sit at the origin (their position fields are zeroed),
so the box is a world-space AABB. Skyrail has 49 rooms (`Terrain*`, `Hall`, `Stairs*`, `Lift*`, ...); the small MP arenas have 1..28.
If several boxes contain the point the original casts +-256 y rays into each candidate's collision (range shrinks
with every hit, strict `<` on the nearest hit, first candidate if nothing is hit); `NavNetwork::find_cel`
does the same with `CollisionWorld::ray_hits` filtered to the candidate's placement (one hit per placement).

Node/bounds binding (`AIPath_BindNodes`) is done in the constructor: nodes/blinks are pushed onto per-cel lists with
head insertion (walk order = reverse creation order, which decides ties in the node search).

**Straddle (`Collide_StraddleCels`)** decides which cels' boundary lists a MoveTest looks at. It floods from the start
cel across *portals* (map block 0x21: `u32 hdr, u32 count`, 56-byte records `4 x vec3 quad, u16 modelA, u16 modelB,
u16 flags, u16 pad`; each portal connects the cels built from those two models, both directions, each pair tested once).
`Intersect_Portal` for the ray hittest 0x201 tests both triangles (0,1,2),(2,3,0) of the quad
with `Collide_RayTriangle`, which intersects the **infinite line** through the (y+1.0) segment (the original never
range-checks the ray parameter); we reproduce that literally. `BoundsTest` returns 0 unless the destination cel is in the list.
(Data check: portal blocks parse with `size == 8 + 56 * count` in every map chunk, 855 portals.)

## Corrections / additions to the spec (verified in pseudocode)

* `AINetwork_BoundsNodeTest`: threshold `t = r^2 * 0.5` **except** `t = r^2` when the post is >= 1.3 r^2 from *both*
  segment ends (spec said "far from `to`, near `from`"). Both posts compare `|closest.y - nodeA.y| < 2.0`. `LinkCreep_Handler`
  r = 0.2, `Increment` 0.4, `Decrement` 0.1.
* `BoundsTest` corner-post test (only `CalcRoute`'s straight-line test and `OptimiseRoute` enable it, and only without a
  hit-out): skipped for the whole link when either end lies within 0.4 of either post; second post uses the
  *unsquared* 2-D distance `< 0.16` and node A's y (the decompilation is explicit; kept).
* `vecutil_intersect_line_line_2d` returns 1 (no point written) for nearly parallel overlapping segments; we treat that as a
  touch at the segment start. The crossing point's y is the segment's y at the crossing.
* Route field `+0x90` is the **creep look-ahead** (`NDrone2_DefaultInit`: 2.0, 4.0 when `drone+0x23`, 8.0 for type 0xd): the
  waypoint advances one 0.4 step only while the owner is closer than this to the next waypoint. `+0x8c` is the arrival radius
  (`NDrone2_MoveToGoalPosition` additionally arrives below `drone+0x8ec` = 2.0).
* `EmitPath`, `Emitter_GetNodeAtDistance`: `GetNodeAtDistance` is a breadth search over an open list sorted by the nodes'
  *stale* `f` (it never sets it) and returns the first neighbour whose table byte is `>= minDist` (0xff counts); the spec's
  "never move to lower nodes" filter is a no-op in the code.
* `SetLinksFlagInCircle` covers every path (no plain filter), needs |dy| <= 2 and uses the closest point on the link.
* `NDrone_InitDoorNodes` (flag 2 -> nearest type-0x1c object, flag cleared if none; link flag 0x20 + boundary 0x800 when
  the door-door link crosses a boundary) and `NDrone_InitKickNodes` (flag 4, 0x40 / 0x1000, flag never cleared).
* MoveTest limits per level (`Drone_PostLoad_Init`): slope 0.7854 / dy 1.5 on level 0x07000008, else 0.8727 / 2.0; max |dy| 4.0,
  max length^2 900.
* Emitters: `EmitPath` range is 255 world units (table byte 0xff = unreachable). Only emitters near the graph centre reach
  every node of the larger levels (Skyrail node 0 reaches 281/336); the validate check requires that *some* node's
  emitter reaches every node on each MP level.
* NavPath `plain()` = `(flags & 0xd) == 0`; patrol paths (flag 4) start a loop route (mode 2), everything else mode 1;
  `InitAIPath` walks degree-2 chains away from the previous node (patrol starts with the second link unless path flag 0x10).
* block 0x19 (612 blocks, 13328 records) lives in the level's **map** chunk only; `static_path_refs` exposes the u16 at
  static offsets +0x40 (count), +0x42, +0x44 (index into the block list). Quaternions are unit length (max error 1.2e-7).

## Verification

`nfdump <gamedir> validate` (nav part, 0 failures):

```
nav: 28 levels with block 0x05, 612 path_data blocks (13328 records, 612 static refs, max |q|-1 1.2e-07), 855 portals
nav runtime: 7 MP levels: 1546 links (27 blocked by a MoveTest limit or boundary crossing, 0 unexplained failures,
  0 nodes outside rooms), emitters ok on 7, 82/82 routes walked to the goal, longest link 37.78, 0 failures
nav patrol/mission paths: 77, 77 assigned and followed (0 nodes outside rooms)
```

It asserts: block records end exactly at the block end, sequential indices, links in range, version 8, 28 levels /
612 blocks / 13328 records, `(size-4) % 28 == 0`, unit quaternions, single component and bound nodes on every MP nav path,
every blocked MP link explained (|dy| > 4, length > 30, slope or boundary crossing), an emitter that reaches every node,
route + link-creep walks (on-node and off-node ends) that arrive, and every patrol/mission path assigning at a node and
following (once-routes arrive, loops advance).

`nfdump <gamedir> nav 07000024.bin out.bmp` (Skyrail): 336 nodes / 400 links, 715 bounds nodes / 713 links, one component, link
length 1.20..26.15 avg 6.57, max degree 5; MoveTest along links 391 ok / 9 blocked (all 9 exceed the 4.0 dy limit — the original
fails them too; A* does not MoveTest links); far pair node 2 -> node 80: A* 24 route nodes, route length 259.3 (1.30 x straight);
the link-creep walk (an agent that only steers at the returned waypoint at 0.25 units/tick) arrives after 1013 ticks having
walked 253.2 units; 100/100 random node pairs and 100/100 off-node point pairs arrive. The BMP shows the boundary loops (red),
nav links (grey) and the route (yellow).

## Known gaps

* Straddle uses room boxes + portals; `NavNetwork::node_search` retries the query cel's portal neighbours
  (vertical span overlapping the query, first hit wins) like the original's per-cel `AINodeSearch` neighbour
  loop; `NavNodeCache`/`NearestNodeDCV` variants are folded into `NavAgent::nearest_node` /
  `NavNetwork::nearest_node`.
* `LinkCreep_Handler`'s drone-side effects (`drone+0x31c` bits, behaviour 0x36) are the drone layer's; `NavRoute::ignore_bounds` stands for
  the `drone+0xba8 != 0` shortcut.
* `Door_IsLocked` is a callback (`set_door_locked_callback`) — doors are SP-only objects.
* The ordinal fields (`+2`) of nodes/links and the reserved words of block 0x05 records are parsed but unused, as in the original.
