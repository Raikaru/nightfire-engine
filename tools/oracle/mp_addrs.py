"""MP ground-truth address map (ACTION.ELF USA, SLUS-20579).

Single source of truth for tools/oracle/mp_*.py. Most addresses were probed live
over PINE on 2026-10-02; [SPEC] denotes spec-derived addresses and [IDA] denotes
offsets recovered from ACTION.ELF pseudocode/disassembly but not yet live-probed. Struct notes cite the source.
"""

# ---- global timebase -------------------------------------------------------
GAMESTATE = 0x2A3768
GS_DONE = GAMESTATE + 0x30      # ++ at END of each logic update
GS_FRAME = GAMESTATE + 0x34     # separate counter; live-diverges from +0x3C, not an MP frame key
GS_FRAME_START = GAMESTATE + 0x3C  # ++ at START of each logic update
FRAME_RATE = 0x30D0D0          # float, 60 / vsyncs per logic frame
FRAME_RATE_INT = 0x30D0CC       # int, 30 or 60
FRAME_RATE_MUL = 0x30D0D8       # float, 60 Hz normalized per-frame scale
REC_FRAME_RATE = 0x30D0DC       # float dt
VBLANK_COUNT = 0x30CA4C         # [ACTION.ELF] incremented by VBlankInt__Fi
REAL_TIME_COUNT = 0x30C714     # [ACTION.ELF] psiInitTimeIn100ths__Fv baseline

# ---- RNG (bit-exact stream, src/core/rng.hpp) -------------------------------
RNG_X = 0x30D0A0
RNG_Y = 0x30D0A4
RNG_NWORDS = 4

RNG_WORDS = 0x30D0A0           # 4 u32 reads (X, Y, +8, +0xC) [SPEC: 4 words]
MPSETTINGS = 0x2A47A0          # 0x1dc bytes (spec 1.3)
MPGAME = 0x2A4980              # 0x1d0 bytes (spec 1.4)
MP_SLOT_STRIDE = 0x30
MP_NSLOTS = 8
# MPSettings offsets
MPS_MP_ACTIVE = 0x180
MPS_TEAMS = 0x18C
MPS_OBJECTIVE = 0x190
MPS_PARTICIPANTS = 0x194
MPS_FRIENDLY_FIRE = 0x198
MPS_SCORE_LIMIT = 0x19C
MPS_TIME_LIMIT_MIN = 0x1A0     # minutes in menu, seconds after confirm
MPS_SCENARIO_MASK = 0x1A4
MPS_MAP = 0x1A8
MPS_HUMANS = 0x1AC
MPS_BOTS = 0x1B0
MPS_WEAPON_SET = 0x1B4
MPS_PICKUP_COUNT = 0x1D8       # u16 live MPpickups entries
# MPGame slot offsets
MPG_KILLS = 0x04
MPG_DEATHS = 0x08
MPG_STREAK = 0x10
MPG_POINTS = 0x18             # float
MPG_OBJ = 0x1C                # obj_tag*
MPG_LAST_ATTACKER = 0x20       # i16
MPG_FF_COOLDOWN = 0x22        # u16
MPG_COUNTDOWN = 0x24          # u16
MPG_STATUS = 0x26             # u16
MPG_LAST_KILLER = 0x28        # i16
# MPGame globals
MPG_TEAM0 = 0x180              # float team scores
MPG_TEAM1 = 0x184
MPG_STATE = 0x188              # 0 running 1 score 2 time 3 hold 4 results 5 idle 6 restart
MPG_BEST = 0x18C
MPG_ELAPSED = 0x190            # float seconds
MPG_LIMIT = 0x194              # float time limit seconds
MPG_TOTAL = 0x19C              # float total seconds (pickup-visit clock)

# ---- players / bots ---------------------------------------------------------
GLB_PLAYERS = 0x2D88E0         # obj_tag*[4] humans
# obj_tag (verified live 2026-10-02)
OBJ_CELL = 0x20                # cel_tag* used by spatial culling
CELL_RAW_SIZE = 0xA0
OBJ_POS = 0x30                 # vec3
OBJ_POS2 = 0x40                # vec3 copy
OBJ_YAW = 0x54                 # f32 rad, forward = (sin, 0, cos)
OBJ_COLL = 0xDC                # collbody* (humans; +0xCC foot height, +96 aim, +98 weapon)
OBJ_BL = 0xE0                  # BLData*
OBJ_STAMP = 0xEC               # i32 spawn/death frame
OBJ_STATE = 0xF4               # u16: humans 1 alive; bots = drone state id
OBJ_TYPE = 0xFF                # u8: 2 bot 3 human 0x11/0x12 dead 0x2f pickup

