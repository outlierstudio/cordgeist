#!/usr/bin/env bash
# SOFTWARE FAKE supervisor investigation: retain parallel test interactions.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
export CARGO_NET_OFFLINE=true
failed=0
for attempt in {1..20}; do
  echo "ADB software-fake investigation attempt $attempt/20"
  if ! cargo test --locked --test adb -- --nocapture; then
    failed=1
  fi
done
exit "$failed"
