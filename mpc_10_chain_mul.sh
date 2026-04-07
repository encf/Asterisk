#!/usr/bin/env bash
set -euo pipefail

# Run a minimal Asterisk MPC example for 10 sequential multiplications.
# "Sequential multiplications" are represented as:
#   depth = 10, gates per depth = 1
#
# Usage:
#   ./mpc_10_chain_mul.sh [num_parties]
#
# Example:
#   ./mpc_10_chain_mul.sh 3

NUM_PARTIES="${1:-3}"
DEPTH=10
GATES_PER_DEPTH=1
OUT_DIR="${OUT_DIR:-./run_logs/chain_mul_10}"

mkdir -p "${OUT_DIR}"

if [[ -x "./benchmarks/asterisk_mpc" ]]; then
  BENCH_BIN="./benchmarks/asterisk_mpc"
elif [[ -x "./build/benchmarks/asterisk_mpc" ]]; then
  BENCH_BIN="./build/benchmarks/asterisk_mpc"
elif compgen -G "./build*/benchmarks/asterisk_mpc" > /dev/null; then
  BENCH_BIN="$(compgen -G "./build*/benchmarks/asterisk_mpc" | head -n 1)"
else
  echo "Missing ./benchmarks/asterisk_mpc binary." >&2
  echo "Please build first:" >&2
  echo "  mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j\$(nproc) benchmarks" >&2
  exit 1
fi

echo "Starting ${NUM_PARTIES}+1 processes (party id 0..${NUM_PARTIES})"
echo "Circuit: depth=${DEPTH}, gates_per_depth=${GATES_PER_DEPTH}"
echo "Logs: ${OUT_DIR}"

for party in $(seq 1 "${NUM_PARTIES}"); do
  "${BENCH_BIN}" \
    -p "${party}" \
    --localhost \
    -g "${GATES_PER_DEPTH}" \
    -d "${DEPTH}" \
    -n "${NUM_PARTIES}" \
    > "${OUT_DIR}/party_${party}.log" 2>&1 &
  pids[${party}]=$!
done

"${BENCH_BIN}" \
  -p 0 \
  --localhost \
  -g "${GATES_PER_DEPTH}" \
  -d "${DEPTH}" \
  -n "${NUM_PARTIES}" \
  -o "${OUT_DIR}/party_0.json" \
  | tee "${OUT_DIR}/party_0.log" &
pids[0]=$!

for party in $(seq 0 "${NUM_PARTIES}"); do
  wait "${pids[${party}]}"
done

echo "Done. Key output: ${OUT_DIR}/party_0.log and ${OUT_DIR}/party_0.json"
