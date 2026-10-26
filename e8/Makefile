# This Makefile builds the experimental HAWK-E8-CM code, its tests, tools and
# comparison benchmarks against the generated, unmodified HAWK reference
# sources in ../Reference_Implementation (produced from ../src by ../build.py).
# The shared measurement modules in common/ (RNG, worker pool, timing, CSV)
# are used by src/, tests/, tools/ and compare/; they depend only on
# upstream HAWK's sha3.h and hawk.h.
.POSIX:

CC = c99
CFLAGS = -Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always
LIBS =

HAWK_DIR = ../Reference_Implementation
HAWK_STAMP = build/hawk_sources.stamp

HAWK_OBJ = build/hawk/hawk_kgen.o build/hawk/hawk_sign.o build/hawk/hawk_vrfy.o build/hawk/ng_fxp.o build/hawk/ng_hawk.o build/hawk/ng_mp31.o build/hawk/ng_ntru.o build/hawk/ng_poly.o build/hawk/ng_zint31.o build/hawk/sha3.o
HAWK_OBJ_NOSIGN = $(filter-out build/hawk/hawk_sign.o,$(HAWK_OBJ))
HAWK_SHIM_OBJ = build/hawk_shim.o
CMN_OBJ = build/common/cmn_timing.o build/common/cmn_rng.o build/common/cmn_pool.o build/common/cmn_csv.o
E8_OBJ = build/e8_math.o build/e8_f2.o build/e8_ntt.o build/e8_codec.o build/e8_vrfy.o build/e8_sign.o build/e8_sign_legacy.o
# The sampler and the shared modules it (and e8_sign.o) uses:
# e8_sampler.o (shared, integer-only), the variants on the generated tables
# (integer-only) and the runtime table builder (floating point, used only by
# the double-sigma entry points of tests and tools).
E8_SAMPLER_VARIANT_OBJ = build/e8_sampler_ref.o build/e8_sampler_hot64.o build/e8_sampler_hot32.o build/e8_sampler_variants.o
E8_SAMPLER_OBJ = build/e8_sampler.o $(E8_SAMPLER_VARIANT_OBJ) build/e8_sampler_runtime.o $(CMN_OBJ)
E8_SAMPLER_PROFILE_OBJ = build/profile/e8_sampler.o build/profile/e8_sampler_ref.o build/profile/e8_sampler_hot64.o build/profile/e8_sampler_hot32.o build/e8_sampler_variants.o build/e8_sampler_runtime.o $(CMN_OBJ)
E8_API_OBJ = build/e8_api.o
E8_PROFILE_OBJ = build/e8_math.o build/e8_f2.o build/e8_ntt.o build/e8_codec.o build/e8_vrfy.o build/profile/e8_sign.o build/e8_sign_legacy.o build/profile/e8_api.o

E8_TEST_PROGS = bin/test_e8_math bin/test_e8_arith bin/test_e8_public bin/test_e8_verify bin/test_e8_sign bin/test_e8_sampler bin/test_e8_sign_sampler bin/test_e8_api bin/test_e8_tables bin/test_e8_variants bin/test_e8_ref_ct bin/test_hawk_variants bin/test_cmn_common
E8_TOOL_PROGS = bin/e8_histograms bin/e8_rejection_summary bin/e8_validation_summary bin/e8_correctness bin/e8_correctness_nodebug bin/e8_distribution_sample
E8_BENCH_PROGS = bin/e8_sampler_bench bin/e8_sampler_profile_bench bin/e8_sign_bench bin/e8_sign_profile_bench bin/hawk_sign_profile_bench
# The signing core shared by e8_sign_bench and bench_compare.
BSC_HEAD = compare/bench_sign_core.h compare/hawk_shim.h
BSC_OBJ = build/bench_sign_core.o
HAWK_PROFILE_SIGN_SOURCE = build/hawk_sign_profile.c
# Benchmark-only HAWK variants (compare/hawk_variants/): the variant signer
# (a patched copy of upstream hawk_sign.c) and its samplers.
HAWKV_DIR = compare/hawk_variants
HAWKV_SIGN_SOURCE = build/hawk_sign_variant.c
HAWKV_HEAD = $(HAWKV_DIR)/hawk_variants.h compare/hawk_shim.h $(CMN_HEAD) $(HAWK_TABLES)
HAWKV_INC = -I$(HAWK_DIR) -Icommon -Itables -Icompare -I$(HAWKV_DIR)
HAWKV_OBJ = build/hawkv/hawk_sign_variant.o build/hawkv/hawk_variants.o build/hawkv/hawk_sampler_ref.o build/hawkv/hawk_sampler_hot64.o build/hawkv/hawk_sampler_hot32.o

HAWK_HEAD = $(HAWK_STAMP)
CMN_HEAD = common/cmn_timing.h common/cmn_rng.h common/cmn_pool.h common/cmn_csv.h $(HAWK_HEAD)
E8_HEAD = src/hawk_e8_inner.h src/hawk_e8.h src/e8_sampler_inner.h $(CMN_HEAD)
E8_TABLES = tables/e8_tables_geom.h tables/e8_tables_hot64.h tables/e8_tables_hot32.h tables/e8_tables_ref.h
HAWK_TABLES = tables/hawk_tables_ref.h tables/hawk_tables_hot64.h tables/hawk_tables_hot32.h
E8_INC = -I$(HAWK_DIR) -Icommon -Itables
CMN_CFLAGS = $(CFLAGS) -I$(HAWK_DIR)
E8_CFLAGS = $(CFLAGS) -DHAWK_ENABLE_E8_EXPERIMENTAL=1
# In-path instrumentation is off by default (HAWK_E8_PROFILE_SIGN=0 and
# HAWK_E8_PROFILE_SAMPLER=0); the profiling builds below enable it
# explicitly and are never used for headline timings.
E8_PROFILE_SIGN_CFLAGS = $(E8_CFLAGS) -DHAWK_E8_PROFILE_SIGN=1 -DHAWK_E8_PROFILE_SAMPLER=0
E8_PROFILE_SAMPLER_CFLAGS = $(E8_CFLAGS) -DHAWK_E8_PROFILE_SAMPLER=1
HAWK_PROFILE_SIGN_CFLAGS = $(CFLAGS) -DHAWK_AVX2=0 -DHAWK_PROFILE_SIGN=1
E8_THREAD_CFLAGS ?= -pthread
E8_THREAD_LIBS ?= -pthread
# The E8 objects without the debug checks (bin/e8_correctness_nodebug,
# Step 6 validation); see the rules below.
E8_NODEBUG_CFLAGS = $(E8_CFLAGS) -DHAWK_E8_DEBUG_CHECKS=0
E8_NODEBUG_OBJ = build/nodebug/e8_math.o build/nodebug/e8_f2.o build/nodebug/e8_ntt.o build/nodebug/e8_codec.o build/nodebug/e8_vrfy.o build/nodebug/e8_sign.o build/nodebug/e8_sign_legacy.o build/nodebug/e8_api.o build/nodebug/e8_sampler.o build/nodebug/e8_sampler_ref.o build/nodebug/e8_sampler_hot64.o build/nodebug/e8_sampler_hot32.o build/nodebug/e8_sampler_variants.o build/nodebug/e8_sampler_runtime.o

