#!/bin/bash
# Record every scenario in scenarios/ from PCSX2 (needs PCSX2 with savestate slot 2 and vpad.py running),
# turn each trace into an nfgame input file and compare the replay against it.
#   run_scenarios.sh <gamedir> <nfgame> <workdir> [scenario ...]
set -e
GAME=${1:?gamedir}; NFGAME=${2:?path to nfgame}; WORK=${3:?workdir}; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
declare -A FRAMES=([stand]=60 [walk]=130 [strafe]=100 [turn]=150 [jump]=170 [crouch]=180 [wall]=270 [slide]=270)
mkdir -p "$WORK/oracle" "$WORK/inputs" "$WORK/replay"
for s in ${@:-stand walk strafe turn jump crouch wall slide}; do
  python3 "$HERE/trace.py" "$WORK/oracle/$s.jsonl" --frames "${FRAMES[$s]}" --load-slot 2 --script "$HERE/scenarios/$s.txt"
  python3 "$HERE/compare.py" make-inputs "$WORK/oracle/$s.jsonl" "$WORK/inputs/$s.inputs"
  "$NFGAME" "$GAME" 07000024.bin --inputs "$WORK/inputs/$s.inputs" --trace "$WORK/replay/$s.jsonl" > /dev/null
  "$NFGAME" "$GAME" 07000024.bin --sync --inputs "$WORK/inputs/$s.inputs" --trace "$WORK/replay/$s.sync.jsonl" > /dev/null
  python3 "$HERE/compare.py" diff "$WORK/oracle/$s.jsonl" "$WORK/replay/$s.jsonl" --name "$s (free)"
  python3 "$HERE/compare.py" diff "$WORK/oracle/$s.jsonl" "$WORK/replay/$s.sync.jsonl" --name "$s (synced)"
done
