#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPARE_SCRIPT="${ROOT_DIR}/scripts/compare_fixedpoint_mul_a2.sh"
OUT_DIR="${ROOT_DIR}/run_logs/fixedpoint_mul_tc"

BANDWIDTH="100mbit"
DELAYS_MS=(20 50)
N=5
FIXED_MUL_COUNT=1000
FRAC_BITS=8
ELL_X=40
SLACK=8
BASE_PORT=""
PING_COUNT=5
CLEAR_TC_ON_EXIT=1
SKIP_TC=0
LABEL=""

usage() {
  cat <<'EOF'
Usage: scripts/run_fixedpoint_mul_tc.sh [options]

Configure loopback tc/netem and run the fixed-point multiplication benchmark
across one or more one-way delay settings.

By default this wrapper runs the scalar fixed-point latency cases:
  - one-way delay: 20ms, 50ms
  - bandwidth: 100mbit
  - participants: 5
  - scalar fixed-point multiplication calls: 1000

Options:
  -n, --num-parties <int>       Number of computing parties (default: 5)
  -c, --fixed-mul-count <int>   Number of scalar fixed-point calls inside one benchmark repetition (default: 1000)
  --frac-bits <int>             Fractional bits m for truncation (default: 8)
  --ell-x <int>                 Truncation ell_x (default: 40)
  --slack <int>                 Truncation slack s (default: 8)
  --delays <list>               Comma-separated one-way delays in ms (default: 20,50)
  --delay <int>                 Shorthand for a single delay value
  --bandwidth <rate>            tc rate, e.g. 100mbit (default: 100mbit)
  --base-port <int>             Base port hint for the first case (default: auto-pick a free range)
  --ping-count <int>            ping probes used after tc setup (default: 5)
  --label <text>                Optional label prefix for saved outputs
  --out-dir <path>              Output directory (default: run_logs/fixedpoint_mul_tc)
  --skip-tc                     Do not modify tc/ping; run on the current local network as-is
  --keep-tc                     Keep the final tc rule instead of clearing it on exit
  -h, --help                    Show help

Examples:
  ./scripts/run_fixedpoint_mul_tc.sh
  ./scripts/run_fixedpoint_mul_tc.sh --delay 50
  ./scripts/run_fixedpoint_mul_tc.sh --delays 20,50 --label fixedmul_scalar
EOF
}

csv_to_array() {
  local raw="$1"
  local -n out_ref="$2"
  IFS=',' read -r -a out_ref <<<"$raw"
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

ensure_sudo_ready() {
  if ! sudo -n true 2>/dev/null; then
    echo "This script needs sudo access to configure tc on loopback (lo)." >&2
    echo "Please run it from an interactive terminal where sudo can prompt for your password," >&2
    echo "or pre-authorize sudo before invoking the script." >&2
    exit 1
  fi
}

validate_port_plan() {
  local start_port="$1"
  local total_parties=$((N + 1))
  local model_stride=$((2 * total_parties * (total_parties - 1)))
  local per_case_width=$((2 * model_stride))
  local case_count=${#DELAYS_MS[@]}
  local max_port_needed=$((start_port + case_count * per_case_width - 1))
  if (( start_port < 1024 || max_port_needed > 65535 )); then
    echo "Invalid --base-port=${start_port}: this wrapper needs hint ports up to ${max_port_needed}, which must stay within 1024..65535." >&2
    exit 1
  fi
}

clear_tc() {
  sudo tc qdisc del dev lo root 2>/dev/null || true
}

set_tc() {
  local delay_ms="$1"
  clear_tc
  sudo tc qdisc add dev lo root netem delay "${delay_ms}ms" rate "${BANDWIDTH}"
}

show_tc() {
  tc qdisc show dev lo
}

measure_ping_avg_ms() {
  local log_file="$1"
  ping -n -c "${PING_COUNT}" 127.0.0.1 | tee "${log_file}" >/dev/null
  python3 - "${log_file}" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r"=\s*([0-9.]+)/([0-9.]+)/([0-9.]+)/", text)
print(match.group(2) if match else "NA")
PY
}

cleanup() {
  if [[ "${SKIP_TC}" -eq 0 && "${CLEAR_TC_ON_EXIT}" -eq 1 ]]; then
    clear_tc
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -n|--num-parties) N="$2"; shift 2 ;;
    -c|--fixed-mul-count) FIXED_MUL_COUNT="$2"; shift 2 ;;
    --frac-bits) FRAC_BITS="$2"; shift 2 ;;
    --ell-x) ELL_X="$2"; shift 2 ;;
    --slack) SLACK="$2"; shift 2 ;;
    --delays)
      csv_to_array "$2" DELAYS_MS
      shift 2
      ;;
    --delay)
      DELAYS_MS=("$2")
      shift 2
      ;;
    --bandwidth) BANDWIDTH="$2"; shift 2 ;;
    --base-port) BASE_PORT="$2"; shift 2 ;;
    --ping-count) PING_COUNT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --skip-tc) SKIP_TC=1; shift ;;
    --keep-tc) CLEAR_TC_ON_EXIT=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

