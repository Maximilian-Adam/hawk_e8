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
# results/compare/analysis/NAME/. Exit status: 0 if every step succeeded
# and the analysis's acceptance checks passed.
#
# usage: compare/run_compare.sh [--smoke] [--flagsets "O2 O3native"]
#          [--runs R] [--keys K] [--sigs-per-key M] [--procs P]
#          [--cpus LIST] [--threads LIST] [--logn LIST] [--warmup W]
#          [--sampler-trials N] [--stage-trials N] [--bootstrap B]
#          [--name NAME] [--no-analysis]

smoke=0
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

status=0
dirs=

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
	printf '  start %s, %s s, exit %s\n' "$t0" "$(( $(date +%s) - s0 ))" \
		"$rc" >> "$dir/commands.txt"
	if [ "$rc" -ne 0 ]; then
		echo "run_compare.sh: $label failed (exit $rc), see $dir" >&2
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
		run_id="$(date -u +%Y%m%dT%H%M%SZ)-$(od -An -N4 -tx4 /dev/urandom | tr -d ' \n')"
		dir="results/compare/$fs/$run_id"
		mkdir -p "$dir"
		dirs="$dirs $dir"
		[ -n "$name" ] || name="$run_id"
		echo "run_compare.sh: run $r/$runs flags=$fs -> $dir" >&2

		common="--run-id $run_id --keys $keys --sigs-per-key $sigs --warmup $warmup"
		[ -n "$logn" ] && common="$common --logn $logn"
		[ -n "$cpus" ] && common="$common --cpus $cpus"
		lat_threads=
		[ -n "$threads" ] && lat_threads="--threads $threads"

		compare/env_capture.sh --out "$dir/env.txt" --run-id "$run_id" \
			--cpus "${cpus:-none}" --binary "bin/bench_compare-$fs" \
			--cc "${CC:-c99}" --repo ..
		{
			echo "run_id: $run_id"
			echo "flags: $fs"
			echo "keys: $keys"
			echo "sigs_per_key: $sigs"
			echo "procs: $procs"
			echo "cpus: ${cpus:-none}"
			echo "smoke: $smoke"
		} > "$dir/commands.txt"

		# shellcheck disable=SC2086
		run_step "$dir" latency "bin/bench_compare-$fs" --mode latency \
			$common $lat_threads --out "$dir/latency.csv" \
			2> "$dir/latency.txt"
		# shellcheck disable=SC2086
		run_step "$dir" throughput "bin/bench_compare-$fs" \
			--mode throughput --procs "$procs" $common \
			--out "$dir/throughput.csv" 2> "$dir/throughput.txt"
		# shellcheck disable=SC2086
		run_step "$dir" latency_avx2 "bin/bench_compare_avx2-$fs" \
			--mode latency --threads 1 $common \
			--out "$dir/latency_avx2.csv" 2> "$dir/latency_avx2.txt"
		# shellcheck disable=SC2086
		run_step "$dir" sampler $(pin "$cpus") "bin/e8_sampler_bench-$fs" \
			--variant-matrix --trials "$sampler_trials" \
			> "$dir/sampler.csv" 2> "$dir/sampler.txt"
		# shellcheck disable=SC2086
		run_step "$dir" stages_hawk $(pin "$first_cpu") \
			"bin/hawk_sign_profile_bench-$fs" --trials "$stage_trials" \
			--stage-csv "$dir/stages_hawk.csv" \
			> /dev/null 2> "$dir/stages_hawk.txt"
		# shellcheck disable=SC2086
		run_step "$dir" stages_e8 $(pin "$first_cpu") \
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
exit "$status"
