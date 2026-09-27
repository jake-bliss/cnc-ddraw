#!/usr/bin/env bash
# Runs tests/lomhd_crash_victim.c against the built ddraw.dll under Wine and asserts, per mode, the
# exit code and which report files exist. Each mode runs in a fresh folder.
#
#   WINE=/path/to/wine64 WINEPREFIX=/scratch/prefix tests/lomhd_crash_victim.sh [MODE...]
#
# Use a scratch prefix, never a game's. Needs i686-w64-mingw32-gcc and a built ./ddraw.dll. The hang
# modes open a small window for about 45 seconds each. Set WINE_ENV to anything extra the Wine build
# needs, as one NAME=value (e.g. WINE_ENV="DYLD_FALLBACK_LIBRARY_PATH=/path/to/Frameworks").
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
: "${WINE:?set WINE to a wine64 binary}"
: "${WINEPREFIX:?set WINEPREFIX to a scratch prefix}"
WINESERVER=${WINESERVER:-$(dirname "$WINE")/wineserver}
export WINEPREFIX WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;ddraw=n,b"
[ -n "${WINE_ENV:-}" ] && export "$WINE_ENV"   # one NAME=value; the value may contain spaces

WORK=$(mktemp -d "${TMPDIR:-/tmp}/lomhd_victim.XXXXXX")
i686-w64-mingw32-gcc -O1 -Wall -Wno-infinite-recursion -o "$WORK/victim.exe" "$ROOT/tests/lomhd_crash_victim.c" || exit 1

# mode | exit | crash .txt | .dmp | hang .txt | lomhd.log must contain | stdout must contain | .txt must contain
EXPECT='
av             |44|1|1|0|report written to          |game filter              |ACCESS_VIOLATION
chain          |43|1|1|0|report written to          |calling its predecessor  |
bypass         |45|1|1|0|replaced ours without      |bypass filter            |
bypass-chain   |44|1|1|0|replaced ours without      |bypass filter: calling   |
bypass-restore |44|1|1|0|uninstalled itself         |restored                 |
none           |*|1|1|0|report written to          |crashing now             |
handled        |0 |0|0|0|crash reports: on          |IsBadReadPtr 1           |
noflag         |44|0|0|0|                           |game filter              |
caught         |3 |0|0|0|first-chance c0000005      |caught                   |
overflow       |44|1|1|0|report written to          |crashing now             |STACK_OVERFLOW
overflow-noflag|44|0|0|0|                           |crashing now             |
continue       |47|1|1|0|resumed after              |resumed after the fault  |Outcome:
freelib        |44|1|1|0|report written to          |still loaded after FreeLibrary: yes|
hang           |0 |0|0|1|hang: report written       |not pumping              |
idle           |0 |0|0|0|crash reports: on          |pumping, no DirectDraw   |
minimized      |0 |0|0|0|crash reports: on          |minimized yes            |
background     |0 |0|0|0|crash reports: on          |in front no              |
'

MODES=("$@")
[ ${#MODES[@]} -eq 0 ] && MODES=($(echo "$EXPECT" | awk -F'|' 'NF>1 {gsub(/ /,"",$1); print $1}'))

field() { echo "$1" | cut -d'|' -f"$2" | sed 's/^ *//; s/ *$//'; }
failures=0

for mode in "${MODES[@]}"; do
  line=$(echo "$EXPECT" | awk -F'|' -v m="$mode" '{k=$1; gsub(/ /,"",k)} k==m')
  [ -n "$line" ] || { echo "unknown mode $mode"; failures=$((failures+1)); continue; }
  dir="$WORK/$mode"; mkdir -p "$dir"
  cp "$ROOT/ddraw.dll" "$WORK/victim.exe" "$dir/"
  arg=$mode
  case $mode in
    noflag) arg=av;;
    *-noflag) ;;
    *) touch "$dir/lomhd_crash_reports";;
  esac

  # A mode that hangs is a failure, not a stuck run: killed after TIMEOUT seconds (exit 124).
  ( cd "$dir" && exec "$WINE" ./victim.exe "$arg" > stdout.txt 2> stderr.txt ) & pid=$!
  waited=0
  while kill -0 $pid 2>/dev/null && [ $waited -lt ${TIMEOUT:-90} ]; do sleep 1; waited=$((waited+1)); done
  if kill -0 $pid 2>/dev/null; then "$WINESERVER" -k 2>/dev/null; wait $pid; code=124; else wait $pid; code=$?; fi
  "$WINESERVER" -k 2>/dev/null

  want_code=$(field "$line" 2); want_txt=$(field "$line" 3); want_dmp=$(field "$line" 4)
  want_hang=$(field "$line" 5); want_log=$(field "$line" 6); want_out=$(field "$line" 7); want_in_txt=$(field "$line" 8)
  have_txt=$(ls "$dir"/lomhd_crash_*.txt 2>/dev/null | wc -l | tr -d ' ')
  have_dmp=$(ls "$dir"/lomhd_crash_*.dmp 2>/dev/null | wc -l | tr -d ' ')
  have_hang=$(ls "$dir"/lomhd_hang_*.txt 2>/dev/null | wc -l | tr -d ' ')
  problems=""

  if [ "$want_code" = "*" ]; then
    case $code in 0|3|43|44|45|46|47) problems+=" exit $code (want a crash exit, not one of the victim's)";; esac
  elif [ "$code" != "$want_code" ]; then problems+=" exit $code (want $want_code)"; fi
  [ "$have_txt" = "$want_txt" ] || problems+=" $have_txt crash .txt (want $want_txt)"
  [ "$have_dmp" = "$want_dmp" ] || problems+=" $have_dmp .dmp (want $want_dmp)"
  [ "$have_hang" = "$want_hang" ] || problems+=" $have_hang hang .txt (want $want_hang)"
  if [ -n "$want_log" ]; then grep -q -- "$want_log" "$dir/lomhd.log" 2>/dev/null || problems+=" lomhd.log lacks '$want_log'"
  elif [ -e "$dir/lomhd.log" ]; then problems+=" lomhd.log exists (want none)"; fi
  [ -z "$want_out" ] || grep -q -- "$want_out" "$dir/stdout.txt" || problems+=" stdout lacks '$want_out'"
  [ -z "$want_in_txt" ] || cat "$dir"/lomhd_crash_*.txt 2>/dev/null | grep -q -- "$want_in_txt" || problems+=" report lacks '$want_in_txt'"

  if [ -z "$problems" ]; then
    echo "ok    $mode (exit $code, $have_txt txt, $have_dmp dmp, $have_hang hang)"
  else
    echo "FAIL  $mode:$problems"; failures=$((failures+1))
    sed 's/^/        stdout: /' "$dir/stdout.txt"; sed 's/^/        log: /' "$dir/lomhd.log" 2>/dev/null
  fi
done

echo "results in $WORK"
[ $failures -eq 0 ] && echo "all passed" || echo "$failures FAILED"
exit $((failures != 0))
