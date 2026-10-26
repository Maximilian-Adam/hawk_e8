#!/bin/sh
# Record the measurement environment of a comparison run (plan.md Steps 7
# and 9) as "key: value" lines. Written beside every harness CSV by
# run_compare.sh; read by analysis/analyze.py (boost, run_id, flags).
#
# usage: env_capture.sh --out FILE [--run-id ID] [--cpus LIST]
#                       [--binary BIN] [--cc CC] [--repo DIR]
#
#   --binary BIN  a harness binary; its --build-info gives the flag set,
#                 the full flags, the compiler version it was built with
#                 and the core-cycle counter status
#   --cpus LIST   the pinned CPU list (comma-separated), or none
#
# Nothing here is fatal: a value that cannot be read (as on WSL, which has
# no cpufreq, boost or SMT control files) is written as "unavailable".

out=
run_id=unavailable
cpus=none
binary=
cc=${CC:-cc}
repo=.

while [ $# -gt 0 ]; do
	case "$1" in
	--out) out=$2; shift 2 ;;
	--run-id) run_id=$2; shift 2 ;;
	--cpus) cpus=$2; shift 2 ;;
	--binary) binary=$2; shift 2 ;;
	--cc) cc=$2; shift 2 ;;
	--repo) repo=$2; shift 2 ;;
	*) echo "env_capture.sh: unknown argument: $1" >&2; exit 2 ;;
	esac
done
if [ -z "$out" ]; then
	echo "usage: env_capture.sh --out FILE [--run-id ID] [--cpus LIST]" \
		"[--binary BIN] [--cc CC] [--repo DIR]" >&2
	exit 2
fi
[ -n "$cpus" ] || cpus=none

# val VALUE: VALUE, or "unavailable" if empty.
val() {
	if [ -n "$1" ]; then printf '%s' "$1"; else printf 'unavailable'; fi
}

# readf FILE: first line of FILE, or empty.
readf() {
	[ -r "$1" ] && head -n 1 "$1" 2>/dev/null | tr -d '\r'
}

# readf_none FILE: like readf, but "none" for a readable empty file
# (isolated, nohz_full: no CPU listed).
readf_none() {
	[ -r "$1" ] || return
	v=$(readf "$1")
	if [ -n "$v" ]; then printf '%s' "$v"; else printf 'none'; fi
}

# emit KEY VALUE
emit() {
	printf '%s: %s\n' "$1" "$(val "$2")"
}

# The CPUs to describe: the pinned list (ranges a-b expanded), or cpu0.
cpu_ids() {
	if [ "$cpus" = none ]; then
		echo 0
		return
	fi
	echo "$cpus" | tr ',' '\n' | while read -r c; do
		case "$c" in
		*-*) seq "${c%-*}" "${c#*-}" ;;
		*) echo "$c" ;;
		esac
	done
}

# per_cpu SUBPATH: "cpuN=value" for each described CPU, space-separated,
# or empty if none is readable.
per_cpu() {
	res=
	for c in $(cpu_ids); do
		v=$(readf "/sys/devices/system/cpu/cpu$c/$1")
		[ -n "$v" ] && res="$res${res:+ }cpu$c=$v"
	done
	printf '%s' "$res"
}

cpuinfo_field() {
	grep -m 1 "^$1[[:space:]]*:" /proc/cpuinfo 2>/dev/null \
		| sed 's/^[^:]*:[[:space:]]*//'
}

boost() {
	b=$(readf /sys/devices/system/cpu/cpufreq/boost)
	if [ -n "$b" ]; then
		[ "$b" = 0 ] && echo off || echo on
		return
	fi
	b=$(readf /sys/devices/system/cpu/intel_pstate/no_turbo)
	if [ -n "$b" ]; then
		[ "$b" = 1 ] && echo off || echo on
		return
	fi
	b=$(readf /sys/devices/system/cpu/amd_pstate/cpb_boost 2>/dev/null)
	if [ -n "$b" ]; then
		[ "$b" = 0 ] && echo off || echo on
	fi
}

wsl() {
	if grep -qi microsoft /proc/version 2>/dev/null; then
		echo yes
	else
		echo no
	fi
}

git_dirty() {
	command -v git > /dev/null 2>&1 || return
	n=$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null \
		| wc -l)
	git -C "$repo" rev-parse HEAD > /dev/null 2>&1 || return
	if [ "$n" -eq 0 ]; then echo no; else echo "yes ($n files)"; fi
}

build_info() {
	[ -n "$binary" ] && [ -x "$binary" ] || return
	"$binary" --build-info 2>/dev/null | grep -m 1 "^$1:" \
		| sed 's/^[^:]*:[[:space:]]*//'
}

march_native() {
	"$cc" -march=native -Q --help=target 2>/dev/null \
		| awk '$1 == "-march=" { print $2; exit }'
}

{
	emit run_id "$run_id"
	emit captured_utc "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)"
	emit hostname "$(uname -n 2>/dev/null)"
	emit kernel "$(uname -srvm 2>/dev/null)"
	emit wsl "$(wsl)"
	emit cpu_model "$(cpuinfo_field 'model name')"
	emit microcode "$(cpuinfo_field microcode)"
	emit cpus_online "$(readf /sys/devices/system/cpu/online)"
	emit cpus_pinned "$cpus"
	emit isolated_cpus "$(readf_none /sys/devices/system/cpu/isolated)"
	emit nohz_full "$(readf_none /sys/devices/system/cpu/nohz_full)"
	emit governor "$(per_cpu cpufreq/scaling_governor)"
	emit scaling_driver "$(per_cpu cpufreq/scaling_driver)"
	emit cur_freq_khz "$(per_cpu cpufreq/scaling_cur_freq)"
	emit boost "$(boost)"
	emit smt_control "$(readf /sys/devices/system/cpu/smt/control)"
	emit smt_active "$(readf /sys/devices/system/cpu/smt/active)"
	emit thread_siblings "$(per_cpu topology/thread_siblings_list)"
	emit l3_id "$(per_cpu cache/index3/id)"
	emit l3_size "$(per_cpu cache/index3/size)"
	emit perf_event_paranoid "$(readf /proc/sys/kernel/perf_event_paranoid)"
	emit core_cycles "$(build_info core_cycles)"
	emit flags "$(build_info flags)"
	emit cflags "$(build_info cflags)"
	emit compiler_built "$(build_info compiler)"
	emit cc "$cc"
	emit cc_version "$("$cc" --version 2>/dev/null | head -n 1)"
	emit march_native "$(march_native)"
	emit binary "$binary"
	emit git_commit "$(git -C "$repo" rev-parse HEAD 2>/dev/null)"
	emit git_dirty "$(git_dirty)"
	emit loadavg "$(readf /proc/loadavg)"
} > "$out"
exit 0
