#!/usr/bin/env python3
"""Match GC / Xbox functions to PS2 symbols from ghidra_export.py feature dumps.

Evidence (each accepted pair records method + confidence; see docs/xref.md):
  str      unique shared string literal(s)                   high (>=2 strings) else medium
  strset   identical set of shared strings, unique           high (>=2 strings) else medium
  bsim     GC BSim tables (gc/*_matches.tsv), sim>=0.7       high if sim>=0.9 else medium
  call/data/const/weak  summed votes from call-sequence gaps, mapped globals, rare
           constants and weak BSim rows (Matcher.propagate)  high at total>=2 else medium
  order    gap between anchors adjacent on both sides         medium
  via-gc   PS2->GC->Xbox composition (seed for PS2<->Xbox)   weaker of the two legs
A pair is only accepted as the unique mutual best with a plausible size; audit()
removes pairs whose call-graph neighbours point at a better partner.

Usage: match.py [action|driving]   writes $XREF/matches/<a>__<b>.tsv and prints stats.
"""
from __future__ import annotations

import bisect
import collections
import csv
import json
import os
import pathlib
import statistics
import sys

HOME = pathlib.Path.home()
PS2 = pathlib.Path(os.environ.get("NIGHTFIRE_PS2", HOME / "Projects/nightfire-ps2"))
XREF = PS2 / "build" / "xref"


class Side:
    def __init__(self, prog: str):
        self.prog = prog
        data = json.loads((XREF / "feat" / f"{prog}.json").read_text())
        self.funcs = {f["addr"]: f for f in data["funcs"]}
        self.addrs = sorted(self.funcs)
        self.callees: dict[int, list[int]] = {}
        self.callers: dict[int, set[int]] = collections.defaultdict(set)
        for a, f in self.funcs.items():
            seq, seen = [], set()
            for c in f["calls"]:
                if c in self.funcs and c != a and c not in seen:
                    seen.add(c)
                    seq.append(c)
            self.callees[a] = seq
            for c in seq:
                self.callers[c].add(a)
        self.strings = {a: set(f["strings"]) for a, f in self.funcs.items()}
        self.consts = {a: clean_floats(f["floats"]) | {("i", v) for v in f["ints"]} for a, f in self.funcs.items()}
        self.drefs = {a: list(dict.fromkeys(f["drefs"])) for a, f in self.funcs.items()}
        self.dref_users: dict[int, set[int]] = collections.defaultdict(set)
        for a, ds in self.drefs.items():
            for d in ds:
                self.dref_users[d].add(a)

    def name(self, a: int) -> str:
        return self.funcs[a]["name"]

    def ninstr(self, a: int) -> int:
        return max(1, self.funcs[a]["ninstr"])


