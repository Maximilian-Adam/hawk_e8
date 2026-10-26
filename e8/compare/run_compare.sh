#!/bin/sh
# Comparison run driver (plan.md Steps 7-9), run from e8/ (make
# bench-compare, bench-compare-smoke). For each run r = 1..R and each flag
# set FS (runs interleaved: r1/FS1, r1/FS2, r2/FS1, ...), it writes one
# self-describing run directory
#
#   results/compare/FS/RUN_ID/
#     env.txt             env_capture.sh (kernel, CPU, flags, boost, ...)
#     commands.txt        every command of the run, its times and status
#     latency.csv/.txt    bin/bench_compare-FS --mode latency
#     throughput.csv/.txt bin/bench_compare-FS --mode throughput
#     latency_avx2.csv/.txt  bin/bench_compare_avx2-FS (HAWK AVX2 context)
#     sampler.csv/.txt    bin/e8_sampler_bench-FS --variant-matrix
#     stages_hawk.csv/.txt, stages_e8.csv/.txt
#                         the profile benches' stage totals (ref, T = 1)
#
# then runs the analysis over all of them into
# results/compare/analysis/NAME/.
#
# Run sets (--run-set; recorded as run_set in env.txt and commands.txt,
# and kept apart by the analysis):
#
#   all        every configuration (T 1, 2, 4, 8, 16) and every step; the
#              default, used by the smoke run; not a Step 9 run set
#   headline   Step 9's headline (Step 10 tables 1-5): T 1, 2, 4, 8 and
#              throughput with P = |--cpus| on the V-cache CCD's cores;
#              every step; --cpus required (except with --smoke)
#   cross-ccd  Step 9's secondary run: latency only, T 1, 2, 4, 8, 16 on
#              both CCDs' cores (V-cache CCD first); --cpus required
#              (except with --smoke)
#
# --threads and --procs override a run set's defaults. Before any step the
# driver refuses (exit 2) a --cpus list with fewer CPUs than the largest T
# or than P, with repeated CPUs, or not a comma-separated list.
#
# Preflight: before every run, compare/preflight.sh checks boost, governor,
# SMT, the pinned CPUs' cores, perf counting, the pinned CPUs' frequencies
# and a clean tree. A non-smoke run refuses to start (exit 3) unless every
# condition is ok or --force is given; the result, each condition and the
# override are recorded in commands.txt and env.txt. Smoke runs record the
# preflight and go on.
#
# Exit status: 0 if every step succeeded and the analysis's acceptance
# checks passed; 1 if a step failed or a check failed; 2 on a usage or
# configuration error; 3 if the preflight refused a run.
#
# usage: compare/run_compare.sh [--smoke] [--run-set all|headline|cross-ccd]
#          [--flagsets "O2 O3native"] [--runs R] [--keys K]
#          [--sigs-per-key M] [--procs P] [--cpus LIST] [--threads LIST]
#          [--logn LIST] [--warmup W] [--sampler-trials N]
#          [--stage-trials N] [--bootstrap B] [--name NAME] [--no-analysis]
#          [--force] [--freq-tolerance PCT]

smoke=0
run_set=all
force=0
freq_tol=1
flagsets=O2
runs=1
keys=
sigs=
procs=
cpus=
threads=
logn=
warmup=1
sampler_trials=
stage_trials=
bootstrap=
name=
analysis=1

while [ $# -gt 0 ]; do
	case "$1" in
	--smoke) smoke=1; shift ;;
	--run-set) run_set=$2; shift 2 ;;
	--force) force=1; shift ;;
	--freq-tolerance) freq_tol=$2; shift 2 ;;
	--flagsets) flagsets=$2; shift 2 ;;
	--runs) runs=$2; shift 2 ;;
	--keys) keys=$2; shift 2 ;;
	--sigs-per-key) sigs=$2; shift 2 ;;
	--procs) procs=$2; shift 2 ;;
	--cpus) cpus=$2; shift 2 ;;
	--threads) threads=$2; shift 2 ;;
	--logn) logn=$2; shift 2 ;;
	--warmup) warmup=$2; shift 2 ;;
	--sampler-trials) sampler_trials=$2; shift 2 ;;
	--stage-trials) stage_trials=$2; shift 2 ;;
	--bootstrap) bootstrap=$2; shift 2 ;;
	--name) name=$2; shift 2 ;;
	--no-analysis) analysis=0; shift ;;
	*) echo "run_compare.sh: unknown argument: $1" >&2; exit 2 ;;
	esac
done

die() {
	echo "run_compare.sh: $*" >&2
	exit 2
}

# count LIST: the number of entries of a comma-separated list.
count() {
	echo "$1" | tr ',' '\n' | grep -c .
}

