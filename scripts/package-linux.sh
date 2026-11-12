#!/bin/sh
# Local archive. Google platform-tools stay external.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
bin=${1:-$root/target/debug/cordgeist}
name=cordgeist-linux-x86_64
dest=$root/build/$name
rm -rf "$dest"
mkdir -p "$dest/bin" "$dest/lib" "$dest/third-party-notices"
cp "$bin" "$dest/bin/cordgeist.bin"
chmod 755 "$dest/bin/cordgeist.bin"
dependencies=$(ldd "$dest/bin/cordgeist.bin")
case "$dependencies" in *"not found"*) echo 'Unresolved loader dependency; package refused.' >&2; exit 1 ;; esac
printf '%s\n' "$dependencies" | while read -r line; do
    case "$line" in
        *'=>'* )
            lib=${line##*=> }
            lib=${lib%% *}
            case "$lib" in
                /lib/*|/usr/lib/*|/lib64/*|not) ;;
                /*) cp -L "$lib" "$dest/lib/" ;;
            esac
            ;;
    esac
done
cp "$root/LICENSE" "$root/NOTICE" "$root/THIRD_PARTY_NOTICES.md" "$dest/"
cp -a "$root/vendor/notices/." "$dest/third-party-notices/"
python3 "$root/scripts/collect-notices.py" "$dest/third-party-notices/rust"
mkdir -p "$dest/third-party-notices/libaoahid" "$dest/docs" "$dest/assets"
cp "$root/vendor/libaoahid/LICENSE" "$root/vendor/libaoahid/NOTICE" "$root/vendor/libaoahid/THIRD_PARTY_NOTICES.md" "$dest/third-party-notices/libaoahid/"
cp "$root/README.md" "$dest/README.md"
cp "$root/docs/setup-linux.md" "$root/docs/cli.md" "$root/docs/mcp.md" "$root/docs/media.md" "$root/docs/dependencies.md" "$root/docs/measurements.md" "$root/docs/api.md" "$root/docs/contracts.json" "$dest/docs/"
cp "$root/assets/v1-test-area.html" "$dest/assets/"
cp "$root/tests/fixtures/synthetic.png" "$dest/assets/owned-test.png"
git -C "$root" rev-parse HEAD > "$dest/SOURCE_COMMIT"
sha256sum "$dest/bin/cordgeist.bin" | awk '{print $1}' > "$dest/BINARY_SHA256"
cat > "$dest/cordgeist" << 'EOF'
#!/bin/sh
here=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
if [ -n "$(ls -A "$here/lib" 2>/dev/null || true)" ]; then
    export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi
exec "$here/bin/cordgeist.bin" "$@"
EOF
chmod 755 "$dest/cordgeist"
(
    cd "$dest"
    find . -type f ! -name MANIFEST.sha256 -exec sha256sum {} \; | sort > MANIFEST.sha256
    sha256sum -c MANIFEST.sha256 >/dev/null
)
tar --owner=0 --group=0 --numeric-owner -C "$root/build" -czf "$root/build/$name.tar.gz" "$name"
(cd "$root/build" && sha256sum "$name.tar.gz" > "$name.tar.gz.sha256")
echo "$root/build/$name.tar.gz"
