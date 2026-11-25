#!/bin/sh
# Usage: tests/libmpv_smoke.sh <dir with the macOS *.xcframework>
#
# Run after building mk-out-archive-xcframeworks-macos-universal-video-full and
# extracting the archive. Fails when:
# - any framework still has undefined symbols resolved at runtime
#   ("dynamically looked up"), e.g. cocoa_* when Swift is disabled;
# - libmpv_smoke.c fails or hangs (mpv_terminate_destroy never returning);
# - libmpv_osd_check.c finds OSD / subtitle text missing or stale with
#   gpu-next (Apple OpenGL PBO uploads, see the libplacebo patch).
set -eu

DIR="$1"
SLICE=macos-arm64_x86_64
OUT="$(mktemp -d)"

# hwdec_mac_gl.c uses these without mpv linking IOSurface.framework; every GUI
# process has it loaded, and media-kit's v0.6.2 build had the same two lookups.
ALLOWED='_IOSurfaceGetHeightOfPlane|_IOSurfaceGetWidthOfPlane'

status=0
for framework in "$DIR"/*.xcframework; do
  name="$(basename "$framework" .xcframework)"
  binary="$framework/$SLICE/$name.framework/Versions/A/$name"
  for arch in arm64 x86_64; do
    lookups="$(nm -arch "$arch" -m "$binary" | grep "dynamically looked up" | grep -E -v "$ALLOWED" || true)"
    if [ -n "$lookups" ]; then
      echo "FAIL $name ($arch) has dynamically looked up symbols:"
      echo "$lookups"
      status=1
    fi
  done
done
[ "$status" = 0 ] && echo "ok no dynamically looked up symbols"

flags=""
for framework in "$DIR"/*.xcframework; do
  flags="$flags -F$framework/$SLICE -Wl,-rpath,$framework/$SLICE"
done
for test in libmpv_smoke libmpv_osd_check; do
  # shellcheck disable=SC2086
  clang -Wno-deprecated-declarations $flags -framework Mpv -framework OpenGL \
    -o "$OUT/$test" "$(dirname "$0")/$test.c"
  perl -e 'alarm 60; exec @ARGV' "$OUT/$test" || status=1
done
exit "$status"