# Comma-separated lists of unsigned integers only (bench_compare's form).
for v in "cpus:$cpus" "threads:$threads"; do
	case "${v#*:}" in
	*[!0-9,]*|,*|*,|*,,*) die "--${v%%:*} must be a comma-separated list" \
		"of CPU / thread numbers: ${v#*:}" ;;
	esac
done

# Run sets: steps and defaults.
all_steps="latency throughput latency_avx2 sampler stages_hawk stages_e8"
case "$run_set" in
all) steps=$all_steps; check_threads=${threads:-1,2,4,8,16} ;;
headline)
	steps=$all_steps
	: "${threads:=1,2,4,8}"
	[ -n "$cpus" ] && : "${procs:=$(count "$cpus")}" ;;
cross-ccd)
	steps=latency
	: "${threads:=1,2,4,8,16}" ;;
*) die "unknown --run-set $run_set (all, headline or cross-ccd)" ;;
esac
: "${check_threads:=$threads}"
if [ "$run_set" != all ] && [ -z "$cpus" ] && [ "$smoke" != 1 ]; then
	die "--run-set $run_set needs --cpus (the pinned CPU list)"
fi

# Defaults: the smoke run (Step 8: every configuration, small counts) and
# the full run (Step 9's volume per run: 10 keys x 1,000 signatures).
if [ "$smoke" = 1 ]; then
	: "${keys:=2}" "${sigs:=4}" "${procs:=4}"
	: "${sampler_trials:=50}" "${stage_trials:=20}" "${bootstrap:=200}"
else
	: "${keys:=10}" "${sigs:=1000}" "${procs:=4}"
	: "${sampler_trials:=2000}" "${stage_trials:=1000}"
	: "${bootstrap:=2000}"
fi

# has_step NAME: NAME is a step of the run set.
has_step() {
	case " $steps " in
	*" $1 "*) return 0 ;;
	esac
	return 1
}

# The pinned list must hold the largest T (latency) and P (throughput),
# each on its own CPU.
if [ -n "$cpus" ]; then
	ncpu=$(count "$cpus")
	if [ "$(echo "$cpus" | tr ',' '\n' | sort -u | grep -c .)" -ne "$ncpu" ]
	then
		die "--cpus $cpus lists a CPU more than once"
	fi
	maxt=$(echo "$check_threads" | tr ',' '\n' | sort -n | tail -n 1)
	if has_step latency && [ "$maxt" -gt "$ncpu" ]; then
		die "--cpus lists $ncpu CPUs, but the thread list" \
			"$check_threads needs $maxt (one CPU per thread);" \
			"pass --threads or a longer --cpus list"
	fi
	if has_step throughput && [ "$procs" -gt "$ncpu" ]; then
		die "--cpus lists $ncpu CPUs, but --procs is $procs" \
			"(one CPU per process)"
	fi
fi

status=0
dirs=
failed=

# The driver's own stderr, for messages from inside run_step (whose
# callers redirect the step's stderr to STEP.txt).
exec 3>&2

# run_step DIR LABEL CMD...: run CMD, logging it to DIR/commands.txt.
run_step() {
	dir=$1
	label=$2
	shift 2
	t0=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	s0=$(date +%s)
	printf '%s: %s\n' "$label" "$*" >> "$dir/commands.txt"
	"$@"
	rc=$?
	# A shell reports a signal as 128 + its number.
	[ "$rc" -gt 128 ] && rc_note=" (signal $((rc - 128)))" || rc_note=""
	printf '  start %s, %s s, exit %s\n' "$t0" "$(( $(date +%s) - s0 ))" \
		"$rc" >> "$dir/commands.txt"
	if [ "$rc" -ne 0 ]; then
		echo "run_compare.sh: $label failed (exit $rc$rc_note), see" \
			"$dir/commands.txt and $dir/$label.txt" >&3
		failed="$failed $dir:$label"
		status=1
	fi
	return "$rc"
}

# taskset prefix for the sampler and stage benches when pinning.
pin() {
	if [ -n "$cpus" ] && command -v taskset > /dev/null 2>&1; then
		echo taskset -c "$1"
	fi
}

first_cpu=${cpus%%,*}
first_cpu=${first_cpu%%-*}

