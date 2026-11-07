#!/usr/bin/env bash
# Checks that generated/default, the recompiled game, was written by tools/codegen.ps1 for the current manifest and
# config. Codegen only runs on Windows, and the game files are not needed to check. Exit status: 0 current, 2 stale or
# unverifiable.
#
#   tools/check-guest-code.sh [ROOT]
set -euo pipefail

ROOT="$(cd "${1:-$(dirname "$0")/..}" && pwd)"
OUT="$ROOT/generated/default"
STAMP="$ROOT/generated/codegen-inputs.stamp"

stale() { echo "Guest code: stale ($*)" >&2; echo "Run tools\\codegen.ps1 on Windows, then build again." >&2; exit 2; }

SDK_VERSION="$(sed -n 's/^SDK_VERSION=//p' "$ROOT/patches/rexglue-sdk/baseline.env" | tr -d '\r')"
XEX_SHA256="$(sed -n 's/.*kXexSha256\[\] = "\([0-9a-f]\{64\}\)".*/\1/p' "$ROOT/src/game_files.h" | tr -d '\r')"
[ -n "$SDK_VERSION" ] || stale "patches/rexglue-sdk/baseline.env names no SDK version"
[ -n "$XEX_SHA256" ] || stale "src/game_files.h names no default.xex hash"
[ -f "$STAMP" ] || stale "no successful codegen record"
[ -f "$OUT/sources.cmake" ] || stale "generated/default/sources.cmake is missing"

# The stamp hashes the Windows checkout, whose config files may have CRLF endings.
input_is_current() {  # file expected-sha256
  [ -f "$1" ] || return 1
  [ "$(sha256sum "$1" | cut -d' ' -f1)" = "$2" ] && return 0
  [ "$(tr -d '\r' <"$1" | sha256sum | cut -d' ' -f1)" = "$2" ] && return 0
  [ "$(tr -d '\r' <"$1" | sed 's/$/\r/' | sha256sum | cut -d' ' -f1)" = "$2" ]
}

inputs=0
while IFS= read -r line || [ -n "$line" ]; do
  line="${line%$'\r'}"
  [ -n "$line" ] || continue
  hash="${line%%  *}"
  name="${line#*  }"
  case "$hash" in
    rexglue) [ "$name" = "$SDK_VERSION" ] || stale "generated/default was written by ReXGlue $name, not $SDK_VERSION" ;;
    "$XEX_SHA256") [ "$name" = game/extracted/default.xex ] || stale "unexpected input $name" ;;
    *)
      [ "$name" != game/extracted/default.xex ] || stale "generated/default was not written from the supported default.xex"
      input_is_current "$ROOT/$name" "$hash" || stale "$name changed"
      ;;
  esac
  inputs=$((inputs + 1))
done <"$STAMP"
[ "$inputs" -ge 3 ] || stale "the codegen record is incomplete"
grep -qF "$XEX_SHA256  game/extracted/default.xex" "$STAMP" || stale "the codegen record names no default.xex"

for fixed in el_chavo_kart_pch.h el_chavo_kart_init.h el_chavo_kart_init.cpp el_chavo_kart_register.cpp; do
  [ -f "$OUT/$fixed" ] || stale "generated/default/$fixed is missing"
done
while IFS= read -r source; do
  [ -f "$OUT/$source" ] || stale "generated/default/$source is missing"
done < <(grep -o '[A-Za-z0-9_.]*\.cpp' "$OUT/sources.cmake" | sort -u)
echo "Guest code: current"
