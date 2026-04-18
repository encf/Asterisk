#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

N=5
BUY_SIZE=32
SELL_SIZE=32
FILL_VALUE=1
LX=16
SLACK=8
REPEAT=1
THREADS=6
SECURITY_PARAM=128
BASE_PORT=""
LABEL=""
OUT_DIR="${ROOT_DIR}/run_logs/vm_protocols"

usage() {
  cat <<'EOF'
Usage: scripts/compare_vm_protocols.sh [options]

Compare Dark Pool VM benchmark across:
  1) Asterisk (legacy baseline)
  2) Asterisk2.0 semi-honest
  3) Asterisk2.0 malicious

Options:
  -n, --num-parties <int>      Number of computing parties (default: 5)
  -b, --buy-size <int>         Buy list size M (default: 32)
  -s, --sell-size <int>        Sell list size N (default: 32)
  --fill-value <int>           Deterministic value for every unit input (default: 1)
  --lx <int>                   Comparison lx parameter for Asterisk2.0 (default: 16)
  --slack <int>                Comparison slack parameter s (default: 8)
  -r, --repeat <int>           Repetitions per case (default: 1)
  --threads <int>              Legacy Asterisk thread count (default: 6)
  --security-param <int>       Legacy Asterisk security parameter (default: 128)
  -p, --base-port <int>        Base port for the first run (default: auto-pick)
  --label <text>               Optional scenario label
  -o, --out-dir <path>         Output directory (default: run_logs/vm_protocols)
  -h, --help                   Show help
EOF
}

compute_port_stride() {
  local total_parties=$1
  python3 - "$total_parties" <<'PY'
import sys
n_total = int(sys.argv[1])
print(2 * n_total * n_total + 64)
PY
}

pick_free_base_port() {
  local width=$1
  python3 - "$width" <<'PY'
import socket
import sys

START = 30000
END = 65000
WIDTH = int(sys.argv[1])
STRIDE = 16

def range_is_free(base):
    for port in range(base, base + WIDTH):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("0.0.0.0", port))
        except OSError:
            s.close()
            return False
        s.close()
    return True

for base in range(START, END - WIDTH + 1, STRIDE):
    if range_is_free(base):
        print(base)
        break
else:
    raise SystemExit("Could not find a free port range for VM benchmark")
PY
}

ensure_base_port_available() {
  local base_port="$1"
  local width="$2"
  python3 - "$base_port" "$width" <<'PY'
import socket
import sys

base = int(sys.argv[1])
width = int(sys.argv[2])
if base < 1024 or base + width - 1 > 65535:
    raise SystemExit(f"Invalid base port {base}: need a free range up to {base + width - 1} within 1024..65535")

try:
    for port in range(base, base + width):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.bind(("0.0.0.0", port))
except OSError as exc:
    raise SystemExit(f"Base port {base} is not usable: {exc}")
PY
}

wait_for_jobs() {
  local -a jobs=("$@")
  local idx
  for idx in "${!jobs[@]}"; do
    local pid="${jobs[$idx]}"
    if ! wait "${pid}"; then
      local status=$?
      local j
      for j in "${!jobs[@]}"; do
        if (( j > idx )); then
          kill "${jobs[$j]}" 2>/dev/null || true
        fi
      done
      for j in "${!jobs[@]}"; do
        if (( j > idx )); then
          wait "${jobs[$j]}" 2>/dev/null || true
        fi
      done
      return "${status}"
    fi
  done
}

repeat_csv() {
  local value="$1"
  local count="$2"
  python3 - "$value" "$count" <<'PY'
import sys
value = sys.argv[1]
count = int(sys.argv[2])
print(",".join([value] * count))
PY
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -n|--num-parties) N="$2"; shift 2 ;;
    -b|--buy-size) BUY_SIZE="$2"; shift 2 ;;
    -s|--sell-size) SELL_SIZE="$2"; shift 2 ;;
    --fill-value) FILL_VALUE="$2"; shift 2 ;;
    --lx) LX="$2"; shift 2 ;;
    --slack) SLACK="$2"; shift 2 ;;
    -r|--repeat) REPEAT="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --security-param) SECURITY_PARAM="$2"; shift 2 ;;
    -p|--base-port) BASE_PORT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    -o|--out-dir) OUT_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ -n "${LABEL}" ]]; then
  RUN_OUT_DIR="${OUT_DIR}/${LABEL}"