# DynamicObjList (ACTION.ELF control_* assembly) and bullet object payload
DYNAMIC_OBJ_LIST = 0x2705A0   # [IDA] head at +0x14; nodes are doubly linked
OBJ_LIST_NEXT = 0x14
OBJ_CUSTOM_DATA = 0xE0        # obj_tag* -> BU_tag for type-5 bullets
BULLET_RAW_SIZE = 0x108       # [SPEC] through BU+0x104 in spec-weapons.md §7.1
BULLET_DIR = 0x00             # vec3
BULLET_OWNER = 0x30           # obj_tag*
BULLET_TARGET = 0x40          # obj_tag* homing target
BULLET_WEAPON_DEF = 0x44      # weapon_definition_tag*
BULLET_TRAVELLED = 0xF4       # f32
BULLET_SPEED = 0xF8           # f32
BULLET_TIMER = 0xFC           # f32
BULLET_BOUNCES = 0x100        # u16
BULLET_IN_AIR = 0x104          # u8
WEAPON_DATA = 0x2BF150        # [SPEC] 115 x 268-byte weapon_definition_tag table
WEAPON_DEF_STRIDE = 0x10C
# BLData (verified)
BL_HEALTH = 0x894              # f32 humans
BL_ARMOUR = 0x8B0              # f32 humans
BL_PITCH = 0x8A8               # f32 humans, units of pi/2
BL_AUTOTARGET = 0x114         # obj_tag* held by Player_AutoAim / Check_AutoAim
# BLData feedback fields ([IDA] ACTION.ELF offsets; pending live confirmation)
BL_DAMAGE_FLASH = 0x8BC      # f32, regular pain flash (Player_HandlePain)
BL_FADE_TOTAL = 0x918         # f32, flash-bang total duration
BL_FADE_TIMER = 0x91C         # f32, flash-bang remaining duration
BL_FADE_COLOUR = 0x963       # u8, flash-bang color
BL_PAIN_DIR = 0x967           # u8, Player_HandlePain
BL_PAIN_ALPHA = 0x968         # u8, HUD damage overlay alpha
# collbody (verified; +0x98 is a pointer-looking 4-byte field, not a weapon ID)
CB_AIM = 0x96                  # u8 aim bit
CB_98 = 0x98                   # 4-byte field; target type not identified
CB_FOOT = 0xCC                 # f32 animated foot height

# ---- bot brain (spec Part 2, §1.5/1.6; slot layout verified, rest [SPEC]) ---
BOT_VARS = 0x26D660            # 4 x 0x780, index = slot-4
BOT_VARS_STRIDE = 0x780
BOT_GOAL0 = 0x000              # 2 x 0x50 goal records
BOT_STATS = 0x0A0              # 14 B copy; +0xA4 u16 maxhp, +0xAA weappref, +0xAB personality
BOT_OTHER = 0x0B0              # 8 x 0x10 perception cache
BOT_WEAPONS = 0x140            # 0x55 x 0xC weapon records (clip u16 +4, has u8 +6)
BOT_RESERVE = 0x698            # 0x21 x u16 ammo reserve
BOT_DISTRACT = 0x728           # f32
BOT_DRONE = 0x750              # ptr &MPSettings[slot] at +0x750, Drone* at +0x754
BOT_PIDX = 0x75C               # s16 player index
BOT_BIDX = 0x75E               # s16 bot index
BOT_NODE = 0x760               # u16 nearest nav node
BOT_GOALSLOT = 0x765           # u8 active goal slot
BOT_STYPE = 0x766              # u8 current state type
BOT_CURWEAP = 0x768            # u8
BOT_ARMOUR = 0x769             # u8
BOT_TRAIT = 0x76B              # s8 preferred trait opponent slot
# Drone struct (via BOT_vars+0x754; +0xd1c back-pointer verified)
DRONE_BOTVARS = 0xD1C
DRONE_HEALTH = 0xAC            # f32 bot health
DRONE_LASTDMG = 0x150          # f32 last damage taken
DRONE_ANIM_SCRIPT = 0x530        # sAnimScript_tag* (dynamic heap object)
# goal record (0x50)
GOAL_POS = 0x00                # CelPos 0x20
GOAL_DISTRACT_LIM = 0x20       # f32
GOAL_TSET = 0x24               # f32 clock when set
GOAL_TIMEOUT = 0x28            # f32
GOAL_TARGET = 0x3C             # ptr
GOAL_RETSTATE = 0x40
GOAL_TYPE = 0x45               # 0 none 1 pickup 2 objective 3 chase
GOAL_KIND = 0x4A               # 0 pickup 1 flag 2 base 3 GE 4 bp 5 base 6 uplink 7 hill 8 demo 9 chase

