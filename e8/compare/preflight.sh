#!/bin/sh
# Machine-state preflight of a comparison run (plan.md Steps 8 and 9),
# called by run_compare.sh before every run directory. It checks the
# conditions Step 9 requires and prints one line per condition:
#
#   preflight_<name>: ok|FAIL|unavailable (detail)
#
#   boost      boost is off (cpufreq/boost, intel_pstate/no_turbo or
#              amd_pstate/cpb_boost)
#   governor   the scaling governor is "performance" on every pinned CPU
#   smt        SMT is off (smt/control off, forceoff or notsupported, and
#              smt/active 0)
#   cores      no two pinned CPUs are SMT siblings
#   perf       perf_event_paranoid <= 2 and the binary's core-cycle
#              counter works (its --build-info)
#   freq       the pinned CPUs' current frequencies, read while each runs
#              a busy loop, are within --freq-tolerance percent of each
#              other (default 1)
#   git        the tree is clean (git_dirty: no; tracked files only)
#
# The values come from env_capture.sh, run while the busy loops hold the
# pinned CPUs. Exit status: 0 if every condition is ok; 1 if any is FAIL or
# unavailable; 2 on a usage error. run_compare.sh refuses a non-smoke run
# on exit 1 unless --force is given, and records these lines either way.
#
# usage: preflight.sh --cpus LIST --binary BIN [--repo DIR]
#                     [--freq-tolerance PCT] [--capture FILE]
#
#   --capture FILE  check FILE (env_capture.sh output) instead of a fresh
#                   capture; for testing the checks on a machine that
#                   cannot show these values (WSL)

cpus=
binary=
repo=.
tol=1
capture=

while [ $# -gt 0 ]; do
	case "$1" in
	--cpus) cpus=$2; shift 2 ;;
	--binary) binary=$2; shift 2 ;;
	--repo) repo=$2; shift 2 ;;
	--freq-tolerance) tol=$2; shift 2 ;;
	--capture) capture=$2; shift 2 ;;
	*) echo "preflight.sh: unknown argument: $1" >&2; exit 2 ;;
	esac
done
if [ -z "$binary" ]; then
	echo "usage: preflight.sh --cpus LIST --binary BIN [--repo DIR]" \
		"[--freq-tolerance PCT]" >&2
	exit 2
fi
[ -n "$cpus" ] || cpus=none

here=$(dirname "$0")
tmp=$(mktemp) || exit 2
pids=

cleanup() {
	for p in $pids; do
		kill "$p" 2>/dev/null
	done
	rm -f "$tmp"
}
trap cleanup EXIT
trap 'exit 2' INT TERM

# Hold every pinned CPU busy so that its frequency is read under load.
if [ -n "$capture" ]; then
	cp "$capture" "$tmp" || exit 2
elif [ "$cpus" != none ] && command -v taskset > /dev/null 2>&1; then
	for c in $(echo "$cpus" | tr ',' ' '); do
		taskset -c "$c" sh -c 'while :; do :; done' &
		pids="$pids $!"
	done
	sleep 1
fi
[ -n "$capture" ] || sh "$here/env_capture.sh" --out "$tmp" --cpus "$cpus" \
	--binary "$binary" --repo "$repo"
for p in $pids; do
	kill "$p" 2>/dev/null
done
pids=

# get KEY: the value of KEY in the capture.
get() {
	sed -n "s/^$1: //p" "$tmp" | head -n 1
}

status=0

# report NAME STATUS DETAIL
report() {
	printf 'preflight_%s: %s (%s)\n' "$1" "$2" "$3"
	[ "$2" = ok ] || status=1
}

v=$(get boost)
case "$v" in
off) report boost ok "$v" ;;
unavailable) report boost unavailable "no boost control readable" ;;
*) report boost FAIL "$v" ;;
esac

# Per-CPU values "cpuN=value ...".
v=$(get governor)
if [ "$v" = unavailable ]; then
	report governor unavailable "no cpufreq governor readable"
else
	bad=$(echo "$v" | tr ' ' '\n' | grep -v '=performance$' | paste -s -d ' ' -)
	n=$(echo "$v" | wc -w)
	want=$(echo "$cpus" | tr ',' ' ' | wc -w)
	[ "$cpus" = none ] && want=1
	if [ -n "$bad" ]; then
		report governor FAIL "$bad"
	elif [ "$n" -ne "$want" ]; then
		report governor FAIL "read on $n of $want CPUs: $v"
	else
		report governor ok "performance on $n CPUs"
	fi
fi

c=$(get smt_control)
a=$(get smt_active)
case "$c" in
off|forceoff|notsupported)
	if [ "$a" = 1 ]; then
		report smt FAIL "control $c, active $a"
	else
		report smt ok "control $c, active $a"
	fi ;;
unavailable) report smt unavailable "no smt/control" ;;
*) report smt FAIL "control $c, active $a" ;;
esac

# cores: a pinned CPU whose sibling list names another pinned CPU.
v=$(get thread_siblings)
if [ "$cpus" = none ]; then
	report cores unavailable "no --cpus list"
elif [ "$v" = unavailable ]; then
	report cores unavailable "no thread_siblings_list"
else
	shared=
	for e in $v; do
		cpu=${e%%=*}
		cpu=${cpu#cpu}
		for s in $(echo "${e#*=}" | tr ',' ' '); do
			case "$s" in
			*-*) s=$(seq "${s%-*}" "${s#*-}") ;;
			esac
			for t in $s; do
				[ "$t" = "$cpu" ] && continue
				case ",$cpus," in
				*",$t,"*) shared="$shared $cpu+$t" ;;
				esac
			done
		done
	done
	if [ -n "$shared" ]; then
		report cores FAIL "SMT siblings pinned together:$shared"
	else
		report cores ok "every pinned CPU on its own core"
	fi
fi

p=$(get perf_event_paranoid)
cc=$(get core_cycles)
if [ "$p" = unavailable ] || [ "$cc" = unavailable ]; then
	report perf unavailable "perf_event_paranoid $p, core cycles $cc"
elif [ "$p" -le 2 ] && [ "$cc" = available ]; then
	report perf ok "perf_event_paranoid $p, core cycles $cc"
else
	report perf FAIL "perf_event_paranoid $p, core cycles $cc"
fi

v=$(get cur_freq_khz)
if [ "$v" = unavailable ]; then
	report freq unavailable "no scaling_cur_freq readable"
else
	r=$(echo "$v" | tr ' ' '\n' | sed 's/^[^=]*=//' | awk -v tol="$tol" '
		NR == 1 { lo = $1; hi = $1 }
		{ if ($1 < lo) lo = $1; if ($1 > hi) hi = $1 }
		END {
			sp = hi > 0 ? 100 * (hi - lo) / hi : 100
			printf "%s %d..%d kHz, spread %.2f%%\n",
				sp <= tol ? "ok" : "FAIL", lo, hi, sp
		}')
	report freq "${r%% *}" "${r#* } (tolerance $tol%): $v"
fi

v=$(get git_dirty)
case "$v" in
no) report git ok "clean at $(get git_commit | cut -c 1-10)" ;;
unavailable) report git unavailable "not a git tree" ;;
*) report git FAIL "dirty: $v" ;;
esac

exit "$status"
