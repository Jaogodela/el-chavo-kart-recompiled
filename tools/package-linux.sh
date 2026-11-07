#!/usr/bin/env bash
# Builds the Linux x86_64 Release and packages dist/ElChavoKart-v<version>-x86_64.AppImage.
# Local only: nothing is published, tagged or uploaded. Run on Linux or WSL.
#
#   tools/package-linux.sh            incremental Release build, then package
#   tools/package-linux.sh --clean    clean Release build first
#   tools/package-linux.sh --out-dir DIR    write the AppImage, its .sha256 and the symbols to DIR instead of dist (test builds)
#   tools/package-linux.sh --verify-only FILE    check an existing AppImage (extracts it, scans it, checks the runtime)
#
# The program's debug information goes to <out dir>/ElChavoKart-v<version>-x86_64-symbols/, which is not packaged.
#
# Needs clang-20, clang++-20, lld-20, cmake 3.25+, ninja, rsync, git, wget or curl, unzip, binutils and the GCC 13+ C++
# runtime. Release builds are made on Ubuntu 20.04 (tools/setup-linux-build-env.sh prepares one): the AppImage needs the
# build machine's glibc.
# The ReXGlue runtime and Vulkan GPU plugin are built from source with the project's patches
# (tools/build-patched-rexruntime.sh) the first time and whenever the patches change, which also needs network access.
# The AppImage runtime (the part that mounts the AppImage before AppRun runs) is built from the pinned
# AppImage/type2-runtime source with the project's patch (tools/build-appimage-runtime.sh): that needs root or sudo,
# network access the first time, and is skipped while the pins and patches are unchanged.
# Needs generated/default from tools/codegen.ps1 (Windows); the freshness check (tools/check-guest-code.sh) reads only
# the manifest and config, and no game file is needed or packaged.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# On the Linux filesystem: much faster than a Windows drive under /mnt, and case-sensitive.
WORK="${ECK_WORK:-$HOME/eck-linux-build}"
PRESET=linux-amd64-release

CLEAN=0
VERIFY_ONLY=""
OUT_DIR="$ROOT/dist"
while [ $# -gt 0 ]; do
  case "$1" in
    --clean) CLEAN=1 ;;
    --out-dir) [ $# -ge 2 ] || { echo "--out-dir needs a folder" >&2; exit 2; }; OUT_DIR="$(realpath -m "$2")"; shift ;;
    --verify-only) [ $# -ge 2 ] || { echo "--verify-only needs a file" >&2; exit 2; }; VERIFY_ONLY="$(realpath "$2")"; shift ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

fail() { echo "ERROR: $*" >&2; exit 1; }
. "$ROOT/tools/linux-policy.sh"
. "$ROOT/tools/linux-work.sh"

VERSION="$(sed -nE 's/^[[:space:]]*project\([[:space:]]*el_chavo_kart[[:space:]]+VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$ROOT/CMakeLists.txt" | tr -d '\r')"
[ -n "$VERSION" ] || fail "project VERSION not found in CMakeLists.txt"
policy_init "$ROOT"
NAME="ElChavoKart-v$VERSION-x86_64"

if [ -n "$VERIFY_ONLY" ]; then
  [ -f "$VERIFY_ONLY" ] || fail "missing $VERIFY_ONLY"
  TMP="$(mktemp -d)"
  trap 'rm -rf "$TMP"' EXIT
  cp "$VERIFY_ONLY" "$TMP/verify.AppImage"
  chmod +x "$TMP/verify.AppImage"
  bash "$ROOT/tools/build-appimage-runtime.sh" --check "$TMP/verify.AppImage"
  bash "$ROOT/tools/build-appimage-runtime.sh" --check-image "$TMP/verify.AppImage"
  (cd "$TMP" && ./verify.AppImage --appimage-extract >/dev/null)
  scan_appdir "$TMP/squashfs-root" "extracted AppImage" "$ROOT" "$WORK" "$HOME"
  CHECKSUM="${VERIFY_ONLY%.AppImage}.sha256"
  if [ -f "$CHECKSUM" ]; then
    [ "$(cut -d' ' -f1 "$CHECKSUM")" = "$(sha256sum "$VERIFY_ONLY" | cut -d' ' -f1)" ] ||
      fail "$CHECKSUM does not match $VERIFY_ONLY"
    echo "  checksum matches $(basename "$CHECKSUM")"
  fi
  echo "Verified $VERIFY_ONLY"
  exit 0
fi

echo "El Chavo Kart PC port $VERSION - Linux AppImage (local only)"
SOURCE="$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
if CHANGES="$(git -C "$ROOT" status --porcelain 2>/dev/null)"; then
  [ -z "$CHANGES" ] || SOURCE="$SOURCE plus uncommitted changes"
else
  SOURCE="$SOURCE, working tree state unknown"
fi
echo "Source: $SOURCE"

echo "== Tools"
for tool in clang-20 clang++-20 ld.lld-20 cmake ninja rsync git sha256sum objcopy strip readelf objdump ldd unzip; do
  command -v "$tool" >/dev/null || fail "$tool not found"