all: build bin $(E8_TEST_PROGS) $(E8_TOOL_PROGS) $(E8_BENCH_PROGS) bench-builds
test-e8: build bin $(E8_TEST_PROGS)
	./bin/test_e8_math
	./bin/test_e8_arith
	./bin/test_e8_public
	./bin/test_e8_verify
	./bin/test_e8_sign
	./bin/test_e8_sampler
	./bin/test_e8_sign_sampler
	./bin/test_e8_api
	./bin/test_e8_tables
	./bin/test_e8_variants
	./bin/test_e8_ref_ct
	./bin/test_hawk_variants
	./bin/test_cmn_common
# Generated outputs live under results/ (gitignored), one directory per
# kind: results/bench/ (benchmarks), results/diagnostics/ (diagnostic
# tools), results/precision/ (precision audit), results/validation/ and
# results/validation-quick/ (Step 6), results/compare/ (the comparison
# harness, below). Committed reference copies live in docs/.
BENCH_OUT = results/bench
DIAG_OUT = results/diagnostics
e8-histograms: build bin bin/e8_histograms
	@mkdir -p $(DIAG_OUT)
	cd $(DIAG_OUT) && ../../bin/e8_histograms
sampler-bench: build bin bin/e8_sampler_bench
	@mkdir -p $(BENCH_OUT)
	@./bin/e8_sampler_bench --selected-configs > $(BENCH_OUT)/e8_sampler_bench.csv
	@printf 'wrote $(BENCH_OUT)/e8_sampler_bench.csv\n'
variant-sampler-bench: build bin bin/e8_sampler_bench
	@mkdir -p $(BENCH_OUT)
	@./bin/e8_sampler_bench --variant-matrix --trials 2000 > $(BENCH_OUT)/e8_variant_sampler_bench.csv
	@printf 'wrote $(BENCH_OUT)/e8_variant_sampler_bench.csv\n'
profile-sampler-bench: build bin bin/e8_sampler_profile_bench
	@mkdir -p $(BENCH_OUT)
	@./bin/e8_sampler_profile_bench --selected-configs > $(BENCH_OUT)/e8_sampler_profile_bench.csv
	@printf 'wrote $(BENCH_OUT)/e8_sampler_profile_bench.csv\n'
sign-bench: build bin bin/e8_sign_bench
	@mkdir -p $(BENCH_OUT)
	@./bin/e8_sign_bench > $(BENCH_OUT)/e8_sign_bench.csv
	@printf 'wrote $(BENCH_OUT)/e8_sign_bench.csv\n'
profile-sign-bench: build bin bin/e8_sign_profile_bench
	@./bin/e8_sign_profile_bench > /dev/null
profile-hawk-sign-bench: build bin bin/hawk_sign_profile_bench
	@./bin/hawk_sign_profile_bench > /dev/null
e8-rejection-summary: build bin bin/e8_rejection_summary
	@mkdir -p $(DIAG_OUT)
	@./bin/e8_rejection_summary > $(DIAG_OUT)/e8_rejection_summary.csv
	@printf 'wrote $(DIAG_OUT)/e8_rejection_summary.csv\n'
e8-validation-summary: build bin bin/e8_validation_summary
	@mkdir -p $(DIAG_OUT)
	@cd $(DIAG_OUT) && ../../bin/e8_validation_summary
	@printf 'wrote e8_validation_summary.csv e8_validation_shell.csv e8_validation_coset.csv e8_validation_global.csv e8_validation_stability.csv in $(DIAG_OUT)\n'
e8-sigma-verify-sweep: build bin bin/e8_rejection_summary
	@mkdir -p $(DIAG_OUT)
	@E8_SIGMA_VERIFY_SWEEP=1 ./bin/e8_rejection_summary > $(DIAG_OUT)/e8_sigma_verify_sweep.csv
	@printf 'wrote $(DIAG_OUT)/e8_sigma_verify_sweep.csv\n'
e8-sigma-sign-sweep: build bin bin/e8_rejection_summary
	@mkdir -p $(DIAG_OUT)
	@E8_SIGMA_SIGN_SWEEP=1 ./bin/e8_rejection_summary > $(DIAG_OUT)/e8_sigma_sign_sweep.csv
	@printf 'wrote $(DIAG_OUT)/e8_sigma_sign_sweep.csv\n'

# Step 6 validation (tools/validate.py, see docs/validation.md): signature
# correctness of every variant of both schemes (no-debug build), the E8
# debug-build run, sampler histograms and the distribution tests. Outputs
# in results/validation-quick/ and results/validation/. validate fails if
# the fresh results/validation/summary.csv differs from the committed
# docs/validation_summary.csv (to update it, copy it there).
VALIDATE_BINS = bin/e8_correctness bin/e8_correctness_nodebug bin/e8_distribution_sample
validate-quick: build bin $(VALIDATE_BINS)
	python3 -I tools/validate.py --quick --out-dir results/validation-quick
validate: build bin $(VALIDATE_BINS)
	python3 -I tools/validate.py --out-dir results/validation --check docs/validation_summary.csv

# Comparison harness (plan.md Steps 7-9; compare/run_compare.sh, docs/
# harness.md): bench_compare in latency and throughput mode, the HAWK AVX2
# context, the sampler-only and stage benches, each run in its own
# results/compare/<flag set>/<run_id>/ with env.txt, then the analysis
# (compare/analysis/analyze.py) into results/compare/analysis/<name>/.
# bench-compare: Step 9's volume per run (10 keys x 1,000 signatures);
# COMPARE_FLAGSETS, COMPARE_RUNS and COMPARE_ARGS select more, for example
# Step 9's run sets (docs/step9_runbook.md):
#   COMPARE_ARGS="--run-set headline --cpus $(sh e8/compare/ccd_cpus.sh headline)"
#   COMPARE_ARGS="--run-set cross-ccd --cpus $(sh e8/compare/ccd_cpus.sh cross-ccd)"
# A non-smoke run first passes compare/preflight.sh (or --force).
# bench-compare-smoke (Step 8): every configuration of both flag sets at
# small counts, the analysis self-test and the analysis.
COMPARE_FLAGSETS = O2
COMPARE_RUNS = 1
COMPARE_ARGS =
bench-compare: build bin bench-builds
	sh compare/run_compare.sh --flagsets "$(COMPARE_FLAGSETS)" --runs $(COMPARE_RUNS) $(COMPARE_ARGS)
