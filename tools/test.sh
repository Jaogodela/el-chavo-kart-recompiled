#!/usr/bin/env bash
# The headless parity gate on Linux. tools/test.ps1 runs it inside WSL (Ubuntu 20.04, glibc 2.31) and folds its results
# into its own summary; it also works on its own on a Linux machine. Nothing opens a window, plays audio or starts the
# game: it compiles the real production sources, builds and runs the C++ suites, runs the Worker and a local TLS front,
# stages and scans a fixture AppDir, runs a fixture AppImage in extract-and-run mode, and inspects the built binaries.
# A check that cannot run fails; nothing is reported NOT RUN.
#
#   tools/test.sh                         --prepare, then --build-dir on the folder it prepared
#   tools/test.sh --prepare               sync this checkout into ~/eck-linux-build (ECK_WORK), install the patched
#                                         runtime, configure out/build/parity there, and print @@BUILD <folder>
#   tools/test.sh --build-dir DIR         build the test targets of a configured folder (the sources are the ones its
#                                         CMakeCache names) and run everything
#   tools/test.sh --emit-vectors FILE     also write the wire vectors this Linux build produces
#   tools/test.sh --peer-vectors FILE     also check them, and the Worker, against vectors another platform printed
#   tools/test.sh --check-environment     only check that this machine can run the gate (Ubuntu 20.04, glibc 2.31, tools)
#
# The real game files in game/ are read in place (never copied). The patched ReXGlue runtime is built on the first run
# (git and network needed once), as are Node.js and the AppImage runtime when the machine has none.
#
# Suites that stay Windows-only by design, because their code does not exist on Linux: eck_update_helper_tests
# (trg-updater.exe, the Windows ZIP installer; Linux replaces the AppImage from the running program, covered by
# eck_linux_appimage_tests) and the release folder checker of the ZIP (Linux has scan_appdir, covered here).
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${ECK_WORK:-$HOME/eck-linux-build}"
BUILD_DIR=""
EMIT_VECTORS=""
PEER_VECTORS=""
ORIGIN=""
WINDOWS_INTEGRATION=""
COMPLETE=0
CHANGED_FILES=""
PRIVATE_NAMES=()
CHECK_ENVIRONMENT=0
PREPARE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --check-environment) CHECK_ENVIRONMENT=1 ;;
    --prepare) PREPARE=1 ;;
    --build-dir) [ $# -ge 2 ] || { echo "--build-dir needs a folder" >&2; exit 2; }; BUILD_DIR="$(realpath "$2")"; shift ;;
    --emit-vectors) [ $# -ge 2 ] || { echo "--emit-vectors needs a file" >&2; exit 2; }; EMIT_VECTORS="$(realpath -m "$2")"; shift ;;
    --peer-vectors) [ $# -ge 2 ] || { echo "--peer-vectors needs a file" >&2; exit 2; }; PEER_VECTORS="$(realpath "$2")"; shift ;;
    --origin) [ $# -ge 2 ] || { echo "--origin needs a folder" >&2; exit 2; }; ORIGIN="$(realpath "$2")"; shift ;;
    --windows-integration) [ $# -ge 2 ] || { echo "--windows-integration needs a file" >&2; exit 2; }; WINDOWS_INTEGRATION="$2"; shift ;;
    --changed-files) [ $# -ge 2 ] || { echo "--changed-files needs a file" >&2; exit 2; }; CHANGED_FILES="$(realpath "$2")"; shift ;;
    --private-name) [ $# -ge 2 ] || { echo "--private-name needs a name" >&2; exit 2; }; PRIVATE_NAMES+=("$2"); shift ;;
    --complete) COMPLETE=1 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$ORIGIN" ] || ORIGIN="$ROOT"

[ "$(uname -s)" = Linux ] || { echo "ERROR: runs on Linux only (tools/test.ps1 is the Windows script)" >&2; exit 1; }

TLS_LOBBY_PID=""
TLS_PID=""
LOBBY_PID=""
TMP="$(mktemp -d)"
cleanup() {
  [ -n "$LOBBY_PID" ] && kill "$LOBBY_PID" 2>/dev/null
  [ -n "$TLS_LOBBY_PID" ] && kill "$TLS_LOBBY_PID" 2>/dev/null
  [ -n "$TLS_PID" ] && kill "$TLS_PID" 2>/dev/null
  rm -rf "$TMP"
}
trap cleanup EXIT

RESULTS=()  # "status|what|area;area;..."
fail() { echo "ERROR: $*" >&2; exit 1; }
. "$ROOT/tools/linux-policy.sh"
. "$ROOT/tools/linux-work.sh"

A_ENV="Linux environment"
A_BDIR="Linux test.sh --build-dir"
A_PROD="Linux production compile paths"
A_GUEST="Linux guest code"
A_INST="Linux game installation"
A_UI="Linux launcher"
A_LOC="Linux localization"
A_SET="Linux settings"
A_KEYS="Linux keyboard/hotkeys"
A_INPUT="Linux controller/mouse/focus"
A_DISP="Linux display"
A_GPU="Linux graphics card"
A_FPS="Linux frame limiter"
A_ACH="Linux achievements"
A_GAME="Linux hooks and patches"
A_CLIP="Linux clipboard runtime"
A_CRASH="Linux crash reporting"
A_DATA="Linux user data/AppImage paths"
A_REL="Linux AppImage relaunch"
A_UPD="Linux self-updater"
A_WS="Linux WebSocket/OpenSSL"
A_NET="Linux online protocol"
A_RELAY="Linux Direct/Relay"
A_RACE="Linux online race"
A_WORKER="Linux Worker"
A_WIRE="Linux wire vectors"
A_PKG="Linux packaging policy"
A_GLIBC="Linux GLIBC <= 2.31"
A_RPATH="Linux RPATH/RUNPATH"
A_DEPS="Linux dynamic dependencies"
A_CORE="Linux core runtime"
A_SAVE="Linux settings persistence"
A_REX="Linux ReXGlue"
A_WARN="Linux compiler warnings"
A_PRIV="Linux privacy"
A_SCRATCH="Linux scratch AppImage"
A_GATE="Linux test.sh gate"
AREAS=("$A_ENV" "$A_BDIR" "$A_PROD" "$A_GUEST" "$A_INST" "$A_UI" "$A_LOC" "$A_SET" "$A_KEYS" "$A_INPUT" "$A_DISP" "$A_GPU"
  "$A_FPS" "$A_ACH" "$A_GAME" "$A_CLIP" "$A_CRASH" "$A_DATA" "$A_REL" "$A_UPD" "$A_WS" "$A_NET" "$A_RELAY" "$A_RACE"
  "$A_WORKER" "$A_WIRE" "$A_PKG" "$A_GLIBC" "$A_RPATH" "$A_DEPS" "$A_CORE" "$A_SAVE" "$A_REX" "$A_WARN" "$A_PRIV" "$A_SCRATCH" "$A_GATE")

A_CROSS="Windows/Linux wire compatibility"
[ -z "$BUILD_DIR" ] || for area in "${AREAS[@]}"; do echo "@@AREA $area"; done

record() {  # status what areas
  RESULTS+=("$1|$2|$3")
  echo "@@RESULT $1|$2|$3"
  [ "$1" = FAIL ] && echo "PARITY FAIL: $2 (${3//;/, })"
  return 0
}

check() {  # what areas command...
  local what="$1" areas="$2" code
  shift 2
  echo "== $what"
  "$@"
  code=$?
  if [ "$code" = 77 ]; then record FAIL "$what (its fixture is missing)" "$areas"
  elif [ "$code" != 0 ]; then record FAIL "$what" "$areas"
  else record PASS "$what" "$areas"
  fi
}

SUITE_COUNT=0
CHECK_COUNT=0
FAILED_CHECKS=0
EXECUTED=" "

suite_counts() {  # log: "<checks> <failures>" summed over every summary line the suite printed
  local line checks=0 failures=0
  while IFS= read -r line; do
    if [[ "$line" =~ ([0-9]+)\ checks,\ ([0-9]+)\ failures ]]; then
      checks=$((checks + BASH_REMATCH[1])); failures=$((failures + BASH_REMATCH[2]))
    elif [[ "$line" =~ (passed|FAILED)\ \(([0-9]+)\ checks,\ ([0-9]+)\ failed\) ]]; then
      checks=$((checks + BASH_REMATCH[2])); failures=$((failures + BASH_REMATCH[3]))
    fi
  done <"$1"
  echo "$checks $failures"
}

