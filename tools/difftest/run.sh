#!/bin/sh
# Usage: tools/difftest/run.sh <addr|all|selftest> [--trials N] [--seed S]
# Runs reimpl-vs-original, original-vs-itself (control 1) and a -DMUTANT reimpl (control 2, must be
# caught). Exit 0 only if reimpl and self show 0 divergences and the mutant diverges.
set -e
here=$(cd "$(dirname "$0")" && pwd)
[ -x "$here/.venv/bin/python" ] || python3 -m venv "$here/.venv"
py="$here/.venv/bin/python"
"$py" -c "import unicorn, capstone" 2>/dev/null || "$py" -m pip install -q -r "$here/requirements.txt"
if [ "$1" = all ]; then
    shift
    "$0" selftest || exit 1
    rc=0
    for a in 0x404ab0 0x4026a0 0x4ac0b0; do "$0" $a "$@" || rc=1; done
    exit $rc
fi
addr=$1
shift
rc=0
[ "$addr" = selftest ] || {
$py "$here/difftest.py" "$addr" --mode reimpl "$@" || rc=1
$py "$here/difftest.py" "$addr" --mode self "$@" || rc=1
$py "$here/difftest.py" "$addr" --mode reimpl --i486 "$@" || rc=1
}
# A control is caught only when the harness finished and reported divergent > 0; a build error or
# crash prints no summary line and fails the control.
control() {
    mode=$1
    shift
    out=$($py "$here/difftest.py" "$addr" --mode "$mode" "$@" 2>&1) || true
    echo "$out"
    case "$out" in *"[$mode]: n/a"*) return 0 ;; esac
    d=$(printf '%s\n' "$out" | sed -n 's/.*trials=[0-9]* divergent=\([0-9]*\).*/\1/p' | tail -1)
    if [ -n "$d" ] && [ "$d" -gt 0 ]; then
        echo "  control ok: $mode caught"
    else
        echo "  CONTROL FAILED: $mode not caught (divergent=${d:-none})"
        rc=1
    fi
}
if [ "$addr" = selftest ]; then
    addr=0x404ab0
    control no_such_mode >/dev/null
    [ $rc -eq 1 ] || { echo "selftest FAILED: a harness error counted as a caught mutant"; exit 1; }
    rc=0
    control mutant >/dev/null
    [ $rc -eq 0 ] || { echo "selftest FAILED: a real mutant was not caught"; exit 1; }
    echo "selftest ok"
    exit 0
fi
control mutant "$@"
control mutant_cache "$@"
exit $rc
