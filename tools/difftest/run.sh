#!/bin/sh
# Usage: tools/difftest/run.sh <addr|all> [--trials N] [--seed S]
# Runs reimpl-vs-original, original-vs-itself (control 1) and a -DMUTANT reimpl (control 2, must be
# caught). Exit 0 only if reimpl and self show 0 divergences and the mutant diverges.
set -e
here=$(cd "$(dirname "$0")" && pwd)
[ -x "$here/.venv/bin/python" ] || python3 -m venv "$here/.venv"
py="$here/.venv/bin/python"
"$py" -c "import unicorn, capstone" 2>/dev/null || "$py" -m pip install -q -r "$here/requirements.txt"
if [ "$1" = all ]; then
    shift
    rc=0
    for a in 0x404ab0 0x4026a0 0x4ac0b0; do "$0" $a "$@" || rc=1; done
    exit $rc
fi
addr=$1
shift
rc=0
$py "$here/difftest.py" "$addr" --mode reimpl "$@" || rc=1
$py "$here/difftest.py" "$addr" --mode self "$@" || rc=1
$py "$here/difftest.py" "$addr" --mode reimpl --i486 "$@" || rc=1
if $py "$here/difftest.py" "$addr" --mode mutant "$@"; then
    echo "  CONTROL FAILED: mutant was not caught"
    rc=1
else
    echo "  control ok: mutant caught"
fi
out=$($py "$here/difftest.py" "$addr" --mode mutant_cache "$@") && st=0 || st=$?
echo "$out"
case "$out" in
*n/a*) ;;
*) if [ $st -eq 0 ]; then echo "  CONTROL FAILED: cache mutant was not caught"; rc=1; else echo "  control ok: cache mutant caught"; fi ;;
esac
exit $rc