run_suite() {  # what areas executable args...
  local what="$1" areas="$2" exe="$3" log code checks failures
  shift 3
  echo "== $what"
  log="$TMP/suite-$SUITE_COUNT.log"
  "$exe" "$@" >"$log" 2>&1
  code=$?
  cat "$log"
  EXECUTED="$EXECUTED$(basename "$exe") "
  SUITE_COUNT=$((SUITE_COUNT + 1))
  read -r checks failures < <(suite_counts "$log")
  CHECK_COUNT=$((CHECK_COUNT + checks))
  FAILED_CHECKS=$((FAILED_CHECKS + failures))
  if [ "$code" = 77 ]; then record FAIL "$what (its fixture is missing)" "$areas"
  elif [ "$code" != 0 ] || [ "$failures" != 0 ]; then record FAIL "$what" "$areas"
  else record PASS "$what" "$areas"
  fi
}

wait_for_port() {  # port
  for _ in $(seq 100); do
    (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

start_lobby() {  # port: the Worker's own code over node:http, in memory; sets LOBBY_PID
  "$NODE" "$SRC/server/eck-lobby/local/server.js" --host 127.0.0.1 --port "$1" --db :memory: >"$TMP/lobby-$1.log" 2>&1 &
  LOBBY_PID=$!
  wait_for_port "$1"
}

stop_lobby() {
  [ -z "$LOBBY_PID" ] || kill "$LOBBY_PID" 2>/dev/null
  wait "$LOBBY_PID" 2>/dev/null
  LOBBY_PID=""
}

# ---- Environment -------------------------------------------------------------------------------------------------

environment_problems() {
  local problems="" tool glibc version
  glibc="$(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2)"
  version="$(. /etc/os-release 2>/dev/null && echo "${NAME:-} ${VERSION_ID:-}")"
  [ "$glibc" = "$MAX_GLIBC" ] || problems="$problems glibc is '$glibc', the baseline machine has $MAX_GLIBC (Ubuntu 20.04);"
  [ "$version" = "Ubuntu 20.04" ] || problems="$problems this is '$version', not Ubuntu 20.04;"
  for tool in clang-20 clang++-20 cmake ninja rsync readelf ldd openssl patchelf python3 unzip sha256sum curl objcopy tar unshare; do
    command -v "$tool" >/dev/null || problems="$problems $tool is not installed (tools/setup-linux-build-env.sh prepares a machine);"
  done
  echo "$problems"
}

PROBLEMS="$(environment_problems)"
if [ "$CHECK_ENVIRONMENT" = 1 ]; then
  [ -z "$PROBLEMS" ] && exit 0
  echo "$PROBLEMS"
  exit 1
fi
echo "== Linux validation environment"
echo "@@ENV distro=$(. /etc/os-release && echo "$PRETTY_NAME")"
echo "@@ENV glibc=$(getconf GNU_LIBC_VERSION 2>/dev/null | cut -d' ' -f2)"
echo "@@ENV compiler=$(clang++-20 --version 2>/dev/null | sed -n 1p)"
echo "@@ENV cmake=$(cmake --version 2>/dev/null | sed -n 1p)"
echo "@@ENV ninja=$(ninja --version 2>/dev/null)"
if [ -n "$PROBLEMS" ]; then
  record FAIL "Linux validation environment is unavailable:$PROBLEMS" "$A_ENV;$A_GATE"
  echo "PARITY FAIL: Linux validation environment is unavailable:$PROBLEMS"
  exit 1
fi
[ -z "$BUILD_DIR" ] || record PASS "Linux validation environment (glibc $MAX_GLIBC, $(. /etc/os-release && echo "$PRETTY_NAME"))" "$A_ENV"

# ---- Work tree, runtime, build ----------------------------------------------------------------------------------

if [ -z "$BUILD_DIR" ]; then
  echo "== Source -> $WORK"
  sync_sources "$ROOT" "$WORK" || fail "cannot copy the sources to $WORK"
  echo "== ReXGlue SDK and patched runtime"
  ensure_sdk "$ROOT" "$WORK"
  echo "== Configure"
  (cd "$WORK" && cmake -S . -B "$WORK/out/build/parity" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 \
    -DCMAKE_CXX_COMPILER=clang++-20 >/dev/null) || fail "cmake could not configure $WORK"
  echo "@@WORK $WORK"
  echo "@@BUILD $WORK/out/build/parity"
  [ "$PREPARE" = 1 ] && exit 0
  rm -rf "$TMP"
  FORWARD=(--build-dir "$WORK/out/build/parity" --origin "$ROOT")
  [ -z "$EMIT_VECTORS" ] || FORWARD+=(--emit-vectors "$EMIT_VECTORS")
  [ -z "$PEER_VECTORS" ] || FORWARD+=(--peer-vectors "$PEER_VECTORS")
  [ -z "$WINDOWS_INTEGRATION" ] || FORWARD+=(--windows-integration "$WINDOWS_INTEGRATION")
  [ -z "$CHANGED_FILES" ] || FORWARD+=(--changed-files "$CHANGED_FILES")
  [ "$COMPLETE" != 1 ] || FORWARD+=(--complete)
  for name in "${PRIVATE_NAMES[@]}"; do FORWARD+=(--private-name "$name"); done
  exec bash "$WORK/tools/test.sh" "${FORWARD[@]}"
fi

BUILD="$BUILD_DIR"
[ -f "$BUILD/build.ninja" ] || fail "$BUILD is not a configured build folder"
SRC="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt")"
[ -f "$SRC/CMakeLists.txt" ] || fail "$BUILD names the source folder '$SRC', which has no CMakeLists.txt"
WORK="$SRC"
SDK="$SRC/tools/rexglue/linux-amd64"
TESTS="$SRC/out/build/parity-tests"
echo "@@ENV build_dir=$BUILD"
echo "@@ENV source_dir=$SRC"
record PASS "tools/test.sh --build-dir used the configured build folder $BUILD (sources $SRC)" "$A_BDIR"
check "patched ReXGlue runtime in the build folder's SDK" "$A_BDIR;$A_ENV" bash "$SRC/tools/build-patched-rexruntime.sh" --check "$SDK"
ensure_node "$WORK"
echo "@@ENV node=$($NODE --version 2>/dev/null)"

echo "== Production sources (every file the program is built from)"
PROD_OK=0
cmake --build "$BUILD" --target el_chavo_kart_linux_production_check >"$TMP/production.log" 2>&1
PROD_CODE=$?
tail -3 "$TMP/production.log"
[ "$PROD_CODE" = 0 ] && [ -x "$BUILD/el_chavo_kart_linux_production_check" ] && PROD_OK=1
[ "$PROD_OK" = 1 ] || grep -E 'error|FAILED' "$TMP/production.log" | head -20
PROD_COUNT="$(ninja -C "$BUILD" -t commands el_chavo_kart_linux_production_check | grep -c 'CMakeFiles/el_chavo_kart_linux_production_check[.]dir/.*[.]o ')"
if [ "$PROD_OK" = 1 ]; then record PASS "production sources compile and link on Linux ($PROD_COUNT source files, no game image)" "$A_PROD"
else record FAIL "production sources do not compile on Linux" "$A_PROD"; fi

echo "== Build the C++ suites"
cmake -S "$SRC/tests" -B "$TESTS" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 \
  -DCMAKE_CXX_COMPILER=clang++-20 -DECK_REXGLUE_SDK="$SDK" -DECK_REXGLUE_SRC="$SRC/rexglue-sdk-src" -DECK_TRG_PATCHED_DIR="$BUILD/trg-launcher-patched" \
  >"$TMP/suites-configure.log" 2>&1
SUITES_CODE=$?
[ "$SUITES_CODE" = 0 ] || tail -20 "$TMP/suites-configure.log"
if [ "$SUITES_CODE" = 0 ]; then
  cmake --build "$TESTS" -- -k 0 >"$TMP/suites.log" 2>&1
  SUITES_CODE=$?
  tail -3 "$TMP/suites.log"
fi
if [ "$SUITES_CODE" = 0 ]; then record PASS "build the C++ suites" "$A_CORE"
else
  grep -E 'error|FAILED' "$TMP/suites.log" 2>/dev/null | head -20
  record FAIL "build the C++ suites" "$A_CORE"
fi

# ---- Scripts and the Worker --------------------------------------------------------------------------------------

check "policy self-test (GLIBC, RUNPATH, dependencies)" "$A_GLIBC;$A_RPATH;$A_DEPS;$A_PKG" policy_self_test
check "codegen scanner: discovered functions are registered exactly once" "$A_GUEST" \
  bash -c "cd '$SRC' && python3 -I tools/find-missing-functions.py --verify"
check "generated guest code matches the manifest and config" "$A_GUEST" bash "$SRC/tools/check-guest-code.sh" "$SRC"
check "the build graph does not run ReXGlue codegen" "$A_GUEST" \
  bash -c "! grep -q -E 'rexglue codegen|bin/rexglue' '$BUILD/build.ninja'"
check "packaging scripts parse (package-linux.sh, test.sh, linux-policy.sh, linux-work.sh, AppRun)" "$A_PKG" \
  bash -c "bash -n '$SRC/tools/package-linux.sh' && bash -n '$SRC/tools/test.sh' && bash -n '$SRC/tools/linux-policy.sh' && bash -n '$SRC/tools/linux-work.sh' && sh -n '$SRC/packaging/linux/AppRun'"
check "AppImage release pins (runtime source, patch and SHA-256, appimagetool, --runtime-file, wrong inputs refused)" "$A_PKG" \
  bash "$SRC/tools/test-appimage-release.sh" "$SRC"
check_worker() {
  local log="$TMP/worker.log" code tests fail skipped cancelled
  grep -qF '"test": "node --test \"test/*.test.js\""' "$SRC/server/eck-lobby/package.json" ||
    { echo "PARITY FAIL: the Worker's npm test command is not the one this gate runs"; return 1; }
  (cd "$SRC/server/eck-lobby" && "$NODE" --test 'test/*.test.js') >"$log" 2>&1
  code=$?
  cat "$log"
  tests="$(sed -n 's/^ℹ tests \([0-9]*\)$/\1/p' "$log" | tail -1)"
  fail="$(sed -n 's/^ℹ fail \([0-9]*\)$/\1/p' "$log" | tail -1)"
  skipped="$(sed -n 's/^ℹ skipped \([0-9]*\)$/\1/p' "$log" | tail -1)"
  cancelled="$(sed -n 's/^ℹ cancelled \([0-9]*\)$/\1/p' "$log" | tail -1)"
  echo "@@STAT worker_tests=${tests:-0}"
  echo "@@STAT worker_failures=${fail:-?}"
  [ "$code" = 0 ] && [ "${tests:-0}" -gt 0 ] && [ "${fail:-1}" = 0 ] && [ "${skipped:-1}" = 0 ] && [ "${cancelled:-1}" = 0 ] ||
    { echo "PARITY FAIL: the Worker suite ran ${tests:-no} tests, ${fail:-?} failed, ${skipped:-?} skipped, ${cancelled:-?} cancelled, exit $code"; return 1; }
}
check "online service (Worker, Node $($NODE --version), canonical npm test command)" "$A_WORKER;$A_NET;$A_RELAY;$A_RACE" check_worker
check "Worker accepts the platforms clients report and counts them (dashboard schema)" "$A_WORKER" \
  bash -c "cd '$SRC/server/eck-lobby' && '$NODE' --test test/platforms.test.js"

# ---- C++ suites --------------------------------------------------------------------------------------------------

# what | executable and arguments | areas
SUITES=(
  "installer and ISO extraction (default.xex size, SHA-256, staging, recovery)|eck_installer_tests|$A_INST"
  "launcher text and pt-BR table|eck_ui_text_tests|$A_LOC;$A_UI"
  "achievements (46, overlay rules, persistence)|eck_achievements_tests|$A_ACH"
  "launcher logic (Shift, relaunch flow, pages)|eck_launcher_logic_tests|$A_UI;$A_SET"
  "crash report (SIGSEGV, SIGABRT and hard exit in child processes), Report a bug|eck_crash_report_tests|$A_CRASH;$A_UI"
  "cursor visibility|eck_cursor_input_tests|$A_INPUT"
  "hotkeys (F2, F3, F7, F11, console removed)|eck_hotkey_tests|$A_KEYS"
  "graphics card (Vulkan enumeration through a stand-in driver)|eck_graphics_adapter_tests|$A_GPU;$A_SET"
  "game window, window modes|eck_game_window_tests|$A_DISP"
  "frame limiter (30 to 240, Unlimited, wait)|eck_frame_limiter_tests|$A_FPS"
  "debug telemetry|eck_debug_telemetry_tests|$A_CORE"
  "input focus (multi-instance isolation)|eck_input_focus_tests|$A_INPUT;$A_RACE"
  "main menu|eck_main_menu_tests|$A_GAME"
  "character unlock|eck_character_unlock_tests|$A_GAME"
  "track unlock|eck_track_unlock_tests|$A_GAME"
  "online identity|eck_online_identity_tests|$A_NET;$A_DATA"
  "online protocol|eck_online_protocol_tests|$A_NET"
  "online peer table|eck_online_peer_tests|$A_NET;$A_RELAY"
  "online service (friends, invites, rooms)|eck_online_service_tests|$A_NET;$A_RELAY"
  "online session (UDP loopback, Direct, Relay, STUN)|eck_online_session_tests|$A_NET;$A_RELAY"
  "online lobby|eck_online_lobby_tests|$A_NET;$A_RACE"
  "online race sync|eck_online_race_sync_tests|$A_RACE"
  "online launcher page|eck_online_launcher_tests|$A_NET;$A_UI"
  "online race|eck_online_race_tests|$A_RACE"
  "online race telemetry|eck_online_race_telemetry_tests|$A_RACE;$A_NET"
  "online menu|eck_online_menu_tests|$A_NET;$A_RACE"
  "online probe|eck_online_probe_tests|$A_RACE"
  "online network (UDP loopback, address forms, STUN codec and Query)|eck_online_network_tests|$A_NET;$A_RELAY"
  "Connection test verdicts and router status|eck_online_connection_tests|$A_NET"
  "UPnP against a scripted router|eck_online_upnp_tests|$A_NET"
  "updater (release JSON, asset selection, SHA-256, AppImage target)|eck_update_tests|$A_UPD;$A_DATA"
  "self-relaunch command (AppImage, spaces, arguments)|eck_relaunch_tests|$A_REL;$A_DATA"
  "user data|eck_user_data_tests|$A_DATA"
  "log path|eck_log_path_tests|$A_DATA"
  "active log|eck_active_log_tests|$A_CORE;$A_CRASH"
  "Vulkan Internal render resolution fallback (100/75/50 kept on the host render target path, other paths fall back to 100% with a warning)|eck_vulkan_scale_tests|$A_GPU;$A_REX;$A_SET"
  "AppImage paths and the Linux self-updater (real folders)|eck_linux_appimage_tests|$A_DATA;$A_UPD;$A_REL"
  "clipboard (src/clipboard_sdl.cpp on SDL's offscreen video driver)|eck_clipboard_sdl_tests|$A_CLIP;$A_NET"
  "launcher file picker (portal replies, fallback order)|eck_file_dialog_tests|$A_UI;$A_INST"
  "window placement|eck_window_placement_tests|$A_DISP"
  "display scaling|eck_dpi_tests|$A_DISP"
  "wire bytes (tests/wire-vectors.txt)|eck_wire_parity_tests|$A_WIRE;$A_NET;$A_RELAY"
  "Open logs folder, Report a bug and links (xdg-open commands, no shell, real runner with a stand-in program)|eck_open_commands_tests|$A_UI;$A_CRASH;$A_DATA"
  "save files through the runtime virtual file system (create, write, reopen, overwrite, letter case, delete)|eck_save_files_tests|$A_DATA;$A_SAVE"
  "achievements (real AchievementManager: 46, 1000 gamerscore, unlock, duplicate, progress, reload, AppImage location)|eck_achievement_store_tests|$A_ACH;$A_DATA"
  "settings.toml (production GameSettings: save, reload, invalid TOML, Unicode paths, Connection test, AppImage location)|eck_settings_tests|$A_SAVE;$A_SET;$A_DATA"
)

manifest_problems() {
  local expected configured name problems=0
  expected="$(sed -n -E 's/^(both|linux)[[:space:]]+([a-z0-9_]+)[[:space:]]*$/\2/p' "$SRC/tests/required-suites.txt" | sort -u)"
  configured="$(for suite in "${SUITES[@]}"; do IFS='|' read -r _ command _ <<<"$suite"; echo "${command%% *}"; done | sort -u)"
  [ -n "$expected" ] || { echo "PARITY FAIL: tests/required-suites.txt names no Linux suite"; return 1; }
  for name in $(comm -23 <(echo "$expected") <(echo "$configured")); do
    echo "PARITY FAIL: the required suite $name is not in tools/test.sh"; problems=1
  done
  for name in $(comm -13 <(echo "$expected") <(echo "$configured")); do
    echo "PARITY FAIL: tools/test.sh runs $name, which tests/required-suites.txt does not list"; problems=1
  done
  echo "$(echo "$expected" | wc -l) required Linux suites, $(echo "$configured" | wc -l) configured"
  return $problems
}
check "the Linux suite list equals tests/required-suites.txt" "$A_CORE" manifest_problems

for suite in "${SUITES[@]}"; do
  IFS='|' read -r what command areas <<<"$suite"
  read -r -a argv <<<"$command"
  if [ ! -x "$TESTS/${argv[0]}" ]; then
    record FAIL "$what (${argv[0]} was not built)" "$areas"
  else
    run_suite "$what" "$areas" "$TESTS/${argv[0]}" "${argv[@]:1}"
  fi
done

# ---- WebSocket over TLS, behind a local front -------------------------------------------------------------------------

INTEGRATION_AREAS="$A_NET;$A_RELAY;$A_RACE;$A_WORKER"
PORT=$((18000 + RANDOM % 2000))
if start_lobby "$PORT"; then
  EXECUTED="$EXECUTED eck_online_integration_tests "
  check "online integration: production client, real Worker code, real loopback UDP (sign-in, friends, invites, 4-player room, Direct, race, telemetry, leave, Connection test)" \
    "$INTEGRATION_AREAS" env ECK_LOBBY_TEST_URL="ws://127.0.0.1:$PORT/" "$TESTS/eck_online_integration_tests"
else
  record FAIL "online integration (the local Worker did not start: $(tr '\n' ' ' <"$TMP/lobby-$PORT.log"))" "$INTEGRATION_AREAS"
fi
stop_lobby

TLS_AREAS="$A_WS;$A_NET"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/key.pem" -out "$TMP/cert.pem" -days 2 -subj "/CN=localhost" \
  -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1
TLS_PORT=$((20000 + RANDOM % 2000))
TLS_LOBBY_PORT=$((22000 + RANDOM % 2000))
"$NODE" "$SRC/server/eck-lobby/local/server.js" --host 127.0.0.1 --port "$TLS_LOBBY_PORT" --db :memory: >"$TMP/tls-lobby.log" 2>&1 &
TLS_LOBBY_PID=$!
wait_for_port "$TLS_LOBBY_PORT"
"$NODE" "$SRC/tests/tls_front.mjs" "$TMP/key.pem" "$TMP/cert.pem" "$TLS_LOBBY_PORT" "$TLS_PORT" >"$TMP/tls.log" 2>&1 &
TLS_PID=$!
if [ -s "$TMP/cert.pem" ] && wait_for_port "$TLS_PORT"; then
  check "WebSocket over TLS: libssl loaded at run time, system certificate configuration honoured, bad certificates refused" \
    "$TLS_AREAS" env ECK_TLS_TEST_URL="wss://localhost:$TLS_PORT/" ECK_TLS_TEST_CA="$TMP/cert.pem" \
    "$TESTS/eck_online_websocket_tls_tests"
  EXECUTED="$EXECUTED eck_online_websocket_tls_tests "
  TLS_INTEGRATION="online integration over wss:// (production client, system OpenSSL, Worker behind TLS)"
  check "$TLS_INTEGRATION" "$TLS_AREAS;$A_RELAY;$A_RACE" env SSL_CERT_FILE="$TMP/cert.pem" SSL_CERT_DIR=/nonexistent \
    ECK_LOBBY_TEST_URL="wss://localhost:$TLS_PORT/" "$TESTS/eck_online_integration_tests"
else
  record FAIL "WebSocket over TLS (the TLS front or its certificate could not be created: $(tr '\n' ' ' <"$TMP/tls.log"))" "$TLS_AREAS"
fi

# One Windows build and one Linux build of the production client in a race, through one Worker.
cross_os_online() {
  local direction host_platform guest_platform dir port win_dir win_url code_a code_b failed=0 windows_temp
  windows_temp="$(wslpath "$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')")" || return 1
  [ -d "$windows_temp" ] || { echo "PARITY FAIL: the Windows temp folder is not reachable from Linux"; return 1; }
  for direction in windows-host linux-host; do
    if [ "$direction" = windows-host ]; then host_platform=Windows guest_platform=Linux; else host_platform=Linux guest_platform=Windows; fi
    dir="$windows_temp/eck-cross-$direction-$$"
    win_dir="$(wslpath -w "$dir")"
    port=$((24000 + RANDOM % 2000))
    rm -rf "$dir"; mkdir -p "$dir"
    start_lobby "$port" || { echo "PARITY FAIL: the local Worker did not start"; return 1; }
    local win_url="ws://localhost:$port/" linux_url="ws://127.0.0.1:$port/"
    local host_log="$TMP/cross-$direction-host.log" guest_log="$TMP/cross-$direction-guest.log"
    if [ "$direction" = windows-host ]; then
      timeout 180 "$WINDOWS_INTEGRATION" --role host --platform Windows --expect-peer Linux --lobby "$win_url" --sync "$win_dir" >"$host_log" 2>&1 &
      code_a=$!
      timeout 180 "$TESTS/eck_online_integration_tests" --role guest --platform Linux --expect-peer Windows --lobby "$linux_url" --sync "$dir" >"$guest_log" 2>&1 &
      code_b=$!
    else
      timeout 180 "$TESTS/eck_online_integration_tests" --role host --platform Linux --expect-peer Windows --lobby "$linux_url" --sync "$dir" >"$host_log" 2>&1 &
      code_a=$!
      timeout 180 "$WINDOWS_INTEGRATION" --role guest --platform Windows --expect-peer Linux --lobby "$win_url" --sync "$win_dir" >"$guest_log" 2>&1 &
      code_b=$!
    fi
    wait "$code_a"; code_a=$?
    wait "$code_b"; code_b=$?
    echo "-- $host_platform host, $guest_platform guest (host exit $code_a, guest exit $code_b)"
    sed 's/^/   host:  /' "$host_log" | tr -d '\r'
    sed 's/^/   guest: /' "$guest_log" | tr -d '\r'
    stop_lobby
    rm -rf "$dir"
    [ "$code_a" = 0 ] && [ "$code_b" = 0 ] || failed=1
  done
  return $failed
}
CROSS_AREAS="$A_CROSS;$A_NET;$A_RELAY;$A_RACE;$A_WORKER"
if [ -n "$WINDOWS_INTEGRATION" ]; then
  check "Windows and Linux builds race each other through one Worker, in both directions (handshake, identity, seats, race sync, input, snapshot, RTT, telemetry, disconnect)" \
    "$CROSS_AREAS" cross_os_online
elif [ "$COMPLETE" = 1 ]; then
  record FAIL "the Windows/Linux online harness needs the Windows build of eck_online_integration_tests" "$CROSS_AREAS"
else
  echo "== Windows/Linux online harness: not run, no Windows build was given (--windows-integration)"
fi

# ---- ReXGlue: patch sets, stamps, and the files that must be refused ------------------------------------------------

rexglue_checks() {
  local patches="$SRC/patches/rexglue-sdk" builder="$SRC/tools/build-patched-rexruntime.sh" listed fingerprint control="$TMP/rexneg"
  local name column2 problems=0 common=0 windows=0 linux=0 stock lib
  listed="$(bash "$builder" --list)" || return 1
  fingerprint="$(sed -n 's/^fingerprint //p' <<<"$listed")"
  [ -n "$fingerprint" ] || { echo "PARITY FAIL: the patch list has no fingerprint"; return 1; }
  [ "$(bash "$builder" --list | sed -n 's/^fingerprint //p')" = "$fingerprint" ] ||
    { echo "PARITY FAIL: the patch list is not deterministic"; return 1; }
  echo "@@STAT linux_rexglue_patches=$(grep -c '\.patch$' <<<"$listed")"
  echo "@@STAT linux_rexglue_fingerprint=$fingerprint"
  echo "Linux patch set: $(grep -c '\.patch$' <<<"$listed") patches, fingerprint ${fingerprint:0:16}"

  for path in "$patches"/*.patch; do
    name="$(basename "$path")"
    case "$name" in
      *.windows.patch)
        windows=$((windows + 1))
        grep -qxF "$name" <<<"$listed" && { echo "PARITY FAIL: $name is a Windows patch but the Linux build applies it"; problems=1; }
        grep -qE "^$name " "$patches/platforms.txt" || { echo "PARITY FAIL: $name is not paired with a Linux patch in platforms.txt"; problems=1; } ;;
      *.linux.patch)
        linux=$((linux + 1))
        grep -qxF "$name" <<<"$listed" || { echo "PARITY FAIL: $name is a Linux patch the Linux build does not apply"; problems=1; }
        grep -qE " $name\$" "$patches/platforms.txt" || { echo "PARITY FAIL: $name is not listed in platforms.txt"; problems=1; } ;;
      *)
        common=$((common + 1))
        grep -qxF "$name" <<<"$listed" || { echo "PARITY FAIL: the common patch $name is missing from the Linux build"; problems=1; } ;;
    esac
  done
  while read -r name column2; do
    case "$name" in '#'* | '') continue ;; esac
    { [ "$name" = - ] || [ -f "$patches/$name" ]; } && [ -f "$patches/$column2" ] ||
      { echo "PARITY FAIL: platforms.txt names a patch that does not exist: $name $column2"; problems=1; }
  done <"$patches/platforms.txt"
  for name in $(grep '\.patch$' <<<"$listed"); do
    [ -f "$patches/$name" ] || { echo "PARITY FAIL: the Linux build lists $name, which does not exist"; problems=1; }
  done
  echo "patch classification: $common common, $windows Windows-only (each paired), $linux Linux-only or paired"

  local zip="$ORIGIN/tools/rexglue/rexglue-sdk-0.10.0-linux-amd64.zip" scale_warning='needs the host render target path'
  [ -f "$zip" ] || zip="$SRC/tools/rexglue/rexglue-sdk-0.10.0-linux-amd64.zip"
  for lib in librexgpu-xenos.so librexgpu-xenosrd.so; do
    grep -qaF "$scale_warning" "$SDK/lib/$lib" ||
      { echo "PARITY FAIL: $lib has no Internal render resolution fallback warning"; problems=1; }
  done
  if unzip -p "$zip" linux-amd64/lib/librexgpu-xenos.so 2>/dev/null | grep -qaF "$scale_warning"; then
    echo "PARITY FAIL: the stock GPU plugin already has the fallback warning, so it proves nothing"; problems=1
  fi

  rm -rf "$control"
  mkdir -p "$control/lib"
  for lib in librexruntime.so librexruntimerd.so librexgpu-xenos.so librexgpu-xenosrd.so; do ln -s "$SDK/lib/$lib" "$control/lib/$lib"; done
  cp "$SDK/rexruntime-patch.txt" "$control/"
  bash "$builder" --check "$control" >/dev/null 2>&1 || { echo "PARITY FAIL: the control copy of the patched runtime is not accepted"; problems=1; }
  refuses() {  # what: the copy in $control must now be refused
    if bash "$builder" --check "$control" >/dev/null 2>&1; then echo "PARITY FAIL: a $1 runtime was accepted"; problems=1; else echo "refused: $1"; fi
  }
  cp "$SDK/rexruntime-patch.txt" "$control/rexruntime-patch.txt.good"
  sed -i 's/^inputs_sha256=.*/inputs_sha256=0000000000000000000000000000000000000000000000000000000000000000/' "$control/rexruntime-patch.txt"
  refuses "stale (built from other inputs)"
  cp "$control/rexruntime-patch.txt.good" "$control/rexruntime-patch.txt"
  rm "$control/lib/librexgpu-xenos.so"
  cp "$SDK/lib/librexgpu-xenos.so" "$control/lib/librexgpu-xenos.so"
  printf 'x' >>"$control/lib/librexgpu-xenos.so"
  refuses "modified"
  rm "$control/lib/librexgpu-xenos.so"
  unzip -p "$zip" linux-amd64/lib/librexgpu-xenos.so >"$control/lib/librexgpu-xenos.so" 2>/dev/null
  stock="$(sed -n 's/^OFFICIAL_LINUX_GPU_SHA256=//p' "$patches/baseline.env" | tr -d '\r')"
  if [ "$(sha256sum "$control/lib/librexgpu-xenos.so" | cut -d' ' -f1)" = "$stock" ]; then
    sed -i "s|^[0-9a-f]\{64\}  lib/librexgpu-xenos.so\$|$stock  lib/librexgpu-xenos.so|" "$control/rexruntime-patch.txt"
    refuses "stock GPU plugin without the Internal render resolution fallback"
    cp "$control/rexruntime-patch.txt.good" "$control/rexruntime-patch.txt"
  else
    echo "PARITY FAIL: the stock GPU plugin could not be extracted for the control run"; problems=1
  fi
  rm "$control/lib/librexgpu-xenos.so"
  ln -s "$SDK/lib/librexgpu-xenos.so" "$control/lib/librexgpu-xenos.so"
  stock="$(sed -n 's/^OFFICIAL_LINUX_RUNTIME_SHA256=//p' "$patches/baseline.env" | tr -d '\r')"
  rm "$control/lib/librexruntime.so"
  unzip -p "$zip" linux-amd64/lib/librexruntime.so >"$control/lib/librexruntime.so" 2>/dev/null
  if [ "$(sha256sum "$control/lib/librexruntime.so" | cut -d' ' -f1)" = "$stock" ]; then
    sed -i "s|^[0-9a-f]\{64\}  lib/librexruntime.so\$|$stock  lib/librexruntime.so|" "$control/rexruntime-patch.txt"
    refuses "stock ReXGlue"
  else
    echo "PARITY FAIL: the stock runtime could not be extracted for the control run"; problems=1
  fi
  rm "$control/lib/librexruntime.so"
  ln -s "$SDK/lib/librexruntime.so" "$control/lib/librexruntime.so"
  cp "$control/rexruntime-patch.txt.good" "$control/rexruntime-patch.txt"
  rm "$control/rexruntime-patch.txt"
  refuses "unstamped"
  return $problems
}
check "ReXGlue: Linux patch set deterministic and classified, stamp current, stale/modified/stock/unstamped runtimes refused" \
  "$A_REX;$A_ENV" rexglue_checks

# ---- New compiler warnings in the program's own sources --------------------------------------------------------------

warning_check() {
  local list="$TMP/warning-sources.txt" log="$TMP/warnings.log" src object line file flag new=0 known=0 third=0 known_file="$SRC/tests/known-warnings.txt"
  {
    (cd "$SRC" && grep -rlE '_WIN32|__linux__' src --include='*.cpp' | grep -v third_party)
    [ -z "$CHANGED_FILES" ] || grep -E '^src/.*\.cpp$' "$CHANGED_FILES"
  } | sort -u >"$list"
  while read -r src; do
    object="$BUILD/CMakeFiles/el_chavo_kart_linux_production_check.dir/$src.o"
    [ ! -f "$object" ] || rm -f "$object"
  done <"$list"
  ninja -C "$BUILD" el_chavo_kart_linux_production_check >"$log" 2>&1 || { tail -20 "$log"; return 1; }
  while IFS= read -r line; do
    file="${line%%:*}"
    flag="$(grep -oE '\[-W[a-z0-9-]+\]' <<<"$line" | head -1 | tr -d '[]')"
    case "$file" in
      *third_party* | */trg-launcher* | */tools/rexglue/* | */rexglue-sdk-src/*) third=$((third + 1)) ;;
      *)
        if grep -qxF "linux|${file#"$SRC/"}|$flag" "$known_file"; then known=$((known + 1))
        else new=$((new + 1)); echo "PARITY FAIL: new warning: ${line:0:200}"; fi ;;
    esac
  done < <(grep -E 'warning:' "$log" | sort -u)
  echo "compiled $(wc -l <"$list") platform-sensitive or changed sources: $new new, $known known, $third third-party warnings"
  echo "@@STAT linux_warnings_new=$new"
  return $((new > 0))
}
check "no new compiler warnings in the program's own sources (platform-sensitive and changed files rebuilt)" "$A_WARN" warning_check

# ---- Wire vectors against the other platform and the Worker --------------------------------------------------------

VECTORS="$TMP/linux-vectors.txt"
"$TESTS/eck_wire_parity_tests" --print >"$VECTORS"
[ -n "$EMIT_VECTORS" ] && cp "$VECTORS" "$EMIT_VECTORS"
check "Worker reads the vectors this Linux build produced" "$A_WIRE;$A_WORKER;$A_RELAY" \
  bash -c "cd '$SRC/server/eck-lobby' && ECK_WIRE_VECTORS='$VECTORS' '$NODE' --test test/wire.test.js"
if [ -n "$PEER_VECTORS" ]; then
  check "Linux reads and reproduces the vectors the Windows build produced" "$A_WIRE;$A_NET;$A_RELAY;$A_CROSS" \
    "$TESTS/eck_wire_parity_tests" --peer "$PEER_VECTORS"
  check "Worker reads the vectors the Windows build produced" "$A_WIRE;$A_WORKER;$A_RELAY;$A_CROSS" \
    bash -c "cd '$SRC/server/eck-lobby' && ECK_WIRE_VECTORS='$PEER_VECTORS' '$NODE' --test test/wire.test.js"
fi

# ---- Binaries: GLIBC, RUNPATH, dependencies --------------------------------------------------------------------------

inspect_binaries() {
  local file label failures=0 highest="" glibc path
  echo "== Shipped runtime libraries and the production link"
  for file in "$SDK/lib/librexruntime.so" "$SDK/lib/librexgpu-xenos.so" "$BUILD/el_chavo_kart_linux_production_check"; do
    [ -f "$file" ] || { echo "PARITY FAIL: missing $file"; failures=$((failures + 1)); continue; }
    check_elf_file "$(basename "$file")" "$file" ' libX11.so.6 libX11-xcb.so.1 libxcb.so.1 libwayland-client.so.0 ' ||
      failures=$((failures + $?))
    glibc="$(readelf -V -W "$file" 2>/dev/null | elf_glibc_text)"
    [ -n "$glibc" ] && highest="$(printf '%s\n%s\n' "$highest" "$glibc" | sort -V | tail -1)"
  done
  echo "== Every test program (GLIBC versions)"
  for file in "$TESTS"/eck_*; do
    [ -f "$file" ] && [ -x "$file" ] && [ "$(head -c 4 "$file")" = $'\x7fELF' ] || continue
    glibc="$(readelf -V -W "$file" 2>/dev/null | elf_glibc_text)"
    check_elf_glibc "$(basename "$file")" "$glibc" || failures=$((failures + 1))
    [ -n "$glibc" ] && highest="$(printf '%s\n%s\n' "$highest" "$glibc" | sort -V | tail -1)"
  done
  echo "@@ENV highest_glibc=GLIBC_$highest"
  echo "  highest GLIBC symbol version required by any Linux binary built or shipped here: GLIBC_$highest (baseline GLIBC_$MAX_GLIBC)"
  echo "== Local paths in the production link"
  for path in "$ROOT" "$WORK" "$HOME" 'S:\' "/mnt/s/"; do
    if grep -qaF -- "$path" "$BUILD/el_chavo_kart_linux_production_check"; then
      echo "PARITY FAIL: the production link contains the local path $path"
      failures=$((failures + 1))
    fi
  done
  return $failures
}

# ---- The production installer on the real disc image, when this checkout has one ------------------------------------

REAL_IMAGE="$(find -L "$SRC/game" -maxdepth 2 -iname '*.iso' 2>/dev/null | head -1)"
if [ -n "$REAL_IMAGE" ]; then
  REAL_INSTALL="$TMP/pasta com espaço/jogo"
  mkdir -p "$REAL_INSTALL"
  check "installer on the real disc image (case-sensitive file system, a folder with spaces and accents, default.xex size and SHA-256, every required file)" \
    "$A_INST;$A_DATA" "$TESTS/eck_installer_tests" --real-image "$REAL_IMAGE" "$REAL_INSTALL/game"
  rm -rf "$TMP/pasta com espaço"
else
  echo "== installer on the real disc image: not run, game/ has no disc image here (the suite above used synthetic images)"
fi

echo "== Binary inspection"
BIN_LOG="$TMP/binaries.log"
inspect_binaries 2>&1 | tee "$BIN_LOG"
BIN_FAILURES=${PIPESTATUS[0]}
glibc_fail="$(grep -c 'requires GLIBC' "$BIN_LOG")"
if [ "$glibc_fail" = 0 ] && [ "$PROD_OK" = 1 ]; then record PASS "no Linux binary needs a glibc newer than $MAX_GLIBC ($(grep -o 'GLIBC_[0-9.]*  *(baseline' "$BIN_LOG" | head -1 | sed 's/ (baseline//'))" "$A_GLIBC"
else record FAIL "a Linux binary needs a glibc newer than $MAX_GLIBC, or none could be inspected" "$A_GLIBC"; fi
if ! grep -q 'PARITY FAIL: Linux RPATH/RUNPATH' "$BIN_LOG" && [ "$PROD_OK" = 1 ]; then
  record PASS "RPATH/RUNPATH of the production link and the shipped runtime libraries is \$ORIGIN only" "$A_RPATH"
else record FAIL "RPATH/RUNPATH leaks a path or could not be inspected" "$A_RPATH"; fi
if ! grep -q 'PARITY FAIL: Linux binary\|contains the local path' "$BIN_LOG" && [ "$PROD_OK" = 1 ]; then
  record PASS "dynamic dependencies of the production link and the shipped runtime libraries are as intended (no libssl, no RenderDoc, no layers, no local paths)" "$A_DEPS"
else record FAIL "unexpected dynamic dependencies or local paths in a Linux binary" "$A_DEPS"; fi
[ "$BIN_FAILURES" = 0 ] || echo "  ($BIN_FAILURES binary check(s) failed)"

# ---- Packaging: a fixture AppDir staged and scanned by the release policy, then run as an AppImage ---------------------

package_fixture() {
  local fix="$TMP/package" bin="$TMP/package/bin" appdir="$TMP/package/AppDir" id
  rm -rf "$fix"
  mkdir -p "$bin/assets/launcher"
  policy_init "$SRC"
  id="$APP_ID"
  cat >"$fix/stub.c" <<STUB
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static const char* const kApplicationId = "$id";
int main(int argc, char** argv) {
  const char* image = getenv("APPIMAGE");
  const char* appdir = getenv("APPDIR");
  char exe[4096] = {0};
  if (readlink("/proc/self/exe", exe, sizeof exe - 1) < 0) exe[0] = 0;
  printf("STUB id=%s appimage=%s appdir=%s exe=%s argc=%d\n", kApplicationId, image ? image : "", appdir ? appdir : "", exe, argc);
  if (image) {
    char copy[4096];
    snprintf(copy, sizeof copy, "%s", image);
    char folder[4096];
    snprintf(folder, sizeof folder, "%s/user_data", dirname(copy));
    mkdir(folder, 0755);
    char file[4200];
    snprintf(file, sizeof file, "%s/stub.txt", folder);
    FILE* out = fopen(file, "w");
    if (out) { fprintf(out, "%s\n", appdir ? appdir : ""); fclose(out); }
  }
  return argc > 1 && strcmp(argv[1], "--exit7") == 0 ? 7 : 0;
}
STUB
  printf 'void eck_stub_library(void) {}\n' >"$fix/lib.c"
  clang-20 -O1 -g -Wl,-rpath,'$ORIGIN' "$fix/stub.c" -o "$bin/$BIN_NAME" || return 1
  clang-20 -shared -fPIC -O1 -Wl,-rpath,'$ORIGIN' "$fix/lib.c" -o "$bin/librexruntime.so" || return 1
  clang-20 -shared -fPIC -O1 -Wl,-rpath,'$ORIGIN' "$fix/lib.c" -o "$bin/librexgpu-xenos.so" || return 1
  cp "$BUILD/shipped-config/$BIN_NAME.toml" "$bin/$BIN_NAME.toml"
  cp "$SRC/assets/launcher/header-art.png" "$SRC/assets/launcher/sponsor-heart.png" "$bin/assets/launcher/"
  stage_appdir "$appdir" "$bin" "$fix/symbols" "$SRC" 0.0.0
}

echo "== Staging a fixture AppDir"
if package_fixture; then
  record PASS "fixture AppDir staged by the packaging script's own staging function" "$A_PKG"
else
  record FAIL "the fixture AppDir could not be staged" "$A_PKG"
fi

APPDIR_FIXTURE="$TMP/package/AppDir"
echo "== Release policy on the fixture AppDir"
if [ -d "$APPDIR_FIXTURE" ] && ( scan_appdir "$APPDIR_FIXTURE" "fixture AppDir" "$ROOT" "$ORIGIN" "$WORK" "$HOME" "${PRIVATE_NAMES[@]}" ); then
  record PASS "release policy accepts the staged fixture AppDir (layout, licenses, README, desktop file, icons, AppRun, RUNPATH, glibc, no local paths)" "$A_PKG;$A_GLIBC;$A_RPATH"
else
  record FAIL "release policy rejects the staged fixture AppDir" "$A_PKG"
fi

check "the shipped configuration keeps VSync on with the Vulkan presenter's setting" "$A_DISP;$A_PKG" \
  grep -qxF 'vulkan_allow_present_mode_immediate = false' "$APPDIR_FIXTURE/usr/bin/$BIN_NAME.toml"

forbidden_case() {  # description reason-the-scan-must-give setup-command (run inside a copy of the AppDir)
  local copy="$TMP/forbidden" report
  rm -rf "$copy"
  cp -a "$APPDIR_FIXTURE" "$copy"
  ( cd "$copy" && eval "$3" ) >/dev/null 2>&1
  report="$( ( scan_appdir "$copy" "fixture AppDir" "$ROOT" "$WORK" "$HOME" zz-test-user "${PRIVATE_NAMES[@]}" ) 2>&1 )"
  case "$report" in
    *"$2"*) ;;
    *) echo "PARITY FAIL: the release policy does not reject $1 for the right reason (expected '$2')"; return 1 ;;
  esac
}

# Each case adds one forbidden thing to a copy of the clean AppDir; the scan must name that rule, not only notice an unlisted file.
forbidden_policy() {
  local failed=0 name="forbidden name" type="forbidden file type" layer="RenderDoc, Tracy or Vulkan layer file"
  forbidden_case "default.xex" "$name" "touch usr/bin/default.xex" || failed=1
  forbidden_case "game data" "$name" "mkdir usr/bin/game && touch usr/bin/game/x" || failed=1
  forbidden_case "the game's folder" "$name" "mkdir usr/bin/ChavoKartGame" || failed=1
  forbidden_case "a title folder" "$name" "mkdir usr/bin/475807D7" || failed=1
  forbidden_case "content" "$name" "mkdir usr/bin/content" || failed=1
  forbidden_case "a SystemUpdate folder" "$name" "mkdir 'usr/bin/\$SystemUpdate'" || failed=1
  forbidden_case "user_data" "$name" "mkdir usr/bin/user_data" || failed=1
  forbidden_case "a settings.toml leftover" "$name" "touch usr/bin/settings.toml" || failed=1
  forbidden_case "saves" "$type" "touch usr/bin/save.sav" || failed=1
  forbidden_case "logs" "$type" "touch usr/bin/el_chavo_kart_001.log" || failed=1
  forbidden_case "a disc image" "$type" "touch usr/bin/game.iso" || failed=1
  forbidden_case "a game package" "$type" "touch usr/bin/ChavoKartGame.xxx" || failed=1
  forbidden_case "a game movie" "$type" "touch usr/bin/intro.bik" || failed=1
  forbidden_case "an Xbox executable file" "$type" "touch usr/bin/image.xex" || failed=1
  forbidden_case "a RenderDoc capture" "$type" "touch usr/bin/capture.rdc" || failed=1
  forbidden_case "a git file" "$type" "touch usr/bin/.gitignore" || failed=1
  forbidden_case "an .orig leftover" "$type" "touch usr/bin/a.orig" || failed=1
  forbidden_case "a .rej leftover" "$type" "touch usr/bin/a.rej" || failed=1
  forbidden_case "a temporary file" "$type" "touch usr/bin/a.tmp" || failed=1
  forbidden_case "an editor backup" "$type" "touch 'usr/bin/a~'" || failed=1
  forbidden_case "a debug symbol file" "$type" "touch usr/bin/program.debug" || failed=1
  forbidden_case "a PDB" "$type" "touch usr/bin/program.pdb" || failed=1
  forbidden_case "a Windows DLL" "$type" "touch usr/bin/rexruntime.dll" || failed=1
  forbidden_case "a source file" "$type" "touch usr/bin/main.cpp" || failed=1
  forbidden_case "an object file" "$type" "touch usr/bin/main.o" || failed=1
  forbidden_case "a developer script" "$type" "touch usr/bin/build.ps1" || failed=1
  forbidden_case "a private key" "$type" "touch usr/bin/server.key" || failed=1
  forbidden_case "TracyClient" "$layer" "touch usr/bin/TracyClient.so" || failed=1
  forbidden_case "a RenderDoc library" "$layer" "touch usr/bin/librenderdoc.so" || failed=1
  forbidden_case "a Vulkan layer manifest" "$layer" "touch usr/bin/VkLayer_test.json" || failed=1
  forbidden_case "an Xbox 360 package" "Xbox 360 package or executable" "printf 'LIVE0000' > usr/bin/package.bin" || failed=1
  forbidden_case "an Xbox 360 executable" "Xbox 360 package or executable" "printf 'XEX20000' > usr/bin/image.bin" || failed=1
  forbidden_case "a Windows binary" "Windows binary" "printf 'MZ000000' > usr/bin/tool.bin" || failed=1
  forbidden_case "DEBUG_SAULO diagnostics" "DEBUG_SAULO in file" "echo DEBUG_SAULO >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a local path inside a file" "local path in file" "echo '$ROOT' >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a symbolic link" "symbolic link" "ln -s usr/bin/$BIN_NAME link" || failed=1
  forbidden_case "a private user name from a test machine" "local path in file" "echo zz-test-user >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "an admin token name" "secret or token" "echo 'ADMIN_TOKEN=x' >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a Cloudflare token name" "secret or token" "echo 'CLOUDFLARE_API_TOKEN' >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a GitHub token" "secret or token" "printf 'ghp_%s' \"\$(printf 'a%.0s' \$(seq 36))\" >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a fine-grained GitHub token" "secret or token" "printf 'github_pat_%s' \"\$(printf 'b%.0s' \$(seq 40))\" >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a private key" "secret or token" "echo '-----BEGIN RSA PRIVATE KEY-----' >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "an online identity token" "secret or token" "printf '\"token\":\"%s\"' \"\$(printf 'a%.0s' \$(seq 64))\" >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "a friend code in an identity line" "secret or token" "echo 'player_id=ABCD-2345' >> usr/share/doc/$BIN_NAME/LICENSE" || failed=1
  forbidden_case "an online identity file" "$name" "touch usr/bin/eck_online_identity.ini" || failed=1
  forbidden_case "an unlisted file" "unexpected AppDir contents" "touch usr/bin/extra.bin" || failed=1
  forbidden_case "a missing license" "unexpected AppDir contents" "rm usr/share/doc/$BIN_NAME/THIRD-PARTY-LICENSES-LINUX.txt" || failed=1
  forbidden_case "a missing README" "unexpected AppDir contents" "rm usr/share/doc/$BIN_NAME/README.txt" || failed=1
  forbidden_case "an absolute RUNPATH on the program" "RPATH/RUNPATH" "patchelf --set-rpath /opt/lib usr/bin/$BIN_NAME" || failed=1
  forbidden_case "a build folder as RUNPATH of the runtime" "RPATH/RUNPATH" "patchelf --set-rpath '\$ORIGIN:$WORK/tools/rexglue/linux-amd64/lib' usr/bin/librexruntime.so" || failed=1
  forbidden_case "a program without an \$ORIGIN RUNPATH" "RPATH/RUNPATH" "patchelf --remove-rpath usr/bin/$BIN_NAME" || failed=1
  forbidden_case "a desktop file naming another window class" "StartupWMClass" "sed -i 's/^StartupWMClass=.*/StartupWMClass=other/' $APP_ID.desktop" || failed=1
  forbidden_case "a desktop file that opens a terminal" "Terminal=false" "sed -i 's/^Terminal=.*/Terminal=true/' $APP_ID.desktop" || failed=1
  forbidden_case "an AppRun that is not executable" "AppRun is not executable" "chmod 644 AppRun" || failed=1
  return $failed
}

if [ -d "$APPDIR_FIXTURE" ]; then
  check "release policy rejects every forbidden kind of content, secret, token, identity, absolute RUNPATH and local path" "$A_PKG;$A_RPATH;$A_PRIV" forbidden_policy
fi

echo "== Fixture AppImage (pinned appimagetool and patched runtime; the stub program only prints its environment)"
appimage_fixture() {
  local out="$TMP/appimage" image failed=0 output status extracted
  rm -rf "$out"
  mkdir -p "$out"
  ensure_appimage_tools "$SRC" "$WORK" || return 1
  image="$out/ElChavoKart-v0.0.0-x86_64.AppImage"
  ARCH=x86_64 "$APPIMAGETOOL" --appimage-extract-and-run --no-appstream --runtime-file "$APPIMAGE_RUNTIME" \
    "$APPDIR_FIXTURE" "$image" >/dev/null 2>&1 || { echo "PARITY FAIL: appimagetool could not build the fixture AppImage"; return 1; }
  chmod 755 "$image"
  bash "$SRC/tools/build-appimage-runtime.sh" --check-image "$image" || { echo "PARITY FAIL: the fixture AppImage does not start with the reviewed runtime"; failed=1; }
  cp "$image" "$image.tampered"
  printf 'X' | dd of="$image.tampered" bs=1 seek=5000 conv=notrunc 2>/dev/null
  if bash "$SRC/tools/build-appimage-runtime.sh" --check-image "$image.tampered" >/dev/null 2>&1; then
    echo "PARITY FAIL: an AppImage with an altered runtime was accepted"; failed=1
  fi
  rm -f "$image.tampered"
  bash "$SRC/tools/build-appimage-runtime.sh" --check "$image" || { echo "PARITY FAIL: the fixture AppImage does not carry the pinned runtime"; failed=1; }

  echo "-- extract-and-run"
  output="$(cd "$out" && APPIMAGE_EXTRACT_AND_RUN=1 "$image" one two 2>&1)"; status=$?
  echo "$output" | sed 's/^/   /'
  [ "$status" = 0 ] || { echo "PARITY FAIL: extract-and-run exited $status"; failed=1; }
  case "$output" in *"appimage=$image "*) ;; *) echo "PARITY FAIL: extract-and-run does not expose the original AppImage path"; failed=1 ;; esac
  case "$output" in *"appdir=/tmp/appimage_extracted_"*) ;; *) echo "PARITY FAIL: extract-and-run did not run from an extracted folder"; failed=1 ;; esac
  [ -f "$out/user_data/stub.txt" ] || { echo "PARITY FAIL: user data was not written beside the AppImage"; failed=1; }
  extracted="$(echo "$output" | sed -n 's/.* appdir=\([^ ]*\) .*/\1/p' | head -1)"
  [ -n "$extracted" ] && [ ! -e "$extracted" ] || { echo "PARITY FAIL: the extracted folder ${extracted:-?} was left behind"; failed=1; }
  rm -rf "$out/user_data"

  echo "-- exit status is passed through"
  (cd "$out" && APPIMAGE_EXTRACT_AND_RUN=1 "$image" --exit7 >/dev/null 2>&1); status=$?
  [ "$status" = 7 ] || { echo "PARITY FAIL: the program's exit status 7 became $status"; failed=1; }
  rm -rf "$out/user_data"

  echo "-- FUSE unavailable: the runtime falls back to extract-and-run"
  # /dev/fuse is replaced inside a private mount namespace, so the kernel refuses the mount as it does where FUSE is missing.
  output="$(cd "$out" && unshare -m bash -c 'mount --bind /dev/null /dev/fuse && exec "$0" fallback' "$image" 2>&1)"; status=$?
  echo "$output" | sed 's/^/   /'
  [ "$status" = 0 ] || { echo "PARITY FAIL: the AppImage did not start without FUSE (exit $status)"; failed=1; }
  case "$output" in *"argc=2"*) ;; *) echo "PARITY FAIL: the program did not run without FUSE"; failed=1 ;; esac
  case "$output" in *"appdir=/tmp/appimage_extracted_"*) ;; *) echo "PARITY FAIL: without FUSE the program did not run from an extracted folder"; failed=1 ;; esac
  [ -f "$out/user_data/stub.txt" ] || { echo "PARITY FAIL: user data was not written beside the AppImage without FUSE"; failed=1; }
  rm -rf "$out/user_data"
  return $failed
}
if [ -d "$APPDIR_FIXTURE" ]; then
  check "fixture AppImage: pinned runtime, extract-and-run, original AppImage path, external user_data, FUSE fallback" \
    "$A_PKG;$A_DATA;$A_REL" appimage_fixture
