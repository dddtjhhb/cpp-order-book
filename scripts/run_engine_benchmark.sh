#!/usr/bin/env bash
# Builds the engine benchmark in Release mode, runs it, and writes a results
# file that records the machine, OS, compiler and commit next to the numbers.
#
#   scripts/run_engine_benchmark.sh            # default: 1M events, 7 runs
#   scripts/run_engine_benchmark.sh --events 2000000 --runs 11
#
# Extra arguments are passed to order_book_engine_benchmark. For stable
# numbers: plug in power, disable Low Power Mode, close other apps, and do not
# use the machine while it runs (about 30 seconds).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=build-bench
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" --target order_book_engine_benchmark --parallel >/dev/null

case "$(uname -s)" in
  Darwin)
    CPU="$(sysctl -n machdep.cpu.brand_string)"
    CORES="$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo '?') performance + $(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo '?') efficiency"
    MEM="$(( $(sysctl -n hw.memsize) / 1073741824 )) GiB"
    OS="macOS $(sw_vers -productVersion) ($(uname -m))"
    POWER="$(pmset -g batt 2>/dev/null | head -1 | sed "s/Now drawing from //; s/'//g" || echo unknown)"
    ;;
  Linux)
    CPU="$(lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -1)"
    CORES="$(nproc) logical CPUs"
    MEM="$(awk '/MemTotal/ {printf "%d GiB", $2/1048576}' /proc/meminfo)"
    OS="$(. /etc/os-release 2>/dev/null && echo "$PRETTY_NAME") ($(uname -m)), kernel $(uname -r)"
    POWER="n/a"
    ;;
  *)
    CPU="unknown"; CORES="?"; MEM="?"; OS="$(uname -a)"; POWER="n/a" ;;
esac
COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)$(git diff --quiet 2>/dev/null || echo '-dirty')"
DATE="$(date +%Y-%m-%d)"
HOST_TAG="$(uname -s | tr '[:upper:]' '[:lower:]')-$(uname -m)"
OUT="docs/benchmarks/${DATE}-${HOST_TAG}.md"
mkdir -p docs/benchmarks

{
  echo "# Engine benchmark — ${DATE}"
  echo
  echo "| Machine | Value |"
  echo "|---|---|"
  echo "| CPU | ${CPU} |"
  echo "| Cores | ${CORES} |"
  echo "| Memory | ${MEM} |"
  echo "| OS | ${OS} |"
  echo "| Power | ${POWER} |"
  echo "| Commit | \`${COMMIT}\` |"
  echo "| Command | \`order_book_engine_benchmark $*\` |"
  echo
  "./$BUILD_DIR/order_book_engine_benchmark" "$@" | sed 's/^## Engine benchmark$/## Results/'
} > "$OUT"

cat "$OUT"
echo
echo "saved: $OUT"
