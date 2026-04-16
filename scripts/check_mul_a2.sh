#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
FIELD_PRIME=18446744073709551557

N=3
X_CLEAR=""
Y_CLEAR=""
REPEAT=1
SECURITY_MODEL="semi-honest"
BASE_PORT=""
LABEL=""
OUT_DIR="${ROOT_DIR}/run_logs/mul_check"

usage() {
  cat <<'EOF'
Usage: scripts/check_mul_a2.sh --x <int> --y <int> [options]

Run a single Asterisk2.0 multiplication locally and reconstruct the result.

Options:
  --x, --x-clear <int>         Clear signed input x (required)
  --y, --y-clear <int>         Clear signed input y (required)
  -n, --num-parties <int>      Number of computing parties (default: 3)
  --security-model <text>      semi-honest or malicious (default: semi-honest)
  -r, --repeat <int>           Number of repetitions (default: 1)
  -p, --base-port <int>        Base port (default: auto-pick a free range)
  --label <text>               Optional label for the output directory
  -o, --out-dir <path>         Output directory (default: run_logs/mul_check)
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
    raise SystemExit("Could not find a free port range for multiplication check")
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
    --y|--y-clear) Y_CLEAR="$2"; shift 2 ;;
    -n|--num-parties) N="$2"; shift 2 ;;
    --security-model) SECURITY_MODEL="$2"; shift 2 ;;
    -r|--repeat) REPEAT="$2"; shift 2 ;;
    -p|--base-port) BASE_PORT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    -o|--out-dir) OUT_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -z "${X_CLEAR}" || -z "${Y_CLEAR}" ]]; then
  echo "--x and --y are required" >&2
  usage
  exit 1
fi

if [[ "${SECURITY_MODEL}" != "semi-honest" && "${SECURITY_MODEL}" != "malicious" ]]; then
  echo "--security-model must be semi-honest or malicious" >&2
  exit 1
fi

if [[ ! -x "${BUILD_DIR}/benchmarks/asterisk2_mul_check" ]]; then
  echo "Missing benchmark binary: ${BUILD_DIR}/benchmarks/asterisk2_mul_check" >&2
  echo "Please build it first, for example:" >&2
  echo "  cmake -S \"${ROOT_DIR}\" -B \"${BUILD_DIR}\" -DCMAKE_BUILD_TYPE=Release" >&2
  echo "  cmake --build \"${BUILD_DIR}\" -j\$(nproc) --target asterisk2_mul_check" >&2
  exit 1
fi

if [[ -n "${LABEL}" ]]; then
  RUN_OUT_DIR="${OUT_DIR}/${LABEL}"
else
  RUN_OUT_DIR="${OUT_DIR}/x_${X_CLEAR}_y_${Y_CLEAR}_${SECURITY_MODEL}_n${N}_$(date +%Y%m%d_%H%M%S)"
fi

mkdir -p "${RUN_OUT_DIR}/logs"

if [[ -z "${BASE_PORT}" ]]; then
  BASE_PORT="$(pick_free_base_port)"
fi

echo "[RUN] MUL check: x=${X_CLEAR}, y=${Y_CLEAR}, n=${N}, model=${SECURITY_MODEL}, repeat=${REPEAT}, port=${BASE_PORT}"

jobs=()
for pid in $(seq 0 "${N}"); do
  "${BUILD_DIR}/benchmarks/asterisk2_mul_check" --localhost -n "${N}" -p "${pid}" \
    --security-model "${SECURITY_MODEL}" -r "${REPEAT}" \
    --x-clear "${X_CLEAR}" --y-clear "${Y_CLEAR}" --port "${BASE_PORT}" \
    -o "${RUN_OUT_DIR}/p${pid}.json" >"${RUN_OUT_DIR}/logs/p${pid}.log" 2>&1 &
  jobs+=("$!")
done

wait_for_jobs "${jobs[@]}"

python3 - "${RUN_OUT_DIR}" "${N}" "${REPEAT}" "${X_CLEAR}" "${Y_CLEAR}" "${SECURITY_MODEL}" "${FIELD_PRIME}" <<'PY'
import json
import pathlib
import sys

run_dir = pathlib.Path(sys.argv[1])
n = int(sys.argv[2])
repeat = int(sys.argv[3])
x_clear = int(sys.argv[4])
y_clear = int(sys.argv[5])
security_model = sys.argv[6]
prime = int(sys.argv[7])

def to_field(v):
    return v % prime

def to_signed(v):
    return v if v <= prime // 2 else v - prime

def load_party(pid):
    text = (run_dir / f"p{pid}.json").read_text().strip().splitlines()[-1]
    return json.loads(text)

party_data = [load_party(pid) for pid in range(n + 1)]
expected = to_field(x_clear * y_clear)

print(f"[INFO] expected x*y = {x_clear} * {y_clear} = {x_clear * y_clear}")
print(f"[INFO] expected field element = {expected}")
all_ok = True
for r in range(repeat):
    out = 0
    delta_out = 0
    delta = 0
    for pid in range(n):
        bench = party_data[pid]["benchmarks"][r]
        out = (out + int(bench["output_share"])) % prime
        if security_model == "malicious":
            delta_out = (delta_out + int(bench["delta_output_share"])) % prime
            delta = (delta + int(bench["delta_share"])) % prime

    ok = (out == expected)
    line = (
        f"[CHECK] repeat={r} reconstructed={out} (signed {to_signed(out)}) "
        f"expected={expected} (signed {to_signed(expected)}) -> {'PASS' if ok else 'FAIL'}"
    )
    if security_model == "malicious":
        mac_ok = (delta_out == (delta * out) % prime)
        ok = ok and mac_ok
        line += f", mac={'PASS' if mac_ok else 'FAIL'}"
    print(line)
    all_ok = all_ok and ok

print(f"[DONE] result={'PASS' if all_ok else 'FAIL'}")
print(f"[DONE] raw outputs: {run_dir}")
sys.exit(0 if all_ok else 1)
PY