fi

# ---- Coverage: nothing configured was left unrun ---------------------------------------------------------------------

executed_problems() {
  local file name problems=0 expected helpers=" eck_fake_vulkan "
  for file in "$TESTS"/eck_*; do
    [ -f "$file" ] && [ -x "$file" ] && [ "$(head -c 4 "$file")" = $'\x7fELF' ] || continue
    name="$(basename "$file")"
    case "$EXECUTED$helpers" in *" $name "* | *"$name "*) ;; *) echo "PARITY FAIL: $name was built but never executed"; problems=1 ;; esac
  done
  for name in $(sed -n -E 's/^(service|linux-service)[[:space:]]+([a-z0-9_]+)[[:space:]]*$/\2/p' "$SRC/tests/required-suites.txt"); do
    case "$EXECUTED" in *"$name "*) ;; *) echo "PARITY FAIL: the required test $name was not executed"; problems=1 ;; esac
  done
  echo "$SUITE_COUNT suites ran, $CHECK_COUNT checks reported, $FAILED_CHECKS reported failed"
  echo "@@STAT linux_suites=$SUITE_COUNT"
  echo "@@STAT linux_checks=$CHECK_COUNT"
  echo "@@STAT linux_check_failures=$FAILED_CHECKS"
  return $problems
}
check "every built test program was executed and every required test ran" "$A_CORE" executed_problems