bench-compare-smoke: build bin bench-builds
	python3 -I compare/analysis/analyze.py --self-test
	sh compare/run_compare.sh --smoke --flagsets "$(BENCH_FLAGSETS)" $(COMPARE_ARGS)

# Regenerate the sampler tables into build/tables and fail if any differs
# from the committed tables/ (to update them, run the generator command in
# their header comment).
tables: build
	-mkdir -p build/tables
	python3 -I tables/gen_tables.py --out build/tables --hawk-src ../src/hawk_sign.c
	@for f in $(E8_TABLES) $(HAWK_TABLES); do \
		cmp -s $$f build/tables/$${f#tables/} \
			|| { echo "tables: $$f differs from the generator output"; exit 1; }; \
	done; echo "tables: committed tables match the generator"

# Precision audit (tools/precision_audit.py, see docs/precision_audit.md):
# self-tests, including the exact 1D moments against the C moments of
# test_e8_tables and test_hawk_variants, then the audit of every variant's
# committed tables (precision_audit.csv, precision_audit_dists.csv,
# precision_summary.csv in results/precision/); fails if the committed
# docs/precision_summary.csv differs from the fresh summary (to update it,
# copy results/precision/precision_summary.csv to docs/).
PRECISION_OUT = results/precision
precision-audit: build bin bin/test_e8_tables bin/test_hawk_variants
	@./bin/test_e8_tables --moments > build/precision_moments.csv
	@./bin/test_hawk_variants --moments >> build/precision_moments.csv
	python3 -I tools/precision_audit.py --self-test --c-moments build/precision_moments.csv --out-dir $(PRECISION_OUT)
	@cmp -s $(PRECISION_OUT)/precision_summary.csv docs/precision_summary.csv \
		|| { echo "precision-audit: docs/precision_summary.csv differs from the fresh $(PRECISION_OUT)/precision_summary.csv"; exit 1; }; \
		echo "precision-audit: committed docs/precision_summary.csv matches"

# Integer-only signing path. Every E8 and HAWK-variant translation unit on
# the signing path (and the shared modules it uses) must compile with
# -mgeneral-regs-only,
# under which GCC rejects any floating-point use; the timed objects are
# compiled with the ordinary flags, identical to HAWK's, and their
# disassembly (with the upstream objects the E8 signer calls) must contain
# no SSE/AVX or x87 floating-point instruction.
INT_ONLY_SRC = src/e8_sign.c src/e8_api.c src/e8_codec.c src/e8_f2.c src/e8_ntt.c src/e8_math.c src/e8_sampler.c src/e8_sampler_ref.c src/e8_sampler_variants.c
INT_ONLY_VARIANTS = src/e8_sampler_hot.c:-DE8_HOT_BITS=64 src/e8_sampler_hot.c:-DE8_HOT_BITS=32
INT_ONLY_CMN_SRC = common/cmn_rng.c common/cmn_pool.c common/cmn_timing.c
INT_ONLY_HAWKV_SRC = $(HAWKV_SIGN_SOURCE):-DHAWK_AVX2=0 $(HAWKV_DIR)/hawk_variants.c: $(HAWKV_DIR)/hawk_sampler_ref.c: $(HAWKV_DIR)/hawk_sampler_hot.c:-DHAWK_HOT_BITS=64 $(HAWKV_DIR)/hawk_sampler_hot.c:-DHAWK_HOT_BITS=32
INT_ONLY_OBJ = $(HAWKV_OBJ) build/e8_sign.o build/e8_api.o build/e8_codec.o build/e8_f2.o build/e8_ntt.o build/e8_math.o build/e8_sampler.o $(E8_SAMPLER_VARIANT_OBJ) build/common/cmn_rng.o build/common/cmn_pool.o build/common/cmn_timing.o build/hawk/sha3.o build/hawk/ng_hawk.o
FP_INSN = (add|sub|mul|div|sqrt|min|max|rcp|rsqrt|hadd|hsub|addsub|round|dp)[sp][sd]\b|cvt|ucomis|comis|fmadd|fmsub|fnmadd|fnmsub|\bf(ld|st|ild|ist|add|sub|mul|div|cos|sin|sqrt|xch|com|ucom|chs|abs)[a-z]*\b
check-integer-only: build $(INT_ONLY_OBJ) $(HAWKV_SIGN_SOURCE)
	@for f in $(INT_ONLY_SRC); do \
		$(CC) $(E8_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -mgeneral-regs-only -c -o /dev/null $$f || exit 1; \
	done
	@for v in $(INT_ONLY_VARIANTS); do \
		$(CC) $(E8_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -mgeneral-regs-only $${v#*:} -c -o /dev/null $${v%%:*} || exit 1; \
	done
	@for f in $(INT_ONLY_CMN_SRC); do \
		$(CC) $(CMN_CFLAGS) $(E8_THREAD_CFLAGS) -mgeneral-regs-only -c -o /dev/null $$f || exit 1; \
	done
	@for v in $(INT_ONLY_HAWKV_SRC); do \
		$(CC) $(CFLAGS) $(HAWKV_INC) $(E8_THREAD_CFLAGS) -mgeneral-regs-only $${v#*:} -c -o /dev/null $${v%%:*} || exit 1; \
	done
	@for o in $(INT_ONLY_OBJ); do \
		if objdump -d --no-show-raw-insn $$o | awk -F'\t' 'NF >= 2 { print $$2 }' | grep -Eq '$(FP_INSN)'; then \
			echo "check-integer-only: floating-point instruction in $$o:"; \
			objdump -d --no-show-raw-insn $$o | awk -F'\t' 'NF >= 2 { print $$2 }' | grep -E '$(FP_INSN)' | sort | uniq -c; \
			exit 1; \
		fi; \
	done
	@echo "check-integer-only: signing path is integer-only"

FORCE:

build:
	-mkdir -p build/hawk build/profile build/common build/hawkv
bin:
	-mkdir -p bin
clean:
	-rm -rf build bin results

# Generated upstream HAWK sources:
$(HAWK_STAMP): ../build.py ../src/*.c ../src/*.h
	-mkdir -p build/hawk
	cd .. && ./build.py ref > /dev/null
	touch $(HAWK_STAMP)

# Binaries:
$(E8_TEST_PROGS) $(E8_TOOL_PROGS) $(E8_BENCH_PROGS): | bin

# Tests
bin/test_e8_math: tests/test_e8_math.c build/e8_math.o
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_math tests/test_e8_math.c build/e8_math.o $(LIBS)
bin/test_e8_arith: tests/test_e8_arith.c $(E8_OBJ) $(E8_SAMPLER_OBJ) build/hawk/sha3.o
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_arith tests/test_e8_arith.c $(E8_OBJ) $(E8_SAMPLER_OBJ) build/hawk/sha3.o $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_public: tests/test_e8_public.c $(HAWK_OBJ) build/e8_math.o
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_public tests/test_e8_public.c $(HAWK_OBJ) build/e8_math.o $(LIBS)
bin/test_e8_verify: tests/test_e8_verify.c $(E8_OBJ) $(E8_SAMPLER_OBJ) build/hawk/sha3.o
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_verify tests/test_e8_verify.c $(E8_OBJ) $(E8_SAMPLER_OBJ) build/hawk/sha3.o $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_sign: tests/test_e8_sign.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_sign tests/test_e8_sign.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_sampler: tests/test_e8_sampler.c build/e8_math.o build/hawk/sha3.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_sampler tests/test_e8_sampler.c build/e8_math.o build/hawk/sha3.o $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_sign_sampler: tests/test_e8_sign_sampler.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_sign_sampler tests/test_e8_sign_sampler.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_api: tests/test_e8_api.c compare/hawk_shim.h $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_api tests/test_e8_api.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_tables: tests/test_e8_tables.c $(E8_HEAD) $(E8_TABLES) $(HAWK_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_tables tests/test_e8_tables.c $(HAWK_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_e8_variants: tests/test_e8_variants.c $(E8_HEAD) $(HAWK_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_variants tests/test_e8_variants.c $(HAWK_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
E8_SAMPLER_REF_OPS_OBJ = $(filter-out build/e8_sampler_ref.o,$(E8_SAMPLER_OBJ)) build/e8_sampler_ref_ops.o
bin/test_e8_ref_ct: tests/test_e8_ref_ct.c $(E8_HEAD) $(E8_TABLES) build/e8_math.o build/hawk/sha3.o $(E8_SAMPLER_REF_OPS_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/test_e8_ref_ct tests/test_e8_ref_ct.c build/e8_math.o build/hawk/sha3.o $(E8_SAMPLER_REF_OPS_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_hawk_variants: tests/test_hawk_variants.c $(HAWKV_HEAD) $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/test_hawk_variants tests/test_hawk_variants.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/test_cmn_common: tests/test_cmn_common.c $(CMN_HEAD) $(CMN_OBJ) build/hawk/sha3.o
	$(CC) $(CMN_CFLAGS) -Icommon $(E8_THREAD_CFLAGS) -o bin/test_cmn_common tests/test_cmn_common.c $(CMN_OBJ) build/hawk/sha3.o $(LIBS) $(E8_THREAD_LIBS)

# Tools
bin/e8_histograms: tools/e8_histograms.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_histograms tools/e8_histograms.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
# The rejection summary reports the sampler's per-stage profile, so it
# links the profiled sampler.
bin/e8_rejection_summary: tools/e8_rejection_summary.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_PROFILE_OBJ)
	$(CC) $(E8_PROFILE_SAMPLER_CFLAGS) $(E8_INC) -o bin/e8_rejection_summary tools/e8_rejection_summary.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_PROFILE_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
# Step 6 validation (docs/validation.md). The correctness tool is built
# twice: with the default flags (E8 debug checks on) and without the E8
# debug checks (HAWK_E8_DEBUG_CHECKS=0), from its own E8 objects in
# build/nodebug/, for the long run; the default objects keep the default
# flags. Only the E8 sources use the flag; HAWK, the HAWK variants and the
# shared modules are the same objects in both.
E8_CORRECTNESS_DEPS = $(HAWKV_HEAD) $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(CMN_OBJ)
bin/e8_correctness: tools/e8_correctness.c $(E8_CORRECTNESS_DEPS) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_correctness tools/e8_correctness.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_correctness_nodebug: tools/e8_correctness.c $(E8_CORRECTNESS_DEPS) $(E8_NODEBUG_OBJ)
	$(CC) $(E8_NODEBUG_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_correctness_nodebug tools/e8_correctness.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_NODEBUG_OBJ) $(CMN_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_distribution_sample: tools/e8_distribution_sample.c $(E8_CORRECTNESS_DEPS) $(E8_TABLES) build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_distribution_sample tools/e8_distribution_sample.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_validation_summary: tools/e8_validation_summary.c compare/hawk_shim.h $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_validation_summary tools/e8_validation_summary.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm

# Comparison benchmarks
bin/e8_sampler_bench: compare/e8_sampler_bench.c $(HAWKV_HEAD) $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_sampler_bench compare/e8_sampler_bench.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_sampler_profile_bench: compare/e8_sampler_bench.c $(HAWKV_HEAD) $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) build/e8_math.o $(E8_SAMPLER_PROFILE_OBJ)
	$(CC) $(E8_PROFILE_SAMPLER_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_sampler_profile_bench compare/e8_sampler_bench.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) build/e8_math.o $(E8_SAMPLER_PROFILE_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_sign_bench: compare/e8_sign_bench.c $(BSC_HEAD) $(HAWKV_HEAD) $(E8_HEAD) $(BSC_OBJ) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_sign_bench compare/e8_sign_bench.c $(BSC_OBJ) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_sign_profile_bench: compare/e8_sign_bench.c $(BSC_HEAD) $(HAWKV_HEAD) $(E8_HEAD) $(BSC_OBJ) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_PROFILE_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_PROFILE_SIGN_CFLAGS) $(E8_INC) $(HAWKV_INC) -o bin/e8_sign_profile_bench compare/e8_sign_bench.c $(BSC_OBJ) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(HAWKV_OBJ) $(E8_PROFILE_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/hawk_sign_profile_bench: compare/hawk_sign_profile_bench.c $(HAWK_PROFILE_SIGN_SOURCE) build/bench_build_info.h $(HAWK_OBJ_NOSIGN)
	$(CC) $(HAWK_PROFILE_SIGN_CFLAGS) -I$(HAWK_DIR) -Ibuild -o bin/hawk_sign_profile_bench compare/hawk_sign_profile_bench.c $(HAWK_OBJ_NOSIGN) $(LIBS)

# The signing core shared by e8_sign_bench and bench_compare, in the
# default build (flag set "default": the default flags, E8 debug checks
# on; not for timing).
$(BSC_OBJ): compare/bench_sign_core.c $(BSC_HEAD) $(HAWKV_HEAD) $(E8_HEAD) build/bench_build_info.h
	$(CC) $(E8_CFLAGS) $(E8_INC) $(HAWKV_INC) -Ibuild -c -o $(BSC_OBJ) compare/bench_sign_core.c
build/bench_build_info.h: FORCE
	@mkdir -p build
	@printf '#define BENCH_FLAGSET "%s"\n#define BENCH_CFLAGS "%s"\n' 'default' '$(CFLAGS) | e8: -DHAWK_ENABLE_E8_EXPERIMENTAL=1' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# ---------------------------------------------------------------------
# Timed builds (plan.md Steps 7 and 9; fairness item 6: one compiler, one
# flag set for every object). Each flag set FS in BENCH_FLAGSETS has its
# own object directory build/bench-FS/ holding every object of the timed
# binaries: upstream HAWK, the HAWK variants, the shim, the shared
# modules, E8 without the debug checks (HAWK_E8_DEBUG_CHECKS=0) and the
# signing core, all compiled with BENCH_CFLAGS_FS -pthread (E8 objects
# add BENCH_E8_DEFS). The default objects and CFLAGS are unchanged.
# Binaries are named after the flag set:
#   bin/bench_compare-FS          the harness (compare/bench_compare.c)
#   bin/bench_compare_avx2-FS     HAWK AVX2 context (upstream
#                                 Optimized_Implementation/avx2 compiled
#                                 into build/bench-FS/avx2/; HAWK only,
#                                 variant ref-avx2)
#   bin/e8_sign_bench-FS          e8_sign_bench
#   bin/e8_sampler_bench-FS       sampler-only timing (--variant-matrix)
#   bin/e8_sign_profile_bench-FS  E8 stage breakdown (profile timers on)
#   bin/hawk_sign_profile_bench-FS  HAWK stage breakdown (profile timers on)
# build/bench-FS/bench_build_info.h records the flag-set name and flags
# (printed by --build-info and written to every CSV row's flags column).
BENCH_FLAGSETS = O2 O3native
BENCH_CFLAGS_O2 = -Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always
# -fno-strict-aliasing: upstream hawk_sign.c reads bytes stored as uint64_t
# through uint32_t pointers (bp_muladd_128/bp_muladd_64, HAWK_UNALIGNED),
# undefined behaviour that -O3 -march=native (GCC 15.2, znver5) turns into
# signatures failing verification (upstream test_self fails too). The flag
# applies to every object of the flag set, both schemes alike
# (docs/harness.md, "Builds").
BENCH_CFLAGS_O3native = -Wall -Wextra -Wshadow -Wundef -O3 -march=native -fno-strict-aliasing -fdiagnostics-color=always
BENCH_THREAD_CFLAGS = -pthread
BENCH_E8_DEFS = -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0
AVX2_DIR = ../Optimized_Implementation/avx2
AVX2_STAMP = build/avx2_sources.stamp
BENCH_HAWK_NAMES = hawk_kgen hawk_vrfy ng_fxp ng_hawk ng_mp31 ng_ntru ng_poly ng_zint31 sha3
BENCH_E8_NAMES = e8_math e8_f2 e8_ntt e8_codec e8_vrfy e8_sign e8_sign_legacy e8_api e8_sampler e8_sampler_ref e8_sampler_hot64 e8_sampler_hot32 e8_sampler_variants e8_sampler_runtime
BENCH_E8_PROFILE_NAMES = $(filter-out e8_sign e8_api,$(BENCH_E8_NAMES))
BENCH_CMN_NAMES = cmn_timing cmn_rng cmn_pool cmn_csv
BENCH_HAWKV_NAMES = hawk_sign_variant hawk_variants hawk_sampler_ref hawk_sampler_hot64 hawk_sampler_hot32
BENCH_BINS = $(foreach fs,$(BENCH_FLAGSETS),bin/bench_compare-$(fs) bin/bench_compare_avx2-$(fs) bin/e8_sign_bench-$(fs) bin/e8_sampler_bench-$(fs) bin/e8_sign_profile_bench-$(fs) bin/hawk_sign_profile_bench-$(fs))

bench-builds: build bin $(BENCH_BINS)

$(AVX2_STAMP): ../build.py ../src/*.c ../src/*.h
	-mkdir -p build
	cd .. && ./build.py avx2 > /dev/null
	touch $(AVX2_STAMP)

# BENCH_FLAGSET_RULES(FS): objects and binaries of flag set FS. Every
# object depends on build/bench-FS/bench_build_info.h, which changes
# exactly when the flags do, so a flag change rebuilds the flag set.
define BENCH_FLAGSET_RULES
BD_$(1) = build/bench-$(1)
BCF_$(1) = $$(BENCH_CFLAGS_$(1)) $$(BENCH_THREAD_CFLAGS)
BHAWK_$(1) = $$(BENCH_HAWK_NAMES:%=build/bench-$(1)/hawk/%.o)
BCMN_$(1) = $$(BENCH_CMN_NAMES:%=build/bench-$(1)/common/%.o)
BHAWKV_$(1) = $$(BENCH_HAWKV_NAMES:%=build/bench-$(1)/hawkv/%.o)
BE8_$(1) = $$(BENCH_E8_NAMES:%=build/bench-$(1)/e8/%.o)
BE8P_$(1) = $$(BENCH_E8_PROFILE_NAMES:%=build/bench-$(1)/e8/%.o) build/bench-$(1)/profile/e8_sign.o build/bench-$(1)/profile/e8_api.o
BSAMP_$(1) = $$(filter build/bench-$(1)/e8/e8_math.o build/bench-$(1)/e8/e8_sampler%,$$(BE8_$(1)))
BAVX2_$(1) = $$(BENCH_HAWK_NAMES:%=build/bench-$(1)/avx2/hawk/%.o) build/bench-$(1)/avx2/hawk_shim.o $$(BENCH_CMN_NAMES:%=build/bench-$(1)/avx2/common/%.o) build/bench-$(1)/avx2/bench_sign_core.o
BLINK_$(1) = $$(BHAWK_$(1)) build/bench-$(1)/hawk_shim.o $$(BHAWKV_$(1)) $$(BCMN_$(1))
BINC_$(1) = $$(E8_INC) $$(HAWKV_INC) -Ibuild/bench-$(1)

build/bench-$(1)/bench_build_info.h: FORCE
	@mkdir -p build/bench-$(1)
	@printf '#define BENCH_FLAGSET "%s"\n#define BENCH_CFLAGS "%s"\n' '$(1)' '$$(BCF_$(1)) | e8: $$(BENCH_E8_DEFS)' > $$@.tmp
	@if cmp -s $$@.tmp $$@; then rm -f $$@.tmp; else mv $$@.tmp $$@; fi

build/bench-$(1)/hawk/%.o: $$(HAWK_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/hawk
	$$(CC) $$(BCF_$(1)) -c -o $$@ $$(HAWK_DIR)/$$*.c
build/bench-$(1)/hawk_shim.o: compare/hawk_shim.c compare/hawk_shim.h $$(HAWK_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)
	$$(CC) $$(BCF_$(1)) -I$$(HAWK_DIR) -c -o $$@ compare/hawk_shim.c
build/bench-$(1)/common/%.o: common/%.c $$(CMN_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/common
	$$(CC) $$(BCF_$(1)) -I$$(HAWK_DIR) -c -o $$@ $$<
build/bench-$(1)/hawkv/hawk_sign_variant.o: $$(HAWKV_SIGN_SOURCE) $$(HAWKV_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/hawkv
	$$(CC) $$(BCF_$(1)) -DHAWK_AVX2=0 $$(HAWKV_INC) -c -o $$@ $$(HAWKV_SIGN_SOURCE)
build/bench-$(1)/hawkv/hawk_sampler_hot64.o: $$(HAWKV_DIR)/hawk_sampler_hot.c $$(HAWKV_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/hawkv
	$$(CC) $$(BCF_$(1)) $$(HAWKV_INC) -DHAWK_HOT_BITS=64 -c -o $$@ $$<
build/bench-$(1)/hawkv/hawk_sampler_hot32.o: $$(HAWKV_DIR)/hawk_sampler_hot.c $$(HAWKV_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/hawkv
	$$(CC) $$(BCF_$(1)) $$(HAWKV_INC) -DHAWK_HOT_BITS=32 -c -o $$@ $$<
build/bench-$(1)/hawkv/%.o: $$(HAWKV_DIR)/%.c $$(HAWKV_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/hawkv
	$$(CC) $$(BCF_$(1)) $$(HAWKV_INC) -c -o $$@ $$<
build/bench-$(1)/e8/e8_sampler_hot64.o: src/e8_sampler_hot.c $$(E8_HEAD) $$(E8_TABLES) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/e8
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(E8_INC) -DE8_HOT_BITS=64 -c -o $$@ $$<
build/bench-$(1)/e8/e8_sampler_hot32.o: src/e8_sampler_hot.c $$(E8_HEAD) $$(E8_TABLES) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/e8
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(E8_INC) -DE8_HOT_BITS=32 -c -o $$@ $$<
build/bench-$(1)/e8/%.o: src/%.c $$(E8_HEAD) $$(E8_TABLES) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/e8
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(E8_INC) -c -o $$@ $$<
build/bench-$(1)/profile/%.o: src/%.c $$(E8_HEAD) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/profile
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) -DHAWK_E8_PROFILE_SIGN=1 -DHAWK_E8_PROFILE_SAMPLER=0 $$(E8_INC) -c -o $$@ $$<
build/bench-$(1)/bench_sign_core.o: compare/bench_sign_core.c $$(BSC_HEAD) $$(HAWKV_HEAD) $$(E8_HEAD) build/bench-$(1)/bench_build_info.h
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(BINC_$(1)) -c -o $$@ compare/bench_sign_core.c

build/bench-$(1)/avx2/hawk/%.o: $$(AVX2_STAMP) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/avx2/hawk
	$$(CC) $$(BCF_$(1)) -c -o $$@ $$(AVX2_DIR)/$$*.c
build/bench-$(1)/avx2/hawk_shim.o: compare/hawk_shim.c compare/hawk_shim.h $$(AVX2_STAMP) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/avx2
	$$(CC) $$(BCF_$(1)) -I$$(AVX2_DIR) -c -o $$@ compare/hawk_shim.c
build/bench-$(1)/avx2/common/%.o: common/%.c $$(CMN_HEAD) $$(AVX2_STAMP) build/bench-$(1)/bench_build_info.h
	@mkdir -p build/bench-$(1)/avx2/common
	$$(CC) $$(BCF_$(1)) -I$$(AVX2_DIR) -c -o $$@ $$<
build/bench-$(1)/avx2/bench_sign_core.o: compare/bench_sign_core.c $$(BSC_HEAD) build/bench-$(1)/bench_build_info.h $$(AVX2_STAMP)
	@mkdir -p build/bench-$(1)/avx2
	$$(CC) $$(BCF_$(1)) -DBSC_HAWK_ONLY=1 -I$$(AVX2_DIR) -Icommon -Icompare -Ibuild/bench-$(1) -c -o $$@ compare/bench_sign_core.c

bin/bench_compare-$(1): compare/bench_compare.c $$(BSC_HEAD) $$(HAWKV_HEAD) $$(E8_HEAD) build/bench-$(1)/bench_sign_core.o $$(BLINK_$(1)) $$(BE8_$(1)) | bin
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(BINC_$(1)) -o $$@ compare/bench_compare.c build/bench-$(1)/bench_sign_core.o $$(BLINK_$(1)) $$(BE8_$(1)) $$(LIBS) $$(E8_THREAD_LIBS) -lm
bin/bench_compare_avx2-$(1): compare/bench_compare.c $$(BSC_HEAD) $$(BAVX2_$(1)) | bin
	$$(CC) $$(BCF_$(1)) -DBSC_HAWK_ONLY=1 -I$$(AVX2_DIR) -Icommon -Icompare -Ibuild/bench-$(1) -o $$@ compare/bench_compare.c $$(BAVX2_$(1)) $$(LIBS) $$(E8_THREAD_LIBS)
bin/e8_sign_bench-$(1): compare/e8_sign_bench.c $$(BSC_HEAD) $$(HAWKV_HEAD) $$(E8_HEAD) build/bench-$(1)/bench_sign_core.o $$(BLINK_$(1)) $$(BE8_$(1)) | bin
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(BINC_$(1)) -o $$@ compare/e8_sign_bench.c build/bench-$(1)/bench_sign_core.o $$(BLINK_$(1)) $$(BE8_$(1)) $$(LIBS) $$(E8_THREAD_LIBS) -lm
bin/e8_sign_profile_bench-$(1): compare/e8_sign_bench.c $$(BSC_HEAD) $$(HAWKV_HEAD) $$(E8_HEAD) build/bench-$(1)/bench_sign_core.o $$(BLINK_$(1)) $$(BE8P_$(1)) | bin
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) -DHAWK_E8_PROFILE_SIGN=1 -DHAWK_E8_PROFILE_SAMPLER=0 $$(BINC_$(1)) -o $$@ compare/e8_sign_bench.c build/bench-$(1)/bench_sign_core.o $$(BLINK_$(1)) $$(BE8P_$(1)) $$(LIBS) $$(E8_THREAD_LIBS) -lm
bin/e8_sampler_bench-$(1): compare/e8_sampler_bench.c $$(HAWKV_HEAD) $$(E8_HEAD) $$(BLINK_$(1)) $$(BSAMP_$(1)) | bin
	$$(CC) $$(BCF_$(1)) $$(BENCH_E8_DEFS) $$(BINC_$(1)) -o $$@ compare/e8_sampler_bench.c $$(BLINK_$(1)) $$(BSAMP_$(1)) $$(LIBS) $$(E8_THREAD_LIBS) -lm
bin/hawk_sign_profile_bench-$(1): compare/hawk_sign_profile_bench.c $$(HAWK_PROFILE_SIGN_SOURCE) build/bench-$(1)/bench_build_info.h $$(BHAWK_$(1)) | bin
	$$(CC) $$(BCF_$(1)) -DHAWK_AVX2=0 -DHAWK_PROFILE_SIGN=1 -I$$(HAWK_DIR) -Ibuild/bench-$(1) -o $$@ compare/hawk_sign_profile_bench.c $$(BHAWK_$(1)) $$(LIBS)
endef
$(foreach fs,$(BENCH_FLAGSETS),$(eval $(call BENCH_FLAGSET_RULES,$(fs))))

# Object files:

# Upstream HAWK, compiled with the upstream reference flags.
build/hawk/hawk_kgen.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/hawk_kgen.o $(HAWK_DIR)/hawk_kgen.c
build/hawk/hawk_sign.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/hawk_sign.o $(HAWK_DIR)/hawk_sign.c
build/hawk/hawk_vrfy.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/hawk_vrfy.o $(HAWK_DIR)/hawk_vrfy.c
build/hawk/ng_fxp.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/ng_fxp.o $(HAWK_DIR)/ng_fxp.c
build/hawk/ng_hawk.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/ng_hawk.o $(HAWK_DIR)/ng_hawk.c
build/hawk/ng_mp31.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/ng_mp31.o $(HAWK_DIR)/ng_mp31.c
build/hawk/ng_ntru.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/ng_ntru.o $(HAWK_DIR)/ng_ntru.c
build/hawk/ng_poly.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/ng_poly.o $(HAWK_DIR)/ng_poly.c
build/hawk/ng_zint31.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/ng_zint31.o $(HAWK_DIR)/ng_zint31.c
build/hawk/sha3.o: $(HAWK_HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk/sha3.o $(HAWK_DIR)/sha3.c

# The single bridge to static HAWK internals (see compare/hawk_shim.h).
$(HAWK_SHIM_OBJ): compare/hawk_shim.c compare/hawk_shim.h $(HAWK_HEAD)
	$(CC) $(CFLAGS) -I$(HAWK_DIR) -c -o $(HAWK_SHIM_OBJ) compare/hawk_shim.c

# Shared measurement modules (common/), compiled with the base flags.
build/common/cmn_timing.o: common/cmn_timing.c $(CMN_HEAD)
	-mkdir -p build/common
	$(CC) $(CMN_CFLAGS) -c -o build/common/cmn_timing.o common/cmn_timing.c
build/common/cmn_rng.o: common/cmn_rng.c $(CMN_HEAD)
	-mkdir -p build/common
	$(CC) $(CMN_CFLAGS) -c -o build/common/cmn_rng.o common/cmn_rng.c
build/common/cmn_pool.o: common/cmn_pool.c $(CMN_HEAD)
	-mkdir -p build/common
	$(CC) $(CMN_CFLAGS) $(E8_THREAD_CFLAGS) -c -o build/common/cmn_pool.o common/cmn_pool.c
build/common/cmn_csv.o: common/cmn_csv.c $(CMN_HEAD)
	-mkdir -p build/common
	$(CC) $(CMN_CFLAGS) -c -o build/common/cmn_csv.o common/cmn_csv.c

# Instrumented copy of upstream HAWK signing; the upstream source is untouched.
$(HAWK_PROFILE_SIGN_SOURCE): ../src/hawk_sign.c compare/hawk_sign_profile.patch
	-mkdir -p build
	patch --silent --fuzz=0 -o $(HAWK_PROFILE_SIGN_SOURCE) ../src/hawk_sign.c compare/hawk_sign_profile.patch

# HAWK variant signer: upstream hawk_sign.c with the sampler call routed
# through hawk_variant_sampler() and renamed entry points; the upstream
# source is untouched. Compiled with the upstream reference flags.
$(HAWKV_SIGN_SOURCE): ../src/hawk_sign.c $(HAWKV_DIR)/hawk_sign_variant.patch
	-mkdir -p build
	patch --silent --fuzz=0 -o $(HAWKV_SIGN_SOURCE) ../src/hawk_sign.c $(HAWKV_DIR)/hawk_sign_variant.patch
build/hawkv/hawk_sign_variant.o: $(HAWKV_SIGN_SOURCE) $(HAWKV_HEAD)
	-mkdir -p build/hawkv
	$(CC) $(CFLAGS) -DHAWK_AVX2=0 $(HAWKV_INC) -c -o build/hawkv/hawk_sign_variant.o $(HAWKV_SIGN_SOURCE)
build/hawkv/hawk_variants.o: $(HAWKV_DIR)/hawk_variants.c $(HAWKV_HEAD)
	-mkdir -p build/hawkv
	$(CC) $(CFLAGS) $(HAWKV_INC) $(E8_THREAD_CFLAGS) -c -o build/hawkv/hawk_variants.o $(HAWKV_DIR)/hawk_variants.c
build/hawkv/hawk_sampler_ref.o: $(HAWKV_DIR)/hawk_sampler_ref.c $(HAWKV_HEAD)
	-mkdir -p build/hawkv
	$(CC) $(CFLAGS) $(HAWKV_INC) -c -o build/hawkv/hawk_sampler_ref.o $(HAWKV_DIR)/hawk_sampler_ref.c
build/hawkv/hawk_sampler_hot64.o: $(HAWKV_DIR)/hawk_sampler_hot.c $(HAWKV_HEAD)
	-mkdir -p build/hawkv
	$(CC) $(CFLAGS) $(HAWKV_INC) -DHAWK_HOT_BITS=64 -c -o build/hawkv/hawk_sampler_hot64.o $(HAWKV_DIR)/hawk_sampler_hot.c
build/hawkv/hawk_sampler_hot32.o: $(HAWKV_DIR)/hawk_sampler_hot.c $(HAWKV_HEAD)
	-mkdir -p build/hawkv
	$(CC) $(CFLAGS) $(HAWKV_INC) -DHAWK_HOT_BITS=32 -c -o build/hawkv/hawk_sampler_hot32.o $(HAWKV_DIR)/hawk_sampler_hot.c

# E8
build/e8_math.o: src/e8_math.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_math.o src/e8_math.c
build/e8_f2.o: src/e8_f2.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_f2.o src/e8_f2.c
build/e8_ntt.o: src/e8_ntt.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_ntt.o src/e8_ntt.c
build/e8_vrfy.o: src/e8_vrfy.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_vrfy.o src/e8_vrfy.c
build/e8_codec.o: src/e8_codec.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_codec.o src/e8_codec.c
build/e8_sign_legacy.o: src/e8_sign_legacy.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_sign_legacy.o src/e8_sign_legacy.c
build/e8_sign.o: src/e8_sign.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_sign.o src/e8_sign.c
build/e8_api.o: src/e8_api.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_api.o src/e8_api.c
build/e8_sampler.o: src/e8_sampler.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -c -o build/e8_sampler.o src/e8_sampler.c
# One object per sampler variant, from the same source with its own
# threshold width and symbols, so that all variants link into one binary.
build/e8_sampler_hot64.o: src/e8_sampler_hot.c $(E8_HEAD) $(E8_TABLES)
	$(CC) $(E8_CFLAGS) $(E8_INC) -DE8_HOT_BITS=64 -c -o build/e8_sampler_hot64.o src/e8_sampler_hot.c
build/e8_sampler_hot32.o: src/e8_sampler_hot.c $(E8_HEAD) $(E8_TABLES)
	$(CC) $(E8_CFLAGS) $(E8_INC) -DE8_HOT_BITS=32 -c -o build/e8_sampler_hot32.o src/e8_sampler_hot.c
build/e8_sampler_ref.o: src/e8_sampler_ref.c $(E8_HEAD) $(E8_TABLES)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_sampler_ref.o src/e8_sampler_ref.c
# ref with loop iteration counters, for the structural constant-time test.
build/e8_sampler_ref_ops.o: src/e8_sampler_ref.c $(E8_HEAD) $(E8_TABLES)
	$(CC) $(E8_CFLAGS) $(E8_INC) -DE8_REF_COUNT_OPS=1 -c -o build/e8_sampler_ref_ops.o src/e8_sampler_ref.c
build/e8_sampler_variants.o: src/e8_sampler_variants.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_sampler_variants.o src/e8_sampler_variants.c
build/e8_sampler_runtime.o: src/e8_sampler_runtime.c $(E8_HEAD) $(E8_TABLES)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -c -o build/e8_sampler_runtime.o src/e8_sampler_runtime.c

# E8 objects without the debug checks, for bin/e8_correctness_nodebug.
build/nodebug/e8_sampler_hot64.o: src/e8_sampler_hot.c $(E8_HEAD) $(E8_TABLES)
	-mkdir -p build/nodebug
	$(CC) $(E8_NODEBUG_CFLAGS) $(E8_INC) -DE8_HOT_BITS=64 -c -o build/nodebug/e8_sampler_hot64.o src/e8_sampler_hot.c
build/nodebug/e8_sampler_hot32.o: src/e8_sampler_hot.c $(E8_HEAD) $(E8_TABLES)
	-mkdir -p build/nodebug
	$(CC) $(E8_NODEBUG_CFLAGS) $(E8_INC) -DE8_HOT_BITS=32 -c -o build/nodebug/e8_sampler_hot32.o src/e8_sampler_hot.c
build/nodebug/%.o: src/%.c $(E8_HEAD) $(E8_TABLES)
	-mkdir -p build/nodebug
	$(CC) $(E8_NODEBUG_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -c -o $@ $<

# E8 sampler with its per-stage profile (HAWK_E8_PROFILE_SAMPLER=1), used
# only by the profiled sampler benchmark and the rejection summary.
build/profile/e8_sampler.o: src/e8_sampler.c $(E8_HEAD)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SAMPLER_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -c -o build/profile/e8_sampler.o src/e8_sampler.c
build/profile/e8_sampler_ref.o: src/e8_sampler_ref.c $(E8_HEAD) $(E8_TABLES)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SAMPLER_CFLAGS) $(E8_INC) -c -o build/profile/e8_sampler_ref.o src/e8_sampler_ref.c
build/profile/e8_sampler_hot64.o: src/e8_sampler_hot.c $(E8_HEAD) $(E8_TABLES)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SAMPLER_CFLAGS) $(E8_INC) -DE8_HOT_BITS=64 -c -o build/profile/e8_sampler_hot64.o src/e8_sampler_hot.c
build/profile/e8_sampler_hot32.o: src/e8_sampler_hot.c $(E8_HEAD) $(E8_TABLES)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SAMPLER_CFLAGS) $(E8_INC) -DE8_HOT_BITS=32 -c -o build/profile/e8_sampler_hot32.o src/e8_sampler_hot.c

# E8 signing objects with per-stage profiling (HAWK_E8_PROFILE_SIGN=1), used
# only by the profile benchmark; the profile changes e8_sign_trace_timing.
build/profile/e8_sign.o: src/e8_sign.c $(E8_HEAD)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SIGN_CFLAGS) $(E8_INC) -c -o build/profile/e8_sign.o src/e8_sign.c
build/profile/e8_api.o: src/e8_api.c $(E8_HEAD)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SIGN_CFLAGS) $(E8_INC) -c -o build/profile/e8_api.o src/e8_api.c
