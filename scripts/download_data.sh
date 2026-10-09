#!/usr/bin/env bash
# Downloads the 10-level LOBSTER sample files (2012-06-21) into data/.
# Source: https://huggingface.co/datasets/Ayrus-1007/lobster-data
# Usage: scripts/download_data.sh [TICKER ...]   (default: all five stocks)

set -euo pipefail

BASE="https://huggingface.co/datasets/Ayrus-1007/lobster-data/resolve/main"
DATA_DIR="$(cd "$(dirname "$0")/.." && pwd)/data"
TICKERS=("${@:-AAPL AMZN GOOG INTC MSFT}")

mkdir -p "$DATA_DIR"
for ticker in ${TICKERS[*]}; do
    for kind in message orderbook; do
        file="${ticker}_2012-06-21_34200000_57600000_${kind}_10.csv"
        if [[ -f "$DATA_DIR/$file" ]]; then
            echo "have        $file"
            continue
        fi
        echo "downloading $file"
        curl -fL --progress-bar -o "$DATA_DIR/$file.part" \
            "$BASE/LOBSTER_SampleFile_${ticker}_2012-06-21_10/$file"
        mv "$DATA_DIR/$file.part" "$DATA_DIR/$file"
    done
done