# A package built from these sources into a scratch folder, so the release artifact in dist is never touched. The
# program inside is never started.
scratch_appimage() {
  local out="$TMP/scratch-dist" image log="$TMP/scratch.log"
  [ "$ORIGIN" != "$SRC" ] || { echo "PARITY FAIL: no original checkout to package from (run tools/test.sh without --build-dir)"; return 1; }
  bash "$ORIGIN/tools/package-linux.sh" --out-dir "$out" >"$log" 2>&1 || { tail -40 "$log"; return 1; }
  image="$(ls "$out"/ElChavoKart-v*-x86_64.AppImage 2>/dev/null | head -1)"
  [ -f "$image" ] || { echo "PARITY FAIL: the scratch package produced no AppImage"; return 1; }
  grep -E 'GLIBC|AppImage runtime|clean|checksum' "$log" | sed 's/^/   build: /'
  bash "$ORIGIN/tools/package-linux.sh" --verify-only "$image" 2>&1 | sed 's/^/   verify-only: /'
  [ "${PIPESTATUS[0]}" = 0 ] || return 1
  local check_dir="$TMP/scratch-check" cxx
  mkdir -p "$check_dir"
  cp "$image" "$check_dir/a.AppImage" && (cd "$check_dir" && ./a.AppImage --appimage-extract >/dev/null)
  echo "   scratch AppImage: $(stat -c %s "$image") bytes, SHA-256 $(sha256sum "$image" | cut -d' ' -f1)"
  echo "@@STAT scratch_appimage_bytes=$(stat -c %s "$image")"
  echo "@@STAT scratch_appimage_sha256=$(sha256sum "$image" | cut -d' ' -f1)"
  rm -rf "$check_dir" "$out"
}
if [ "$COMPLETE" = 1 ]; then
  check "a scratch AppImage built from these sources passes package-linux.sh --verify-only (policy, ELF, glibc, GLIBCXX, runtime pins)" \
    "$A_SCRATCH;$A_PKG;$A_GLIBC;$A_RPATH;$A_DEPS;$A_PRIV" scratch_appimage