else
  RUN_OUT_DIR="${OUT_DIR}/run_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "${RUN_OUT_DIR}"

for bin in Darkpool_VM asterisk2_darkpool_vm; do
  if [[ ! -x "${BUILD_DIR}/benchmarks/${bin}" ]]; then
    echo "Missing benchmark binary: ${BUILD_DIR}/benchmarks/${bin}" >&2
    echo "Please build benchmarks first, for example:" >&2
    echo "  cmake -S \"${ROOT_DIR}\" -B \"${BUILD_DIR}\" -DCMAKE_BUILD_TYPE=Release" >&2
    echo "  cmake --build \"${BUILD_DIR}\" -j\$(nproc) --target Darkpool_VM asterisk2_darkpool_vm" >&2
    exit 1
  fi
done

TOTAL_PARTIES=$((N + 1))
PORT_STRIDE="$(compute_port_stride "${TOTAL_PARTIES}")"
if [[ -n "${BASE_PORT}" ]]; then
  ensure_base_port_available "${BASE_PORT}" "$((3 * PORT_STRIDE))"
fi

SELL_UNITS="$(repeat_csv "${FILL_VALUE}" "${SELL_SIZE}")"
BUY_UNITS="$(repeat_csv "${FILL_VALUE}" "${BUY_SIZE}")"

run_multiparty() {
  local tag="$1"
  local port="${2:-}"
  shift 2
  local -a cmd=("$@")
  local run_dir="${RUN_OUT_DIR}/${tag}"
  local log_dir="${run_dir}/logs"
  if [[ -z "${port}" ]]; then
    port="$(pick_free_base_port "${PORT_STRIDE}")"
  fi
  echo "[RUN] tag=${tag}, n=${N}, buy=${BUY_SIZE}, sell=${SELL_SIZE}, repeat=${REPEAT}, port=${port}"
  rm -rf "${run_dir}"
  mkdir -p "${log_dir}"
  local -a jobs=()
  for pid in $(seq 0 "${N}"); do
    "${cmd[@]}" --localhost -n "${N}" -p "${pid}" --port "${port}" \
      -o "${run_dir}/p${pid}.json" >"${log_dir}/p${pid}.log" 2>&1 &
    jobs+=("$!")
  done
  wait_for_jobs "${jobs[@]}"
  echo "[DONE] tag=${tag}"
}

if [[ -n "${BASE_PORT}" ]]; then
  run_multiparty "asterisk_vm" "${BASE_PORT}" \
    "${BUILD_DIR}/benchmarks/Darkpool_VM" \
    -b "${BUY_SIZE}" -s "${SELL_SIZE}" -r "${REPEAT}" \
    --fill-value "${FILL_VALUE}" --threads "${THREADS}" --security-param "${SECURITY_PARAM}"
  run_multiparty "asterisk2_vm_sh" "$((BASE_PORT + PORT_STRIDE))" \
    "${BUILD_DIR}/benchmarks/asterisk2_darkpool_vm" \
    -b "${BUY_SIZE}" -s "${SELL_SIZE}" -r "${REPEAT}" \
    --security-model semi-honest --lx "${LX}" --slack "${SLACK}" \
    --sell-units "${SELL_UNITS}" --buy-units "${BUY_UNITS}"
  run_multiparty "asterisk2_vm_dh" "$((BASE_PORT + 2 * PORT_STRIDE))" \
    "${BUILD_DIR}/benchmarks/asterisk2_darkpool_vm" \
    -b "${BUY_SIZE}" -s "${SELL_SIZE}" -r "${REPEAT}" \
    --security-model malicious --lx "${LX}" --slack "${SLACK}" \
    --sell-units "${SELL_UNITS}" --buy-units "${BUY_UNITS}"
