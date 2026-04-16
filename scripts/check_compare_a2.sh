#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIELD_PRIME=18446744073709551557

N=3
X_CLEAR=""
LX=16
SLACK=8
REPEAT=1
SECURITY_MODEL="semi-honest"
MODE="gtez"
BASE_PORT=""
LABEL=""
OUT_DIR="${ROOT_DIR}/run_logs/compare_check"

usage() {
  cat <<'EOF'
Usage: scripts/check_compare_a2.sh --x <int> [options]

Run Asterisk2.0 comparison locally and reconstruct either GTEZ(x) or LTZ(x).

Options:
  --x, --x-clear <int>         Clear signed input x (required)
  --mode <text>                gtez or ltz (default: gtez)
  -n, --num-parties <int>      Number of computing parties (default: 3)
  --security-model <text>      semi-honest or malicious (default: semi-honest)
  --lx <int>                   Bit-length parameter lx (default: 16)
  --slack <int>                Statistical slack s (default: 8)
  -r, --repeat <int>           Number of repetitions (default: 1)
  -p, --base-port <int>        Base port (default: auto-pick a free range)
  --label <text>               Optional label for the output directory
  -o, --out-dir <path>         Output directory (default: run_logs/compare_check)
  -h, --help                   Show help
EOF
}

pick_free_base_port() {
  python3 <<'PY'
import socket
START = 20000
END = 65000
WIDTH = 128
STRIDE = 16

def range_is_free(base):
    socks = []
    try:
        for port in range(base, base + WIDTH):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.bind(("0.0.0.0", port))
            socks.append(s)
        return True
    except OSError:
        return False
    finally:
        for s in socks:
            s.close()

for base in range(START, END - WIDTH + 1, STRIDE):
    if range_is_free(base):
        print(base)
        break
else:
    raise SystemExit("Could not find a free port range for compare check")
PY
}

wait_for_jobs() {
  local -a jobs=("$@")
  local pid
  for pid in "${jobs[@]}"; do
    wait "${pid}"
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --x|--x-clear) X_CLEAR="$2"; shift 2 ;;
    --mode) MODE="$2"; shift 2 ;;
    -n|--num-parties) N="$2"; shift 2 ;;
    --security-model) SECURITY_MODEL="$2"; shift 2 ;;
    --lx) LX="$2"; shift 2 ;;
    --slack) SLACK="$2"; shift 2 ;;
    -r|--repeat) REPEAT="$2"; shift 2 ;;
    -p|--base-port) BASE_PORT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    -o|--out-dir) OUT_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "${X_CLEAR}" ]]; then
  echo "--x is required" >&2
  usage
  exit 1
fi

if [[ "${SECURITY_MODEL}" != "semi-honest" && "${SECURITY_MODEL}" != "malicious" ]]; then
  echo "--security-model must be semi-honest or malicious" >&2
  exit 1
fi

if [[ "${MODE}" != "gtez" && "${MODE}" != "ltz" ]]; then
  echo "--mode must be gtez or ltz" >&2
  exit 1
fi

if [[ ! -x "${BUILD_DIR}/benchmarks/asterisk2_bgtez" ]]; then
  echo "Missing benchmark binary: ${BUILD_DIR}/benchmarks/asterisk2_bgtez" >&2
  echo "Please build it first, for example:" >&2
  echo "  cmake -S \"${ROOT_DIR}\" -B \"${BUILD_DIR}\" -DCMAKE_BUILD_TYPE=Release" >&2
  echo "  cmake --build \"${BUILD_DIR}\" -j\$(nproc) --target asterisk2_bgtez" >&2
  exit 1
fi

if [[ -n "${LABEL}" ]]; then
  RUN_OUT_DIR="${OUT_DIR}/${LABEL}"
else
  RUN_OUT_DIR="${OUT_DIR}/x_${X_CLEAR}_${MODE}_${SECURITY_MODEL}_n${N}_$(date +%Y%m%d_%H%M%S)"
fi

mkdir -p "${RUN_OUT_DIR}/logs"

if [[ -z "${BASE_PORT}" ]]; then
  BASE_PORT="$(pick_free_base_port)"
fi

echo "[RUN] Compare check: x=${X_CLEAR}, mode=${MODE}, n=${N}, model=${SECURITY_MODEL}, lx=${LX}, slack=${SLACK}, repeat=${REPEAT}, port=${BASE_PORT}"

jobs=()
for pid in $(seq 0 "${N}"); do
  "${BUILD_DIR}/benchmarks/asterisk2_bgtez" --localhost -n "${N}" -p "${pid}" \
    --security-model "${SECURITY_MODEL}" -r "${REPEAT}" \
    --lx "${LX}" --slack "${SLACK}" --x-clear "${X_CLEAR}" --port "${BASE_PORT}" \
    -o "${RUN_OUT_DIR}/p${pid}.json" >"${RUN_OUT_DIR}/logs/p${pid}.log" 2>&1 &
  jobs+=("$!")
done

wait_for_jobs "${jobs[@]}"

python3 - "${RUN_OUT_DIR}" "${N}" "${REPEAT}" "${X_CLEAR}" "${MODE}" "${FIELD_PRIME}" <<'PY'
import json
import pathlib
import sys

run_dir = pathlib.Path(sys.argv[1])
n = int(sys.argv[2])
repeat = int(sys.argv[3])
x_clear = int(sys.argv[4])
mode = sys.argv[5]
prime = int(sys.argv[6])

def load_party(pid):
    text = (run_dir / f"p{pid}.json").read_text().strip().splitlines()[-1]
    return json.loads(text)

party_data = [load_party(pid) for pid in range(n + 1)]
gtez_expected = 1 if x_clear >= 0 else 0
expected = gtez_expected if mode == "gtez" else (1 - gtez_expected)

print(f"[INFO] expected {mode.upper()}({x_clear}) = {expected}")
all_ok = True
for r in range(repeat):
    gtez = 0
    for pid in range(n):
        bench = party_data[pid]["benchmarks"][r]
        gtez = (gtez + int(bench["output_share"])) % prime
    out = gtez if mode == "gtez" else (1 - gtez)
    ok = (out == expected)
    print(f"[CHECK] repeat={r} reconstructed={out} expected={expected} -> {'PASS' if ok else 'FAIL'}")
    all_ok = all_ok and ok

print(f"[DONE] result={'PASS' if all_ok else 'FAIL'}")
print(f"[DONE] raw outputs: {run_dir}")
sys.exit(0 if all_ok else 1)
PY