else
  echo "== scratch AppImage: not run, this is not the complete release mode (--complete)"
fi

# ---- Summary -----------------------------------------------------------------------------------------------------------

FAILED=0
for result in "${RESULTS[@]}"; do
  IFS='|' read -r status _ _ <<<"$result"
  [ "$status" = PASS ] || FAILED=$((FAILED + 1))
done
if [ "$FAILED" = 0 ]; then record PASS "every Linux check passed" "$A_GATE"; else record FAIL "$FAILED Linux check(s) failed" "$A_GATE"; fi

echo
echo "Linux parity ($(. /etc/os-release && echo "$PRETTY_NAME"), glibc $(getconf GNU_LIBC_VERSION | cut -d' ' -f2)):"
echo
for area in "${AREAS[@]}"; do
  pass=0 fail=0
  for result in "${RESULTS[@]}"; do
    IFS='|' read -r status _ areas <<<"$result"
    case ";$areas;" in *";$area;"*) ;; *) continue ;; esac
    [ "$status" = PASS ] && pass=$((pass + 1)) || fail=$((fail + 1))
  done
  if [ "$fail" -gt 0 ]; then text="FAIL ($fail of $((pass + fail)) checks)"
  elif [ "$pass" = 0 ]; then text="FAIL (no check ran)"
  else text="PASS ($pass checks)"
  fi
  printf '  %-34s%s\n' "$area" "$text"
done
echo
for result in "${RESULTS[@]}"; do
  IFS='|' read -r status what _ <<<"$result"
  [ "$status" = PASS ] || echo "  PARITY FAIL: $what"
done
[ "$FAILED" = 0 ] || exit 1
echo "All Linux parity checks passed."
