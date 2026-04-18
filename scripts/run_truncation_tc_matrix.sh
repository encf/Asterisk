#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="${ROOT_DIR}/scripts/run_truncation_tc.sh"
OUT_DIR="${ROOT_DIR}/run_logs/truncation_tc_matrix"

BANDWIDTH="100mbit"
DELAYS_MS=(20 50)
PARTIES=(5 10 16)
BATCH_SIZE=1000
SINGLE_REPEAT=5
BATCH_REPEAT=1
BASE_PORT=""

usage() {
  cat <<'EOF'
Usage: scripts/run_truncation_tc_matrix.sh [options]

Run the standalone truncation benchmark across a matrix of loopback tc settings.
By default this matches the current paper grid:
  - one-way delay: 20ms, 50ms
  - participants: 5, 10, 16

Options:
  --bandwidth <rate>            tc rate, e.g. 100mbit (default: 100mbit)
  --delays <list>               comma-separated one-way delays in ms (default: 20,50)
  --parties <list>              comma-separated participant counts (default: 5,10,16)
  -b, --batch-size <int>        Batch truncation size (default: 1000)
  --single-repeat <int>         Repetitions for single latency (default: 5)
  --batch-repeat <int>          Repetitions for batched case (default: 1)
  --base-port <int>             Base port for the first condition (default: auto-pick per case)
  --out-dir <path>              Output directory (default: run_logs/truncation_tc_matrix)
  -h, --help                    Show help

Example:
  ./scripts/run_truncation_tc_matrix.sh
EOF
}

csv_to_array() {
  local raw="$1"
  local -n out_ref="$2"
  IFS=',' read -r -a out_ref <<<"$raw"
}

ensure_sudo_ready() {
  if ! sudo -n true 2>/dev/null; then
    echo "This script delegates to run_truncation_tc.sh, which needs sudo access to configure tc on loopback (lo)." >&2
    echo "Please run it from an interactive terminal where sudo can prompt for your password," >&2
    echo "or pre-authorize sudo before invoking the matrix run." >&2
    exit 1
  fi
}

validate_port_plan() {
  local start_port="$1"
  local current="$start_port"
  local last_port=0
  local delay_ms n total_parties case_stride
  for delay_ms in "${DELAYS_MS[@]}"; do
    for n in "${PARTIES[@]}"; do
      total_parties=$((n + 1))
      case_stride=$((2 * total_parties * (total_parties - 1)))
      last_port=$((current + case_stride - 1))
      current=$((current + case_stride))
    done
  done
  if (( start_port < 1024 || last_port > 65535 )); then
    echo "Invalid --base-port=${start_port}: this matrix run needs hint ports up to ${last_port}, which must stay within 1024..65535." >&2
    exit 1
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bandwidth) BANDWIDTH="$2"; shift 2 ;;
    --delays)
      csv_to_array "$2" DELAYS_MS
      shift 2
      ;;
    --parties)
      csv_to_array "$2" PARTIES
      shift 2
      ;;
    -b|--batch-size) BATCH_SIZE="$2"; shift 2 ;;
    --single-repeat) SINGLE_REPEAT="$2"; shift 2 ;;
    --batch-repeat) BATCH_REPEAT="$2"; shift 2 ;;
    --base-port) BASE_PORT="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ ! -x "${RUN_SCRIPT}" ]]; then
  echo "Expected executable wrapper script at ${RUN_SCRIPT}" >&2
  exit 1
fi

ensure_sudo_ready

if [[ -n "${BASE_PORT}" ]]; then
  validate_port_plan "${BASE_PORT}"
fi

mkdir -p "${OUT_DIR}"
current_port="${BASE_PORT}"

for delay_ms in "${DELAYS_MS[@]}"; do
  delay_ms="${delay_ms%ms}"
  for n in "${PARTIES[@]}"; do
    label="owd${delay_ms}ms_n${n}"
    run_dir="${OUT_DIR}/${label}"

    echo "=== Running truncation benchmark: delay=${delay_ms}ms, bandwidth=${BANDWIDTH}, n=${n} ==="
    run_cmd=(
      "${RUN_SCRIPT}"
      --delay "${delay_ms}"
      --bandwidth "${BANDWIDTH}"
      -n "${n}"
      -b "${BATCH_SIZE}"
      --single-repeat "${SINGLE_REPEAT}"
      --batch-repeat "${BATCH_REPEAT}"
      --label "${label}"
      --out-dir "${OUT_DIR}"
    )

    if [[ -n "${BASE_PORT}" ]]; then
      run_cmd+=(--base-port "${current_port}")
    fi

    "${run_cmd[@]}" | tee "${run_dir}.log"

    if [[ -n "${BASE_PORT}" ]]; then
      total_parties=$((n + 1))
      per_run_width=$((2 * total_parties * (total_parties - 1)))
      current_port=$((current_port + per_run_width))
    fi
  done
done

echo
echo "[DONE] Matrix truncation runs saved under: ${OUT_DIR}"
