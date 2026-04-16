#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPARE_SCRIPT="${ROOT_DIR}/scripts/compare_truncation_a2.sh"
OUT_DIR="${ROOT_DIR}/run_logs/truncation_tc"

BANDWIDTH="100mbit"
ONE_WAY_DELAY_MS=20
N=5
BATCH_SIZE=1000
SINGLE_REPEAT=5
BATCH_REPEAT=1
BASE_PORT=65000
PING_COUNT=5
CLEAR_TC_ON_EXIT=1
LABEL=""

usage() {
  cat <<'EOF'
Usage: scripts/run_truncation_tc.sh [options]

Configure loopback tc/netem, run the standalone truncation benchmark, and save
the network snapshot together with benchmark outputs.

This wrapper is intended for localhost multi-process experiments, so it shapes
the loopback interface `lo`.

Options:
  -n, --num-parties <int>       Number of computing parties (default: 5)
  -b, --batch-size <int>        Number of truncations in the batch run (default: 1000)
  --single-repeat <int>         Repetitions for single truncation latency (default: 5)
  --batch-repeat <int>          Repetitions for the batched run (default: 1)
  --delay <int>                 Symmetric one-way delay in ms on `lo` (default: 20)
  --bandwidth <rate>            tc rate, e.g. 100mbit (default: 100mbit)
  --base-port <int>             Base port for benchmark processes (default: 65000)
  --ping-count <int>            ping probes used after tc setup (default: 5)
  --label <text>                Optional label for saved outputs
  --out-dir <path>              Output directory (default: run_logs/truncation_tc)
  --keep-tc                     Keep the final tc rule instead of clearing it on exit
  -h, --help                    Show help

Example:
  ./scripts/run_truncation_tc.sh --delay 20 --bandwidth 100mbit -n 5 --label owd20ms_n5
EOF
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

validate_port_range() {
  local port="$1"
  local max_port_needed=$((port + 399))
  if (( port < 1024 || max_port_needed > 65535 )); then
    echo "Invalid --base-port=${port}: this wrapper needs ports up to ${max_port_needed}, which must stay within 1024..65535." >&2
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
  if [[ "${CLEAR_TC_ON_EXIT}" -eq 1 ]]; then
    clear_tc
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -n|--num-parties) N="$2"; shift 2 ;;
    -b|--batch-size) BATCH_SIZE="$2"; shift 2 ;;
    --single-repeat) SINGLE_REPEAT="$2"; shift 2 ;;
    --batch-repeat) BATCH_REPEAT="$2"; shift 2 ;;
    --delay) ONE_WAY_DELAY_MS="$2"; shift 2 ;;
    --bandwidth) BANDWIDTH="$2"; shift 2 ;;
    --base-port) BASE_PORT="$2"; shift 2 ;;
    --ping-count) PING_COUNT="$2"; shift 2 ;;
    --label) LABEL="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --keep-tc) CLEAR_TC_ON_EXIT=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

require_cmd sudo
require_cmd tc
require_cmd ping
require_cmd python3

if [[ ! -x "${COMPARE_SCRIPT}" ]]; then
  echo "Expected executable compare script at ${COMPARE_SCRIPT}" >&2
  exit 1
fi

validate_port_range "${BASE_PORT}"

if [[ -z "${LABEL}" ]]; then
  LABEL="bw_${BANDWIDTH}_owd_${ONE_WAY_DELAY_MS}ms_n${N}"
fi

RUN_DIR="${OUT_DIR}/${LABEL}"
RAW_DIR="${RUN_DIR}/raw"
mkdir -p "${RUN_DIR}"

trap cleanup EXIT

sudo -v

echo "=== Configuring loopback tc: bandwidth=${BANDWIDTH}, one-way delay=${ONE_WAY_DELAY_MS}ms ==="
set_tc "${ONE_WAY_DELAY_MS}"
show_tc | tee "${RUN_DIR}/tc_qdisc.txt"

PING_AVG_MS="$(measure_ping_avg_ms "${RUN_DIR}/ping.txt")"
{
  echo "interface=lo"
  echo "bandwidth=${BANDWIDTH}"
  echo "one_way_delay_ms=${ONE_WAY_DELAY_MS}"
  echo "approx_rtt_ms=$((ONE_WAY_DELAY_MS * 2))"
  echo "measured_ping_avg_ms=${PING_AVG_MS}"
  echo "num_parties=${N}"
  echo "batch_size=${BATCH_SIZE}"
  echo "single_repeat=${SINGLE_REPEAT}"
  echo "batch_repeat=${BATCH_REPEAT}"
} > "${RUN_DIR}/env.txt"

"${COMPARE_SCRIPT}" \
  -n "${N}" \
  -b "${BATCH_SIZE}" \
  --single-repeat "${SINGLE_REPEAT}" \
  --batch-repeat "${BATCH_REPEAT}" \
  -p "${BASE_PORT}" \
  --label "${LABEL}" \
  -o "${RAW_DIR}" | tee "${RUN_DIR}/compare_output.txt"

echo
echo "[DONE] Truncation tc run saved to: ${RUN_DIR}"