r=1
while [ "$r" -le "$runs" ]; do
	for fs in $flagsets; do
		for b in bench_compare bench_compare_avx2 e8_sampler_bench \
			e8_sign_profile_bench hawk_sign_profile_bench
		do
			if [ ! -x "bin/$b-$fs" ]; then
				echo "run_compare.sh: bin/$b-$fs missing" \
					"(make bench-builds)" >&2
				exit 2
			fi
		done
		# Preflight before the run directory exists, so that a
		# refused run leaves nothing behind.
		pre=$(sh compare/preflight.sh --cpus "${cpus:-none}" \
			--binary "bin/bench_compare-$fs" --repo .. \
			--freq-tolerance "$freq_tol")
		pre_rc=$?
		[ "$pre_rc" -le 1 ] || die "preflight.sh failed (exit $pre_rc)"
		if [ "$pre_rc" -eq 0 ]; then
			pre_result=passed
		elif [ "$smoke" = 1 ]; then
			pre_result="failed (smoke run, not enforced)"
		elif [ "$force" = 1 ]; then
			pre_result="failed (forced: --force)"
		else
			echo "$pre" >&2
			echo "run_compare.sh: preflight refused run $r/$runs" \
				"(flags $fs): every condition above must be ok" \
				"for a non-smoke run (e8/docs/step9_runbook.md);" \
				"--force overrides and is recorded" >&2
			exit 3
		fi
		[ "$pre_rc" -eq 0 ] || echo "$pre" | grep -v ': ok (' | \
			sed "s/^/run_compare.sh: /" >&2

		run_id="$(date -u +%Y%m%dT%H%M%SZ)-$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')"
		dir="results/compare/$fs/$run_id"
		mkdir -p "$dir"
		dirs="$dirs $dir"
		if [ -z "$name" ]; then
			[ "$run_set" = all ] && name=$run_id \
				|| name="$run_set-$run_id"
		fi
		echo "run_compare.sh: run $r/$runs flags=$fs run_set=$run_set" \
			"-> $dir" >&2

		common="--run-id $run_id --keys $keys --sigs-per-key $sigs --warmup $warmup"
		[ -n "$logn" ] && common="$common --logn $logn"
		[ -n "$cpus" ] && common="$common --cpus $cpus"
		lat_threads=
		[ -n "$threads" ] && lat_threads="--threads $threads"

		# run_set and the preflight go into env.txt and commands.txt.
		record() {
			echo "run_set: $run_set"
			echo "preflight: $pre_result"
			echo "$pre"
		}
		compare/env_capture.sh --out "$dir/env.txt" --run-id "$run_id" \
			--cpus "${cpus:-none}" --binary "bin/bench_compare-$fs" \
			--cc "${CC:-c99}" --repo ..
		record >> "$dir/env.txt"
		{
			echo "run_id: $run_id"
			echo "flags: $fs"
			echo "keys: $keys"
			echo "sigs_per_key: $sigs"
			echo "procs: $procs"
			echo "cpus: ${cpus:-none}"
			echo "threads: ${threads:-default}"
			echo "smoke: $smoke"
			echo "steps: $steps"
			record
		} > "$dir/commands.txt"

		# shellcheck disable=SC2086
		has_step latency && run_step "$dir" latency \
			"bin/bench_compare-$fs" --mode latency \
			$common $lat_threads --out "$dir/latency.csv" \
			2> "$dir/latency.txt"
		# shellcheck disable=SC2086
		has_step throughput && run_step "$dir" throughput \
			"bin/bench_compare-$fs" \
			--mode throughput --procs "$procs" $common \
			--out "$dir/throughput.csv" 2> "$dir/throughput.txt"
		# shellcheck disable=SC2086
		has_step latency_avx2 && run_step "$dir" latency_avx2 \
			"bin/bench_compare_avx2-$fs" \
			--mode latency --threads 1 $common \
			--out "$dir/latency_avx2.csv" 2> "$dir/latency_avx2.txt"
		# Table 3 uses T = 1 only; the matrix's default thread counts
		# (1, 8, 16) would oversubscribe a short --cpus list.
		# shellcheck disable=SC2086
		has_step sampler && run_step "$dir" sampler \
			$(pin "$first_cpu") "bin/e8_sampler_bench-$fs" \
			--variant-matrix --threads 1 --trials "$sampler_trials" \
			> "$dir/sampler.csv" 2> "$dir/sampler.txt"
		# shellcheck disable=SC2086
		has_step stages_hawk && run_step "$dir" stages_hawk \
			$(pin "$first_cpu") \
			"bin/hawk_sign_profile_bench-$fs" --trials "$stage_trials" \
			--stage-csv "$dir/stages_hawk.csv" \
			> /dev/null 2> "$dir/stages_hawk.txt"
		# shellcheck disable=SC2086
		has_step stages_e8 && run_step "$dir" stages_e8 \
			$(pin "$first_cpu") \
			"bin/e8_sign_profile_bench-$fs" --variant ref --threads 1 \
			--trials "$stage_trials" --stage-csv "$dir/stages_e8.csv" \
			> /dev/null 2> "$dir/stages_e8.txt"
	done
	r=$((r + 1))
done

if [ "$analysis" = 1 ]; then
	out="results/compare/analysis/$name"
	echo "run_compare.sh: analysis -> $out" >&2
	# shellcheck disable=SC2086
	python3 -I compare/analysis/analyze.py --out-dir "$out" \
		--bootstrap "$bootstrap" $dirs || status=1
fi
if [ -n "$failed" ]; then
	echo "run_compare.sh: failed steps:$failed" >&2
fi
exit "$status"