done
command -v wget >/dev/null || command -v curl >/dev/null || fail "wget or curl not found"
clang++-20 --version | sed -n 1p
cmake --version | sed -n 1p
HOST_GLIBC="$(getconf GNU_LIBC_VERSION | cut -d' ' -f2)"
echo "glibc $HOST_GLIBC"
[ "$(printf '%s\n' "$MAX_GLIBC" "$HOST_GLIBC" | sort -V | tail -1)" = "$MAX_GLIBC" ] ||
  fail "this machine has glibc $HOST_GLIBC; release builds need Ubuntu 20.04 (glibc $MAX_GLIBC), see tools/setup-linux-build-env.sh"

echo "== Inputs"
[ -f "$ROOT/third_party/trg-launcher/CMakeLists.txt" ] || fail "third_party/trg-launcher is empty"
bash "$ROOT/tools/check-guest-code.sh" "$ROOT" || fail "generated/default is not current (run tools/codegen.ps1 on Windows first)"

echo "== Source -> $WORK"
sync_sources "$ROOT" "$WORK"

echo "== ReXGlue SDK and patched runtime"
ensure_sdk "$ROOT" "$WORK"
SDK="$WORK/tools/rexglue/linux-amd64"
PATCHED="$WORK/tools/build-patched-rexruntime.sh"
bash "$PATCHED" --check "$SDK"

# The shipped libraries are the built ones with their symbol tables stripped (the debug sections stay out of the AppImage).
holds_built_libraries() {  # dir
  local lib stripped
  stripped="$(mktemp)"
  for lib in librexruntime.so librexgpu-xenos.so; do
    strip --strip-unneeded -o "$stripped" "$BIN/$lib"
    cmp -s "$stripped" "$1/$lib" || { rm -f "$stripped"; return 1; }
  done
  rm -f "$stripped"
}

echo "== Build ($PRESET)"
BIN="$WORK/out/build/$PRESET"
[ "$CLEAN" = 1 ] && rm -rf "$BIN"
(cd "$WORK" && cmake --preset "$PRESET" >/dev/null && cmake --build --preset "$PRESET")
for f in $BIN_NAME librexruntime.so librexgpu-xenos.so $BIN_NAME.toml assets/launcher/header-art.png assets/launcher/sponsor-heart.png; do
  [ -f "$BIN/$f" ] || fail "build output is missing $f"
done
bash "$PATCHED" --check "$SDK" --staged "$BIN" || fail "the build folder does not hold the patched ReXGlue libraries"
grep -qxF 'vulkan_allow_present_mode_immediate = false' "$BIN/$BIN_NAME.toml" ||
  fail "$BIN_NAME.toml does not keep VSync on by default"

echo "== AppDir"
APPDIR="$WORK/AppDir"
SYMBOLS="$OUT_DIR/$NAME-symbols"
stage_appdir "$APPDIR" "$BIN" "$SYMBOLS" "$WORK" "$VERSION"
holds_built_libraries "$APPDIR/usr/bin" || fail "the AppDir does not hold the patched ReXGlue libraries"

echo "== Release check"
scan_appdir "$APPDIR" AppDir "$ROOT" "$WORK" "$HOME"

echo "== AppImage"
ensure_appimage_tools "$ROOT" "$WORK"
TOOL="$APPIMAGETOOL"
RUNTIME="$APPIMAGE_RUNTIME"
mkdir -p "$OUT_DIR"
OUT="$OUT_DIR/$NAME.AppImage"
rm -f "$OUT"
# A Windows drive under /mnt cannot hold the file while appimagetool writes it as a mapped image.
BUILT="$WORK/$NAME.AppImage"
rm -f "$BUILT"
ARCH=x86_64 "$TOOL" --appimage-extract-and-run --no-appstream --runtime-file "$RUNTIME" "$APPDIR" "$BUILT" >/dev/null
chmod 755 "$BUILT"
bash "$ROOT/tools/build-appimage-runtime.sh" --check "$BUILT"
bash "$ROOT/tools/build-appimage-runtime.sh" --check-image "$BUILT"

echo "== Final AppImage check"
VERIFY="$(mktemp -d)"
trap 'rm -rf "$VERIFY"' EXIT
cp "$BUILT" "$VERIFY/verify.AppImage"
(cd "$VERIFY" && ./verify.AppImage --appimage-extract >/dev/null)
scan_appdir "$VERIFY/squashfs-root" "extracted AppImage" "$ROOT" "$WORK" "$HOME"
holds_built_libraries "$VERIFY/squashfs-root/usr/bin" || fail "the AppImage does not hold the built runtime"
cp "$BUILT" "$OUT"
chmod 755 "$OUT" 2>/dev/null || true
cmp -s "$BUILT" "$OUT" || fail "$OUT differs from the AppImage that was checked"
(cd "$OUT_DIR" && sha256sum "$NAME.AppImage" > "$NAME.sha256")

echo
echo "===================================================="
echo " APPIMAGE READY (nothing was published)"
echo "===================================================="
echo "  AppImage: $OUT ($(du -h "$OUT" | cut -f1))"
echo "  SHA-256:  $(cut -d' ' -f1 "$OUT_DIR/$NAME.sha256")"
echo "  Symbols:  $SYMBOLS (not in the AppImage)"
echo "  Source:   $SOURCE"
echo "  Runtime:  $(sed -n 's/^inputs_sha256=//p' "$SDK/rexruntime-patch.txt" | cut -c1-16) (ReXGlue patch fingerprint)"
echo "  Contents:"
echo "$EXPECTED" | sed 's/^/    /'