else
  run_multiparty "asterisk_vm" "" \
    "${BUILD_DIR}/benchmarks/Darkpool_VM" \
    -b "${BUY_SIZE}" -s "${SELL_SIZE}" -r "${REPEAT}" \
    --fill-value "${FILL_VALUE}" --threads "${THREADS}" --security-param "${SECURITY_PARAM}"
  run_multiparty "asterisk2_vm_sh" "" \
    "${BUILD_DIR}/benchmarks/asterisk2_darkpool_vm" \
    -b "${BUY_SIZE}" -s "${SELL_SIZE}" -r "${REPEAT}" \
    --security-model semi-honest --lx "${LX}" --slack "${SLACK}" \
    --sell-units "${SELL_UNITS}" --buy-units "${BUY_UNITS}"
  run_multiparty "asterisk2_vm_dh" "" \
    "${BUILD_DIR}/benchmarks/asterisk2_darkpool_vm" \
    -b "${BUY_SIZE}" -s "${SELL_SIZE}" -r "${REPEAT}" \
    --security-model malicious --lx "${LX}" --slack "${SLACK}" \
    --sell-units "${SELL_UNITS}" --buy-units "${BUY_UNITS}"
fi

python3 - "${RUN_OUT_DIR}" "${N}" "${LABEL}" "${BUY_SIZE}" "${SELL_SIZE}" "${FILL_VALUE}" "${LX}" "${SLACK}" "${REPEAT}" <<'PY'
import json
import pathlib
import statistics
import sys

out_dir = pathlib.Path(sys.argv[1])
n = int(sys.argv[2])
label = sys.argv[3]
buy_size = int(sys.argv[4])
sell_size = int(sys.argv[5])
fill_value = int(sys.argv[6])
lx = int(sys.argv[7])
slack = int(sys.argv[8])
repeat = int(sys.argv[9])
MB = 1024 * 1024

def read_rows(tag):
    parties = [json.loads((out_dir / tag / f"p{pid}.json").read_text()) for pid in range(n + 1)]
    reps = len(parties[0]["benchmarks"])
    return parties, reps

def summarize_split(tag):
    parties, reps = read_rows(tag)
    off_comm, off_time, on_comm, on_time = [], [], [], []
    for r in range(reps):
        off_b = on_b = 0
        off_ms = on_ms = 0.0
        for pid in range(n + 1):
            row = parties[pid]["benchmarks"][r]
            off_b += int(row["offline_bytes"])
            on_b += int(row["online_bytes"])
            off_ms = max(off_ms, float(row["offline"]["time"]))
            on_ms = max(on_ms, float(row["online"]["time"]))
        off_comm.append(off_b / MB)
        on_comm.append(on_b / MB)
        off_time.append(off_ms / 1000.0)
        on_time.append(on_ms / 1000.0)
    return {
        "offline_comm_mb": statistics.mean(off_comm),
        "offline_time_s": statistics.mean(off_time),
        "online_comm_mb": statistics.mean(on_comm),
        "online_time_s": statistics.mean(on_time),
    }

summary = {
    "label": label,
    "num_parties": n,
    "buy_list_size": buy_size,
    "sell_list_size": sell_size,
    "fill_value": fill_value,
    "repeat": repeat,
    "parameters": {"lx": lx, "slack": slack},
    "asterisk": summarize_split("asterisk_vm"),
    "asterisk2_sh": summarize_split("asterisk2_vm_sh"),
    "asterisk2_dh": summarize_split("asterisk2_vm_dh"),
}

(out_dir / "summary.json").write_text(json.dumps(summary, indent=2))

md = [
    f"=== VM Benchmark Summary === [{label}]",
    "| Protocol | Offline Comm (MB) | Offline Time (s) | Online Comm (MB) | Online Time (s) |",
    "|---|---:|---:|---:|---:|",
]

rows = [
    ("Asterisk", summary["asterisk"]),
    ("Asterisk2.0 semi-honest", summary["asterisk2_sh"]),
    ("Asterisk2.0 malicious", summary["asterisk2_dh"]),
]
for name, row in rows:
    md.append(
        f"| {name} | {row['offline_comm_mb']:.6f} | {row['offline_time_s']:.6f} | "
        f"{row['online_comm_mb']:.6f} | {row['online_time_s']:.6f} |"
    )

md.append("")
md.append(f"[INFO] Parameters: n={n}, N=M={sell_size}, fill_value={fill_value}, repeat={repeat}, lx={lx}, slack={slack}")
md.append(f"[INFO] Machine-readable summary: {out_dir / 'summary.json'}")

summary_md = "\n".join(md) + "\n"
(out_dir / "summary.md").write_text(summary_md)
print(summary_md, end="")
PY
