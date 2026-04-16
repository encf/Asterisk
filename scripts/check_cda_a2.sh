#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${ROOT_DIR}/build/benchmarks/asterisk2_darkpool_cda"
OUT_ROOT="${ROOT_DIR}/run_logs/cda_manual"

NUM_PARTIES=3
BUY_SIZE=4
SELL_SIZE=4
LX=16
SLACK=8
REPEAT=1
PORT=59100
LABEL="manual_cda"
SECURITY_MODEL="semi-honest"
NEW_ORDER_NAME=1
NEW_ORDER_UNIT=1
NEW_ORDER_PRICE=1

usage() {
  cat <<EOF
Usage: $0 [options]

Optional:
  -n NUM               Number of computing parties (default: ${NUM_PARTIES})
  -b NUM               Buy list size M (default: ${BUY_SIZE})
  -s NUM               Sell list size N (default: ${SELL_SIZE})
  --security-model     semi-honest or malicious (default: ${SECURITY_MODEL})
  --new-order-name X   Manual input for the new order name wire (default: ${NEW_ORDER_NAME})
  --new-order-unit X   Manual input for the new order unit wire (default: ${NEW_ORDER_UNIT})
  --new-order-price X  Manual input for the new order price wire (default: ${NEW_ORDER_PRICE})
  --lx NUM             Comparison lx parameter (default: ${LX})
  --slack NUM          Comparison slack parameter (default: ${SLACK})
  -r NUM               Repeat count (default: ${REPEAT})
  --port NUM           Base port (default: ${PORT})
  --label STR          Output label (default: ${LABEL})
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -n)
      NUM_PARTIES="$2"
      shift 2
      ;;
    -b)
      BUY_SIZE="$2"
      shift 2
      ;;
    -s)
      SELL_SIZE="$2"
      shift 2
      ;;
    --security-model)
      SECURITY_MODEL="$2"
      shift 2
      ;;
    --new-order-name)
      NEW_ORDER_NAME="$2"
      shift 2
      ;;
    --new-order-unit)
      NEW_ORDER_UNIT="$2"
      shift 2
      ;;
    --new-order-price)
      NEW_ORDER_PRICE="$2"
      shift 2
      ;;
    --lx)
      LX="$2"
      shift 2
      ;;
    --slack)
      SLACK="$2"
      shift 2
      ;;
    -r)
      REPEAT="$2"
      shift 2
      ;;
    --port)
      PORT="$2"
      shift 2
      ;;
    --label)
      LABEL="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! -x "${BIN}" ]]; then
  cat >&2 <<EOF
Missing benchmark binary: ${BIN}
Build it first with:
  cd ${ROOT_DIR}
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j"\$(nproc)" --target asterisk2_darkpool_cda
EOF
  exit 1
fi

if [[ "${SECURITY_MODEL}" != "semi-honest" && "${SECURITY_MODEL}" != "malicious" ]]; then
  echo "--security-model must be semi-honest or malicious." >&2
  exit 1
fi

CASE_DIR="${OUT_ROOT}/${LABEL}"
RAW_DIR="${CASE_DIR}/raw"
mkdir -p "${RAW_DIR}"

echo "=== Manual CDA check ==="
echo "buy_size=${BUY_SIZE}"
echo "sell_size=${SELL_SIZE}"
echo "new_order_name=${NEW_ORDER_NAME}"
echo "new_order_unit=${NEW_ORDER_UNIT}"
echo "new_order_price=${NEW_ORDER_PRICE}"
echo "n=${NUM_PARTIES}"
echo "security_model=${SECURITY_MODEL}"
echo "lx=${LX}"
echo "slack=${SLACK}"
echo "port=${PORT}"
echo

for pid in $(seq 0 "${NUM_PARTIES}"); do
  "${BIN}" \
    --localhost \
    -n "${NUM_PARTIES}" \
    -p "${pid}" \
    --security-model "${SECURITY_MODEL}" \
    -b "${BUY_SIZE}" \
    -s "${SELL_SIZE}" \
    --new-order-name "${NEW_ORDER_NAME}" \
    --new-order-unit "${NEW_ORDER_UNIT}" \
    --new-order-price "${NEW_ORDER_PRICE}" \
    --lx "${LX}" \
    --slack "${SLACK}" \
    -r "${REPEAT}" \
    --port "${PORT}" \
    -o "${RAW_DIR}/p${pid}.json" \
    > "${RAW_DIR}/p${pid}.log" 2>&1 &
done
wait

python3 - "${RAW_DIR}" "${NUM_PARTIES}" "${SECURITY_MODEL}" <<'PY'
import json
import sys
from pathlib import Path

prime = 18446744073709551557
root = Path(sys.argv[1])
n = int(sys.argv[2])
security_model = sys.argv[3]
files = [root / f"p{pid}.json" for pid in range(n + 1)]
for f in files:
    if not f.exists():
        raise SystemExit(f"missing output file: {f}")

parties = [json.loads(f.read_text().strip().splitlines()[-1]) for f in files]
bench = [p["benchmarks"][0] for p in parties]
expected = parties[0]["expected_outputs"]
reconstructed = []
reconstructed_delta = []
for i in range(len(expected)):
    s = 0
    ds = 0
    for pid in range(n):
        s = (s + int(bench[pid]["output_shares"][i])) % prime
        if security_model == "malicious":
            ds = (ds + int(bench[pid]["output_delta_shares"][i])) % prime
    reconstructed.append(s)
    if security_model == "malicious":
        reconstructed_delta.append(ds)

print("expected      =", expected)
print("reconstructed =", reconstructed)
print("matches       =", reconstructed == expected)
if security_model == "malicious":
    delta = None
    for idx, val in enumerate(expected):
        if val != 0:
            delta = reconstructed_delta[idx] * pow(val, -1, prime) % prime
            break
    if delta is None:
        delta = 0
    mac_ok = reconstructed_delta == [((delta * x) % prime) for x in expected]
    print("delta_shares  =", reconstructed_delta)
    print("mac_matches   =", mac_ok)
print("raw_dir       =", root)
PY