require_cmd python3
if [[ "${SKIP_TC}" -eq 0 ]]; then
  require_cmd sudo
  require_cmd tc
  require_cmd ping
fi

if [[ ! -x "${COMPARE_SCRIPT}" ]]; then
  echo "Expected executable compare script at ${COMPARE_SCRIPT}" >&2
  exit 1
fi

if [[ "${SKIP_TC}" -eq 0 ]]; then
  ensure_sudo_ready
fi

if [[ -n "${BASE_PORT}" ]]; then
  validate_port_plan "${BASE_PORT}"
fi

mkdir -p "${OUT_DIR}"

trap cleanup EXIT

current_port="${BASE_PORT}"
per_case_width=$((4 * (N + 1) * N))

for delay_ms in "${DELAYS_MS[@]}"; do
  delay_ms="${delay_ms%ms}"
  if [[ -n "${LABEL}" ]]; then
    if (( ${#DELAYS_MS[@]} == 1 )); then
      run_label="${LABEL}"
    else
      run_label="${LABEL}_owd${delay_ms}ms_n${N}"
    fi
  else
    run_label="fixedmul_owd_${delay_ms}ms_n${N}"
  fi

  RUN_DIR="${OUT_DIR}/${run_label}"
  RAW_DIR="${RUN_DIR}/raw"
  mkdir -p "${RUN_DIR}"

  if [[ "${SKIP_TC}" -eq 0 ]]; then
    echo "=== Configuring loopback tc: bandwidth=${BANDWIDTH}, one-way delay=${delay_ms}ms ==="
    set_tc "${delay_ms}"
    show_tc | tee "${RUN_DIR}/tc_qdisc.txt"
    echo "[INFO] Measuring loopback ping..."
    PING_AVG_MS="$(measure_ping_avg_ms "${RUN_DIR}/ping.txt")"
    {
      echo "network_mode=tc_lo"
      echo "interface=lo"
      echo "bandwidth=${BANDWIDTH}"
      echo "one_way_delay_ms=${delay_ms}"
      echo "approx_rtt_ms=$((delay_ms * 2))"
      echo "measured_ping_avg_ms=${PING_AVG_MS}"
      echo "num_parties=${N}"
      echo "fixed_mul_count=${FIXED_MUL_COUNT}"
      echo "frac_bits=${FRAC_BITS}"
      echo "ell_x=${ELL_X}"
      echo "slack=${SLACK}"
    } > "${RUN_DIR}/env.txt"
  else
    echo "=== Skipping tc configuration; running on the current local network for delay label ${delay_ms}ms ==="
    {
      echo "network_mode=unchanged_local"
      echo "interface=lo"
      echo "bandwidth=unchanged"
      echo "one_way_delay_ms=${delay_ms}"
      echo "approx_rtt_ms=unchanged"
      echo "measured_ping_avg_ms=NA"
      echo "num_parties=${N}"
      echo "fixed_mul_count=${FIXED_MUL_COUNT}"
      echo "frac_bits=${FRAC_BITS}"
      echo "ell_x=${ELL_X}"
      echo "slack=${SLACK}"
    } > "${RUN_DIR}/env.txt"
  fi

  compare_cmd=(
    "${COMPARE_SCRIPT}"
    -n "${N}"
    -c "${FIXED_MUL_COUNT}"
    --frac-bits "${FRAC_BITS}"
    --ell-x "${ELL_X}"
    --slack "${SLACK}"
    -o "${RAW_DIR}"
  )

  if [[ -n "${current_port}" ]]; then
    compare_cmd+=(-p "${current_port}")
  fi

  "${compare_cmd[@]}" | tee "${RUN_DIR}/compare_output.txt"

  if [[ -n "${current_port}" ]]; then
    current_port=$((current_port + per_case_width))
  fi
done

echo
echo "[DONE] Fixed-point multiplication tc runs saved under: ${OUT_DIR}"
