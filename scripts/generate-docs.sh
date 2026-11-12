#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ${1:-} == --check ]]; then
  tmp=$(mktemp)
  trap 'rm -f "$tmp"' EXIT
  cargo run --quiet --locked --no-default-features --bin contracts > "$tmp"
  diff -u docs/contracts.json "$tmp"
else
  cargo run --quiet --locked --no-default-features --bin contracts > docs/contracts.json
fi