# ---- pickups (spec 1.10) ----------------------------------------------------
MPPICKUPS = 0x2A4B50           # 64 x 0xA0
MPPICKUP_STRIDE = 0xA0
MPPICKUP_VISIT = 0x80           # float visit-until seconds for bot indices 0..3
MPPICKUP_POS = 0x10            # vec4
PICKUPINFO_OFF = 0xE0          # obj+0xE0 for type-0x2f objs
PI_STATE = 0x20                # s16 0 settling 1 active 2 respawning
PI_CAT = 0x22                  # u16 category
PI_ITEM = 0x24                 # u16 item/weapon id
PI_RESPAWN_UNITS = 0x2C     # u16 units of 10 s; 0 one-shot, 0xffff never removed
PI_LIFETIME_FRAMES = 0x2E   # u16 lifetime countdown for dropped items (0 = infinite)
PI_AMOUNT = 0x26              # u16 item quantity (PICKUPINFO+0x26)
OBJ_FLAGS = 0xF0              # bit 0x10 hides dropped pickup from radar
PI_INDEX = 0x30                # s16 MPpickups index

# ---- objectives (spec 1B; ext blobs for the recorder) -----------------------
MPOBJECTS = 0x2A47D0           # obj*[64] (overlaps MPSettings tail; see spec)
FLAGS = 0x317210               # 2 x 0x90
BASES = 0x317330               # 2 x 0x90
UPLINKS = 0x317450             # 8 x 0x90
DEMOLITION = 0x3178D0          # 0x90
PROTECTION = 0x317C60          # 0x90
GOLDENEYE = 0x317FF0           # 4 x 0x90 (key, crystal, effect-handle, target)
GOLDENEYE_EFFECT = GOLDENEYE + 2 * 0x90  # effect handle in first word
GOLDENEYE_TARGET = GOLDENEYE + 3 * 0x90  # active strike target obj*
MP_ASSASSINATION_TARGET = 0x30D770       # Target obj* ("Assasin" ELF symbol)
MP_ASSASSIN = 0x30D774                  # Assassin obj* (adjacent unlabeled word)
BLUEPRINT = 0x318830           # 0x90
ESPONAGE_BASE = 0x3188C0       # 2 x 0x90
HILL = 0x318CE0                # 0x90
SPAWNPOINTS = 0x2A7350         # 64 x 0x30
SWITCH_FD = 0x26FD8D           # score-limit channel
SWITCH_FE = 0x26FD8E           # time-up channel

# ---- menu / unlock ----------------------------------------------------------
MENU_UNLOCK_EVERYTHING = 0x30D2A7  # u8 cheat flag; bypasses scenario row checks
SP_LEVEL = 0x2DF2E0            # 12 x 0x18 SP mission unlock rows, +0x10 flag

# ---- input (trace.py compat) ------------------------------------------------
PLAYER_SETTING_STRIDE = 0x158       # 4 per-controller PlayerSetting entries
TSLOT_STRIDE = 0x180                 # per-port tSlot; verified Input_Init offsets
OBJ_SUBSTATE = 0xF6                 # u16 player substate
BL_RAW_SIZE = 0x970                 # through BLData+0x968 feedback byte
CB_RAW_SIZE = 0xD0                  # full collbody sample used by trace.py
DRONE_RAW_SIZE = 0xD20              # through bot_vars back-pointer at +0xD1C
TSLOT0 = 0x245680
TSLOT_PADW = 0x122              # Sony button word (active-high)
TSLOT_STICKS = 0x128           # rx ry lx ly post-deadzone
PLAYER_SETTING = 0x2A38C8

# ---- MP maps ----------------------------------------------------------------
MP_MAPS = [
    (0x07000024, "Skyrail"),
    (0x07000027, "Fort Knox"),
    (0x07000029, "Snow Blind"),
    (0x07000026, "Phoenix Base"),
    (0x07000023, "Atlantis"),
    (0x07000028, "Missile Silo"),
    (0x07000025, "Sub Pen"),
    (0x0700004B, "Ravine"),
]
# scenario wheel order (down from Quick Game), masks in spec 1.2
MP_SCENARIOS = [
    (0x00000000, "Quick Game"),
    (0x00000001, "Arena"),
    (0x20000002, "Team Arena"),
    (0x20000004, "Capture The Flag"),
    (0x60000008, "Uplink"),
    (0x00000010, "Top Agent"),
    (0x20000040, "Demolition"),
    (0x20000080, "Protection"),
    (0x20000100, "Industrial Espionage"),
    (0x20000200, "GoldenEye Strike"),
    (0x00000400, "Assassination"),
    (0x40000800, "King of the Hill"),
    (0x60001000, "Team King of the Hill"),
]

# PINE savestate slots owned by the MP oracle (1-9 belong to Movement-2).
MP_SLOT_FIRST = 10
