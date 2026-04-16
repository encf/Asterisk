#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_SCRIPT="${ROOT_DIR}/scripts/run_eqz_tc.sh"
OUT_DIR="${ROOT_DIR}/run_logs/eqz_paper_grid"

BANDWIDTH="100mbit"
DELAYS_MS=(20 50)
PARTIES=(5 10 16)
REPEAT=5
LX=16
SLACK=8
X_CLEAR=0
SKIP_TC=0

usage() {
  cat <<'EOF'
Usage: scripts/run_eqz_paper_grid.sh [options]

Run the paper's standalone EQZ experiment grid on localhost loopback:
  - one-way delay: 20ms, 50ms
  - participants: 5, 10, 16

Options:
  --bandwidth <rate>            tc rate, e.g. 100mbit (default: 100mbit)
  -r, --repeat <int>            Number of repetitions averaged in each case (default: 5)
  --lx <int>                    EQZ lx parameter (default: 16)
  --slack <int>                 EQZ slack parameter s (default: 8)
  --x, --x-clear <int>          Clear signed input x (default: 0)
  --out-dir <path>              Output directory (default: run_logs/eqz_paper_grid)
  --skip-tc                     Do not configure tc; run each case on the current local network
  -h, --help                    Show help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bandwidth) BANDWIDTH="$2"; shift 2 ;;
    -r|--repeat) REPEAT="$2"; shift 2 ;;
    --lx) LX="$2"; shift 2 ;;
    --slack) SLACK="$2"; shift 2 ;;
    --x|--x-clear) X_CLEAR="$2"; shift 2 ;;
    --out-dir) OUT_DIR="$2"; shift 2 ;;
    --skip-tc) SKIP_TC=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

if [[ ! -x "${RUN_SCRIPT}" ]]; then
  echo "Expected executable wrapper script at ${RUN_SCRIPT}" >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

echo "=== EQZ paper grid ==="
echo "bandwidth=${BANDWIDTH}"
echo "repeat=${REPEAT}"
echo "lx=${LX}"
echo "slack=${SLACK}"
echo "x_clear=${X_CLEAR}"
echo "skip_tc=${SKIP_TC}"
echo "delays=${DELAYS_MS[*]}"
echo "parties=${PARTIES[*]}"
echo

for delay_ms in "${DELAYS_MS[@]}"; do
  for n in "${PARTIES[@]}"; do
    label="eqz_owd${delay_ms}ms_n${n}"
    echo "=== Running case: ${label} ==="
    cmd=(
      "${RUN_SCRIPT}"
      --delay "${delay_ms}"
      --bandwidth "${BANDWIDTH}"
      -n "${n}"
      -r "${REPEAT}"
      --lx "${LX}"
      --slack "${SLACK}"
      --x-clear "${X_CLEAR}"
      --label "${label}"
      --out-dir "${OUT_DIR}"
    )
    if [[ "${SKIP_TC}" -eq 1 ]]; then
      cmd+=(--skip-tc)
    fi
    "${cmd[@]}"
    echo
  done
done

echo "[DONE] Paper-grid EQZ results saved under: ${OUT_DIR}"
