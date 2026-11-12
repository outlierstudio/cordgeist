#!/usr/bin/env bash
# Local, offline archive only. Never push, tag, publish, fetch, sudo or install.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
[[ $# -ge 3 ]] || { echo 'usage: package-release.sh BINARY NATIVE_LIB_DIR LIBUSB_SOURCE_TAR [BINARY_SOURCE_COMMIT]' >&2; exit 2; }
binary=$(realpath "$1")
native=$(realpath "$2")
usb_source=$(realpath "$3")
source_commit=$(git rev-parse HEAD)
binary_source=$(git rev-parse "${4:-HEAD}")
# Documentation may be dirty; material is read from the committed source tree.
git diff --quiet HEAD -- Cargo.toml Cargo.lock build.rs src vendor/libaoahid scripts/portable-build-inner.sh || { echo 'Uncommitted compiled-source changes; refusing source identity.' >&2; exit 2; }
git diff --quiet "$binary_source" "$source_commit" -- Cargo.toml Cargo.lock build.rs src vendor/libaoahid scripts/portable-build-inner.sh || { echo 'Binary source differs from packaged compiled source.' >&2; exit 2; }
version=$(git show HEAD:Cargo.toml | python3 -c 'import sys,re; print(re.search(r"(?m)^version\s*=\s*\"([^\"]+)\"",sys.stdin.read()).group(1))')
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Expected plain release semver.' >&2; exit 2; }
name="cordgeist-v$version-linux-x86_64"
out="$root/build/releases/${source_commit:0:12}"
mkdir -p "$out"
archive="$out/$name.tar.gz"
[[ ! -e $archive ]] || { echo 'Archive already exists; preserving it.' >&2; exit 2; }
tmp=$(mktemp -d "$out/stage.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
stage="$tmp/$name"
mkdir -p "$stage/bin" "$stage/lib" "$stage/sources" "$stage/verification" "$stage/third-party-notices" "$stage/packaging"
cp "$binary" "$stage/bin/cordgeist.bin"
chmod 755 "$stage/bin/cordgeist.bin"
cp -L "$native/libaoahid.so.2" "$native/libusb-1.0.so.0" "$stage/lib/"
symbols=$(nm -D "$stage/lib/libaoahid.so.2")
[[ $symbols != *cg_test_discovery_seed* ]] || { echo 'Software-fake native library refused.' >&2; exit 2; }
for file in "$stage/bin/cordgeist.bin" "$stage/lib/"*; do
    objdump -T "$file" | sed "s|$stage/||g" > "$stage/verification/$(basename "$file").objdump.txt"
done
python3 - "$stage/verification" <<'PY'
from pathlib import Path
import re,sys
for p in Path(sys.argv[1]).glob('*.objdump.txt'):
    text=p.read_text()
    versions=[tuple(map(int,v.split('.'))) for v in re.findall(r'GLIBC_([0-9.]+)',text)]
    if not versions or 'GLIBC_PRIVATE' in text or max(v+(0,)*(3-len(v)) for v in versions)>(2,31,0):
        raise SystemExit('GLIBC <=2.31 gate failed: '+p.name)
PY
for file in README.md LICENSE NOTICE THIRD_PARTY_NOTICES.md CHANGELOG.md; do
    if git cat-file -e "HEAD:$file" 2>/dev/null; then git show "HEAD:$file" > "$stage/$file"; fi
done
for file in 51-cordgeist.rules UDEV.md PORTABLE-BUILD.md; do git show "HEAD:packaging/$file" > "$stage/packaging/$file"; done
git show HEAD:scripts/portable-build-inner.sh > "$stage/packaging/portable-build-inner.sh"
# Ship the committed public docs, including nested acceptance evidence and links.
while IFS= read -r file; do
    mkdir -p "$stage/$(dirname "$file")"
    git show "HEAD:$file" > "$stage/$file"
done < <(git ls-tree -r --name-only HEAD docs)
cp -a vendor/notices/. "$stage/third-party-notices/"
mkdir -p "$stage/third-party-notices/libaoahid"
for file in LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
    git show "HEAD:vendor/libaoahid/$file" > "$stage/third-party-notices/libaoahid/$file"
done
export CARGO_NET_OFFLINE=true
python3 scripts/collect-notices.py "$stage/third-party-notices/rust"
cp "$usb_source" "$stage/sources/libusb-1.0.30.tar.gz"
git archive --format=tar HEAD | gzip -n > "$stage/sources/cordgeist-source.tar.gz"
printf '%s\n' "$source_commit" > "$stage/SOURCE_COMMIT"
printf '%s\n' "$binary_source" > "$stage/BINARY_SOURCE_COMMIT"
sha256sum "$stage/bin/cordgeist.bin" | awk '{print $1}' > "$stage/BINARY_SHA256"
cat > "$stage/cordgeist" <<'WRAPPER'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$here/bin/cordgeist.bin" "$@"
WRAPPER
chmod 755 "$stage/cordgeist"
(cd "$stage" && find . -type f ! -name MANIFEST.sha256 -exec sha256sum {} \; | LC_ALL=C sort > MANIFEST.sha256 && sha256sum -c MANIFEST.sha256 >/dev/null)
tar --owner=0 --group=0 --numeric-owner -C "$tmp" -czf "$archive" "$name"
(cd "$out" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
printf '%s\n' "$archive"