class Matcher:
    """A = PS2 side (names), B = other platform."""

    def __init__(self, A: Side, B: Side):
        self.A, self.B = A, B
        self.ab: dict[int, int] = {}
        self.ba: dict[int, int] = {}
        self.meta: dict[int, tuple[str, str, str]] = {}  # a -> (method, conf, evidence)
        self.ratio_lo, self.ratio_hi = 0.2, 5.0
        self.median_ratio = 1.0
        self.rejected = collections.Counter()
        self.gab: dict[int, int] = {}  # global variable map (PS2 addr -> B addr)
        self.gba: dict[int, int] = {}
        self.banned: set[tuple[int, int]] = set()

    # ---------------------------------------------------------------- helpers
    def add(self, a: int, b: int, method: str, conf: str, ev: str = "") -> bool:
        if a in self.ab or b in self.ba or (a, b) in self.banned:
            return False
        self.ab[a] = b
        self.ba[b] = a
        self.meta[a] = (method, conf, ev)
        return True

    def audit(self) -> int:
        """Drop non-string pairs when the call graph points at a better partner.

        support(a,b) = matched callers of a whose partner calls b (and vice versa).
        alternative  = the function most often called by the partners of a's
        matched callers instead of b (or, symmetrically, by the partners of b's
        callers instead of a).  A pair is dropped when an alternative has >= 2
        votes and strictly more than its support -- typical for near-identical
        siblings (e.g. PlayerAnimSetInitNormal/Crouch) that BSim or a gap vote
        swapped.  Callee-side differences are ignored: platform helpers (VU0 asm
        on PS2, inlining on MSVC) make them noisy.  Dropped pairs are banned so
        propagation cannot re-add them.
        """
        A, B = self.A, self.B
        callees_a = {a: set(v) for a, v in A.callees.items()}
        callees_b = {b: set(v) for b, v in B.callees.items()}
        removed = 0
        while True:
            drop = []
            for a, b in self.ab.items():
                if self.meta[a][0] in ("str", "strset"):
                    continue
                sup_a = sup_b = 0
                alt_b, alt_a = collections.Counter(), collections.Counter()
                for ca in A.callers.get(a, ()):
                    cb = self.ab.get(ca)
                    if cb is None:
                        continue
                    cs = callees_b.get(cb, set())
                    if b in cs:
                        sup_a += 1
                    # callees already explained as partners of ca's other callees are siblings, not rivals
                    explained = {self.ab[x] for x in callees_a.get(ca, ()) if x in self.ab}
                    alt_b.update(y for y in cs - explained - {b} if self.size_sim(a, y) >= 0.5)
                for cb in B.callers.get(b, ()):
                    ca = self.ba.get(cb)
                    if ca is None:
                        continue
                    cs = callees_a.get(ca, set())
                    if a in cs:
                        sup_b += 1
                    explained = {self.ba[y] for y in callees_b.get(cb, ()) if y in self.ba}
                    alt_a.update(x for x in cs - explained - {a} if self.size_sim(x, b) >= 0.5)
                best = max(max(alt_b.values(), default=0) - sup_a, max(alt_a.values(), default=0) - sup_b)
                worst_alt = max(max(alt_b.values(), default=0), max(alt_a.values(), default=0))
                if worst_alt >= 2 and best > 0:
                    drop.append((best, a, b))
            if not drop:
                break
            # remove the worst offender(s) first; their removal may clear others
            drop.sort(reverse=True)
            worst = drop[0][0]
            for score, a, b in drop:
                if score < worst:
                    break
                self.rejected["audit:" + self.meta[a][0]] += 1
                del self.ab[a], self.ba[b], self.meta[a]
                self.banned.add((a, b))
                removed += 1
        return removed

    def size_ok(self, a: int, b: int, slack: float = 1.0) -> bool:
        na, nb = self.A.ninstr(a), self.B.ninstr(b)
        if abs(nb - na * self.median_ratio) <= 4:  # tiny functions: ratios are noise
            return True
        r = nb / na
        return self.ratio_lo / slack <= r <= self.ratio_hi * slack

    def calibrate(self) -> None:
        """Instruction-count ratio bounds from high-confidence pairs (all pairs if <50)."""
        def ratios(pred):
            return sorted(self.B.ninstr(b) / self.A.ninstr(a) for a, b in self.ab.items()
                          if pred(a) and self.A.ninstr(a) >= 8)
        rs = ratios(lambda a: self.meta[a][1] == "high")
        if len(rs) < 50:
            rs = ratios(lambda a: True)
        if len(rs) >= 50:
            lo = rs[int(len(rs) * 0.03)]
            hi = rs[int(len(rs) * 0.97)]
            self.ratio_lo, self.ratio_hi = lo * 0.8, hi * 1.25
        self.median_ratio = statistics.median(rs) if rs else 1.0

    def accept_votes(self, votes: dict) -> int:
        """votes[(a,b)] = Counter(source -> weight).  Accept unique mutual best, total >= 1."""
        tot = {k: sum(c.values()) for k, c in votes.items()
               if k[0] not in self.ab and k[1] not in self.ba}
        best_a = collections.defaultdict(list)
        best_b = collections.defaultdict(list)
        for (a, b), w in tot.items():
            best_a[a].append((w, b))
            best_b[b].append((w, a))
        n = 0
        for (a, b), w in sorted(tot.items(), key=lambda kv: -kv[1]):
            if a in self.ab or b in self.ba or w < 1:
                continue
            method = "+".join(sorted(s for s, v in votes[(a, b)].items() if v > 0))
            ca = sorted(best_a[a], reverse=True)
            cb = sorted(best_b[b], reverse=True)
            if ca[0][1] != b or cb[0][1] != a:
                continue
            if (len(ca) > 1 and ca[1][0] >= w * 0.75) or (len(cb) > 1 and cb[1][0] >= w * 0.75):
                self.rejected["vote:tie"] += 1
                continue
            if not self.size_ok(a, b):
                self.rejected["vote:size"] += 1
                continue
            if w < 2 and "str" not in votes[(a, b)] and self.size_sim(a, b) < 0.5 \
                    and abs(self.B.ninstr(b) - self.A.ninstr(a) * self.median_ratio) > 4:
                self.rejected["vote:size-weak"] += 1  # single weak vote: demand similar size
                continue
            conf = "high" if w >= 2 else "medium"
            ev = " ".join(f"{s}={v:g}" for s, v in sorted(votes[(a, b)].items()) if v > 0)
            if self.add(a, b, method, conf, ev):
                n += 1
        return n

    # ---------------------------------------------------------------- evidence
    def by_strings(self) -> int:
        A, B = self.A, self.B
        sa = collections.defaultdict(set)
        sb = collections.defaultdict(set)
        for a, ss in A.strings.items():
            for s in ss:
                sa[s].add(a)
        for b, ss in B.strings.items():
            for s in ss:
                sb[s].add(b)
        shared = set(sa) & set(sb)
        votes = collections.Counter()
        for s in shared:
            if len(sa[s]) == 1 and len(sb[s]) == 1:
                a, = sa[s]
                b, = sb[s]
                votes[(a, b)] += 1
        n = self.accept_votes({k: collections.Counter(str=v) for k, v in votes.items()})
        # string-set signatures among still-unmatched functions
        siga, sigb = collections.defaultdict(list), collections.defaultdict(list)
        for a, ss in A.strings.items():
            k = frozenset(ss & shared)
            if k and a not in self.ab:
                siga[k].append(a)
        for b, ss in B.strings.items():
            k = frozenset(ss & shared)
            if k and b not in self.ba:
                sigb[k].append(b)
        for k, la in siga.items():
            lb = sigb.get(k)
            if lb and len(la) == 1 and len(lb) == 1 and self.size_ok(la[0], lb[0], 2.0):
                conf = "high" if len(k) >= 2 else "medium"
                n += self.add(la[0], lb[0], "strset", conf, f"{len(k)} strings")
        return n

    def by_bsim(self, rows: list[tuple[int, int, float, float]]) -> int:
        n = 0
        for a, b, sim, sig in sorted(rows, key=lambda r: -r[2]):
            if a not in self.A.funcs or b not in self.B.funcs:
                self.rejected["bsim:nofunc"] += 1
                continue
            if self.ab.get(a) == b:
                continue
            if a in self.ab or b in self.ba:
                self.rejected["bsim:conflict"] += 1
                continue
            conf = "high" if sim >= 0.9 else "medium"
            n += self.add(a, b, "bsim", conf, f"sim={sim:.2f} sig={sig:.1f}")
        return n

    def propagate(self, weak: dict | None = None) -> int:
        """Iterate multi-source voting until no new pair is accepted.

        Sources (weight per observation):
          call   callee-sequence gap with one unmatched callee each side (1),
                 equal-length multi gaps (0.5), unique unmatched caller (1)
          data   global variable mapped via matched functions, referenced by exactly
                 one unmatched function on each side (1)
          const  float/int constant unique on both sides among unmatched (0.5 each, max 1.5)
          weak   externally supplied weak pairs, e.g. BSim 0.5<=sim<0.7 (0.5)
        A pair needs total >=1 and must be the unique mutual best; >=2 is "high".
        """
        A, B = self.A, self.B
        total = 0
        while True:
            votes: dict = collections.defaultdict(collections.Counter)
            for a, b in list(self.ab.items()):
                sa, sb = A.callees.get(a, []), B.callees.get(b, [])
                if sa and sb:
                    self._align_votes(sa, sb, self.ab, self.ba, votes, "call", sized=True)
                ua = [x for x in A.callers.get(a, ()) if x not in self.ab]
                ub = [y for y in B.callers.get(b, ()) if y not in self.ba]
                if len(ua) == 1 and len(ub) == 1:
                    na, nb = len(A.callers.get(a, ())), len(B.callers.get(b, ()))
                    if abs(na - nb) <= max(1, na // 3):
                        votes[(ua[0], ub[0])]["call"] += 1
            self._data_votes(votes)
            self._const_votes(votes)
            for (a, b), w in (weak or {}).items():
                if a not in self.ab and b not in self.ba:
                    votes[(a, b)]["weak"] += w
            n = self.accept_votes(votes)
            total += n
            if n == 0:
                break
        return total

    def size_sim(self, a: int, b: int) -> float:
        s = self.B.ninstr(b) / (self.A.ninstr(a) * self.median_ratio)
        return min(s, 1 / s)

    def _align_votes(self, sa, sb, ab, ba, votes, src, sized=False) -> None:
        posb = {y: j for j, y in enumerate(sb)}
        anchors = []
        for i, x in enumerate(sa):
            y = ab.get(x)
            if y is not None and y in posb:
                anchors.append((i, posb[y]))
        anchors = lis(anchors)
        bounds = [(-1, -1)] + anchors + [(len(sa), len(sb))]
        for (i0, j0), (i1, j1) in zip(bounds, bounds[1:]):
            ga = [x for x in sa[i0 + 1:i1] if x not in ab]
            gb = [y for y in sb[j0 + 1:j1] if y not in ba]
            if not ga or not gb:
                continue
            local = collections.Counter()
            if len(ga) == 1 and len(gb) == 1:
                local[(ga[0], gb[0])] += 1
            elif len(ga) == len(gb) and len(ga) <= 4 and (i1 - i0) == (j1 - j0):
                for x, y in zip(ga, gb):
                    local[(x, y)] += 0.5
            elif sized and len(ga) <= 8 and len(gb) <= 8:
                # unequal gap (inlining on one side): walk in from both ends while sizes agree
                for seq_a, seq_b in ((ga, gb), (ga[::-1], gb[::-1])):
                    for x, y in zip(seq_a, seq_b):
                        if self.size_sim(x, y) < 0.6:
                            break
                        local[(x, y)] = 0.5
            if sized and 1 < len(ga) == len(gb) <= 8:
                # order-free fallback for members the positional pass left unpaired in an
                # equal-length gap: switch cases get reordered (MSVC) -> pair distinctive sizes
                paired_a = {x for x, _ in local}
                paired_b = {y for _, y in local}
                for x in ga:
                    if x in paired_a:
                        continue
                    sims = sorted(((self.size_sim(x, y), y) for y in gb if y not in paired_b), reverse=True)
                    if not sims:
                        continue
                    s, y = sims[0]
                    if s < 0.75 or (len(sims) > 1 and sims[1][0] > s - 0.15):
                        continue
                    back = max((self.size_sim(x2, y) for x2 in ga if x2 != x), default=0)
                    if back <= s - 0.15:
                        local[(x, y)] = 0.5
            for k, w in local.items():
                votes[k][src] += w

    def _data_votes(self, votes) -> None:
        A, B = self.A, self.B
        gv: dict = collections.defaultdict(collections.Counter)
        for a, b in self.ab.items():
            da, db = A.drefs.get(a, []), B.drefs.get(b, [])
            if da and db:
                self._align_votes(da, db, self.gab, self.gba, gv, "g")
        cand_a, cand_b = collections.defaultdict(list), collections.defaultdict(list)
        for (x, y), c in gv.items():
            cand_a[x].append((c["g"], y))
            cand_b[y].append((c["g"], x))
        for (x, y), c in gv.items():
            w = c["g"]
            if w < 1 or x in self.gab or y in self.gba:
                continue
            ca, cb = sorted(cand_a[x], reverse=True), sorted(cand_b[y], reverse=True)
            if ca[0] != (w, y) or cb[0] != (w, x):
                continue
            if (len(ca) > 1 and ca[1][0] >= w) or (len(cb) > 1 and cb[1][0] >= w):
                continue
            self.gab[x], self.gba[y] = y, x
        for x, y in self.gab.items():
            ua = [f for f in A.dref_users.get(x, ()) if f not in self.ab]
            ub = [f for f in B.dref_users.get(y, ()) if f not in self.ba]
            if len(ua) == 1 and len(ub) == 1:
                votes[(ua[0], ub[0])]["data"] += 1

    def _const_votes(self, votes) -> None:
        A, B = self.A, self.B
        ca, cb = collections.defaultdict(set), collections.defaultdict(set)
        for a, cs in A.consts.items():
            if a not in self.ab:
                for c in cs:
                    ca[c].add(a)
        for b, cs in B.consts.items():
            if b not in self.ba:
                for c in cs:
                    cb[c].add(b)
        for c in set(ca) & set(cb):
            if len(ca[c]) == 1 and len(cb[c]) == 1:
                k = (next(iter(ca[c])), next(iter(cb[c])))
                if votes[k]["const"] < 1.5:
                    votes[k]["const"] += 0.5

    def by_order(self) -> int:
        """Match functions in gaps between two anchors that are adjacent on both sides.

        Link order is not globally shared between platforms, but within an object
        file every compiler keeps source order: if PS2 anchors a0<a1 are adjacent
        among matched PS2 functions and their counterparts b0<b1 are adjacent among
        matched B functions, equal-length gaps line up 1:1.
        """
        A, B = self.A, self.B
        ma = sorted(self.ab)
        rankb = {b: i for i, b in enumerate(sorted(self.ba))}
        n = 0
        for a0, a1 in zip(ma, ma[1:]):
            b0, b1 = self.ab[a0], self.ab[a1]
            if not (b0 < b1 and rankb[b1] == rankb[b0] + 1):
                continue
            ia0, ia1 = bisect.bisect_right(A.addrs, a0), bisect.bisect_left(A.addrs, a1)
            ib0, ib1 = bisect.bisect_right(B.addrs, b0), bisect.bisect_left(B.addrs, b1)
            ga = [x for x in A.addrs[ia0:ia1] if A.ninstr(x) > 2]
            gb = [y for y in B.addrs[ib0:ib1] if B.ninstr(y) > 2]
            if not ga or len(ga) != len(gb) or len(ga) > 6:
                continue
            ok = all(self.size_ok(x, y) and 0.5 <= (B.ninstr(y) / A.ninstr(x)) / self.median_ratio <= 2.0
                     for x, y in zip(ga, gb))
            if not ok:
                self.rejected["order:size"] += 1
                continue
            for x, y in zip(ga, gb):
                n += self.add(x, y, "order", "medium", f"gap {len(ga)}")
        return n


def clean_floats(vals: list[int]) -> set[int]:
    """Drop a lone `lui` upper half when the full lui+ori/addiu constant was also seen."""
    s = set(vals)
    his = {v & 0xFFFF0000 for v in s if v & 0xFFFF} | {(v + 0x8000) & 0xFFFF0000 for v in s if v & 0xFFFF}
    return {v for v in s if v & 0xFFFF or v not in his}


def lis(pairs: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Longest chain increasing in both coordinates (pairs sorted by first)."""
    pairs = sorted(pairs)
    tails, tails_idx, prev = [], [], [-1] * len(pairs)
    for i, (_, y) in enumerate(pairs):
        k = bisect.bisect_left(tails, y)
        if k == len(tails):
            tails.append(y)
            tails_idx.append(i)
        else:
            tails[k] = y
            tails_idx[k] = i
        prev[i] = tails_idx[k - 1] if k else -1
    out, i = [], tails_idx[-1] if tails_idx else -1
    while i >= 0:
        out.append(pairs[i])
        i = prev[i]
    return out[::-1]


def load_bsim(target: str) -> tuple[list[tuple[int, int, float, float]], dict[tuple[int, int], float]]:
    """GC BSim rows for the same game: strong (sim>=0.7) seeds and weak (0.5..0.7) votes."""
    fn = {"action": "nightfire_matches.tsv", "driving": "driving_matches.tsv"}[target]
    exe = f"nf_{target}.elf"
    strong, weak = [], {}
    with open(PS2 / "gc" / fn) as f:
        for r in csv.DictReader(f, delimiter="\t"):
            if r["exe"] != exe or not r["ps2_addr"]:
                continue
            sim, sig = float(r["similarity"]), float(r["significance"])
            a, b = int(r["ps2_addr"], 16), int(r["dol_addr"], 16)
            if sim >= 0.7:
                strong.append((a, b, sim, sig))
            elif sim >= 0.5:
                weak[(a, b)] = 0.5
    return strong, weak


def run_pair(a_prog: str, b_prog: str, bsim_target: str | None = None, seeds=None) -> Matcher:
    m = Matcher(Side(a_prog), Side(b_prog))
    stats = collections.OrderedDict()
    stats["str"] = m.by_strings()
    weak = {}
    if bsim_target:
        strong, weak = load_bsim(bsim_target)
        stats["bsim"] = m.by_bsim(strong)
    m.calibrate()
    if seeds:
        n = 0
        for a, b, meth, conf, ev in seeds:
            if m.ab.get(a) == b:
                m.rejected["seed:agree"] += 1  # independent PS2<->Xbox string match confirms it
                continue
            if a in m.ab or b in m.ba:
                m.rejected["seed:conflict"] += 1
            elif not m.size_ok(a, b, 1.5):
                m.rejected["seed:size"] += 1
            else:
                n += m.add(a, b, meth, conf, ev)
        stats["seed"] = n
        m.calibrate()
    for rnd in range(6):
        m.calibrate()
        p = m.propagate(weak)
        o = m.by_order()
        d = m.audit()
        stats[f"vote{rnd}"], stats[f"order{rnd}"], stats[f"audit{rnd}"] = p, o, -d
        if p + o == 0:
            break
    print(f"{a_prog} <-> {b_prog}: {len(m.ab)} pairs  {dict(stats)}  ratio=[{m.ratio_lo:.2f},{m.ratio_hi:.2f}] "
          f"globals={len(m.gab)} rejected={dict(m.rejected)}", flush=True)
    return m


def write_pairs(m: Matcher, path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w") as f:
        f.write("a_addr\ta_name\tb_addr\tb_name\tmethod\tconfidence\tevidence\n")
        for a in sorted(m.ab):
            b = m.ab[a]
            meth, conf, ev = m.meta[a]
            f.write(f"{a:08x}\t{m.A.name(a)}\t{b:08x}\t{m.B.name(b)}\t{meth}\t{conf}\t{ev}\n")


def compose(m_pg: Matcher, m_gx: Matcher) -> list[tuple[int, int, str, str, str]]:
    """PS2->GC->Xbox seeds; confidence is the weaker of the two legs."""
    rank = {"high": 2, "medium": 1}
    out = []
    for a, g in m_pg.ab.items():
        x = m_gx.ab.get(g)
        if x is None:
            continue
        m1, c1, _ = m_pg.meta[a]
        m2, c2, _ = m_gx.meta[g]
        conf = c1 if rank[c1] <= rank[c2] else c2
        out.append((a, x, "via-gc", conf, f"gc={g:08x} {m1}/{m2}"))
    return out


def main() -> None:
    targets = sys.argv[1:] or ["action", "driving"]
    out = XREF / "matches"
    for target in targets:
        ps2, gc, xb = f"ps2_{target}", f"gc_{target}", f"xbox_{target}"
        m_pg = run_pair(ps2, gc, bsim_target=target)
        write_pairs(m_pg, out / f"{ps2}__{gc}.tsv")
        m_gx = run_pair(gc, xb)
        write_pairs(m_gx, out / f"{gc}__{xb}.tsv")
        m_px = run_pair(ps2, xb, seeds=compose(m_pg, m_gx))
        write_pairs(m_px, out / f"{ps2}__{xb}.tsv")


if __name__ == "__main__":
    main()
