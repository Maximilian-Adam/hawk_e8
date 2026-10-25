# This Makefile builds the experimental HAWK-E8-CM code, its tests, tools and
# comparison benchmarks against the generated, unmodified HAWK reference
# sources in ../Reference_Implementation (produced from ../src by ../build.py).
.POSIX:

CC = c99
CFLAGS = -Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always
LIBS =

HAWK_DIR = ../Reference_Implementation
HAWK_STAMP = build/hawk_sources.stamp

HAWK_OBJ = build/hawk/hawk_kgen.o build/hawk/hawk_sign.o build/hawk/hawk_vrfy.o build/hawk/ng_fxp.o build/hawk/ng_hawk.o build/hawk/ng_mp31.o build/hawk/ng_ntru.o build/hawk/ng_poly.o build/hawk/ng_zint31.o build/hawk/sha3.o
HAWK_OBJ_NOSIGN = $(filter-out build/hawk/hawk_sign.o,$(HAWK_OBJ))
HAWK_SHIM_OBJ = build/hawk_shim.o
E8_OBJ = build/e8_math.o build/e8_f2.o build/e8_ntt.o build/e8_vrfy.o build/e8_sign.o
E8_SAMPLER_OBJ = build/e8_sampler.o
E8_API_OBJ = build/e8_api.o
E8_PROFILE_OBJ = build/e8_math.o build/e8_f2.o build/e8_ntt.o build/e8_vrfy.o build/profile/e8_sign.o build/profile/e8_api.o

E8_TEST_PROGS = bin/test_e8_math bin/test_e8_arith bin/test_e8_public bin/test_e8_verify bin/test_e8_sign bin/test_e8_sampler bin/test_e8_sign_sampler bin/test_e8_api
E8_TOOL_PROGS = bin/e8_histograms bin/e8_rejection_summary bin/e8_validation_summary
E8_BENCH_PROGS = bin/e8_sampler_bench bin/e8_sign_bench bin/e8_sign_profile_bench bin/hawk_sign_profile_bench
HAWK_PROFILE_SIGN_SOURCE = build/hawk_sign_profile.c

HAWK_HEAD = $(HAWK_STAMP)
E8_HEAD = src/hawk_e8_inner.h src/hawk_e8.h $(HAWK_HEAD)
E8_INC = -I$(HAWK_DIR)
E8_CFLAGS = $(CFLAGS) -DHAWK_ENABLE_E8_EXPERIMENTAL=1
E8_PROFILE_SIGN_CFLAGS = $(E8_CFLAGS) -DHAWK_E8_PROFILE_SIGN=1
HAWK_PROFILE_SIGN_CFLAGS = $(CFLAGS) -DHAWK_AVX2=0 -DHAWK_PROFILE_SIGN=1
E8_THREAD_CFLAGS ?= -pthread
E8_THREAD_LIBS ?= -pthread

all: build bin $(E8_TEST_PROGS) $(E8_TOOL_PROGS) $(E8_BENCH_PROGS)
test-e8: build bin $(E8_TEST_PROGS)
	./bin/test_e8_math
	./bin/test_e8_arith
	./bin/test_e8_public
	./bin/test_e8_verify
	./bin/test_e8_sign
	./bin/test_e8_sampler
	./bin/test_e8_sign_sampler
	./bin/test_e8_api
e8-histograms: build bin bin/e8_histograms
	./bin/e8_histograms
sampler-bench: build bin bin/e8_sampler_bench
	@./bin/e8_sampler_bench --selected-configs > e8_sampler_bench.csv
	@printf 'wrote e8_sampler_bench.csv\n'
sign-bench: build bin bin/e8_sign_bench
	@./bin/e8_sign_bench > e8_sign_bench.csv
	@printf 'wrote e8_sign_bench.csv\n'
profile-sign-bench: build bin bin/e8_sign_profile_bench
	@./bin/e8_sign_profile_bench > /dev/null
profile-hawk-sign-bench: build bin bin/hawk_sign_profile_bench
	@./bin/hawk_sign_profile_bench > /dev/null
e8-rejection-summary: build bin bin/e8_rejection_summary
	@./bin/e8_rejection_summary > e8_rejection_summary.csv
	@printf 'wrote e8_rejection_summary.csv\n'
e8-validation-summary: build bin bin/e8_validation_summary
	@./bin/e8_validation_summary
	@printf 'wrote e8_validation_summary.csv e8_validation_shell.csv e8_validation_coset.csv e8_validation_global.csv e8_validation_stability.csv\n'
e8-sigma-verify-sweep: build bin bin/e8_rejection_summary
	@E8_SIGMA_VERIFY_SWEEP=1 ./bin/e8_rejection_summary > e8_sigma_verify_sweep.csv
	@printf 'wrote e8_sigma_verify_sweep.csv\n'
e8-sigma-sign-sweep: build bin bin/e8_rejection_summary
	@E8_SIGMA_SIGN_SWEEP=1 ./bin/e8_rejection_summary > e8_sigma_sign_sweep.csv
	@printf 'wrote e8_sigma_sign_sweep.csv\n'

build:
	-mkdir -p build/hawk build/profile
bin:
	-mkdir -p bin
clean:
	-rm -rf build bin
	-rm -f e8_hist_public.csv e8_hist_signatures.csv e8_sampler_bench.csv e8_sign_bench.csv hawk_sign_profile_bench.csv e8_rejection_summary.csv e8_validation_summary.csv e8_validation_shell.csv e8_validation_coset.csv e8_validation_global.csv e8_validation_stability.csv e8_sigma_verify_sweep.csv e8_sigma_sign_sweep.csv

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

# Tools
bin/e8_histograms: tools/e8_histograms.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_histograms tools/e8_histograms.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_rejection_summary: tools/e8_rejection_summary.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_rejection_summary tools/e8_rejection_summary.c $(HAWK_OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_validation_summary: tools/e8_validation_summary.c compare/hawk_shim.h $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_validation_summary tools/e8_validation_summary.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm

# Comparison benchmarks
bin/e8_sampler_bench: compare/e8_sampler_bench.c compare/hawk_shim.h $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_sampler_bench compare/e8_sampler_bench.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_sign_bench: compare/e8_sign_bench.c compare/hawk_shim.h $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) $(E8_INC) -o bin/e8_sign_bench compare/e8_sign_bench.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(E8_OBJ) $(E8_API_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/e8_sign_profile_bench: compare/e8_sign_bench.c compare/hawk_shim.h $(E8_HEAD) $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(E8_PROFILE_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_PROFILE_SIGN_CFLAGS) $(E8_INC) -o bin/e8_sign_profile_bench compare/e8_sign_bench.c $(HAWK_OBJ_NOSIGN) $(HAWK_SHIM_OBJ) $(E8_PROFILE_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) $(E8_THREAD_LIBS) -lm
bin/hawk_sign_profile_bench: compare/hawk_sign_profile_bench.c $(HAWK_PROFILE_SIGN_SOURCE) $(HAWK_OBJ_NOSIGN)
	$(CC) $(HAWK_PROFILE_SIGN_CFLAGS) -I$(HAWK_DIR) -o bin/hawk_sign_profile_bench compare/hawk_sign_profile_bench.c $(HAWK_OBJ_NOSIGN) $(LIBS)

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

# Instrumented copy of upstream HAWK signing; the upstream source is untouched.
$(HAWK_PROFILE_SIGN_SOURCE): ../src/hawk_sign.c compare/hawk_sign_profile.patch
	-mkdir -p build
	patch --silent --fuzz=0 -o $(HAWK_PROFILE_SIGN_SOURCE) ../src/hawk_sign.c compare/hawk_sign_profile.patch

# E8
build/e8_math.o: src/e8_math.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_math.o src/e8_math.c
build/e8_f2.o: src/e8_f2.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_f2.o src/e8_f2.c
build/e8_ntt.o: src/e8_ntt.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_ntt.o src/e8_ntt.c
build/e8_vrfy.o: src/e8_vrfy.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_vrfy.o src/e8_vrfy.c
build/e8_sign.o: src/e8_sign.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_sign.o src/e8_sign.c
build/e8_api.o: src/e8_api.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) -c -o build/e8_api.o src/e8_api.c
build/e8_sampler.o: src/e8_sampler.c $(E8_HEAD)
	$(CC) $(E8_CFLAGS) $(E8_INC) $(E8_THREAD_CFLAGS) -c -o build/e8_sampler.o src/e8_sampler.c

# E8 signing objects with per-stage profiling (HAWK_E8_PROFILE_SIGN=1), used
# only by the profile benchmark; the profile changes e8_sign_trace_timing.
build/profile/e8_sign.o: src/e8_sign.c $(E8_HEAD)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SIGN_CFLAGS) $(E8_INC) -c -o build/profile/e8_sign.o src/e8_sign.c
build/profile/e8_api.o: src/e8_api.c $(E8_HEAD)
	-mkdir -p build/profile
	$(CC) $(E8_PROFILE_SIGN_CFLAGS) $(E8_INC) -c -o build/profile/e8_api.o src/e8_api.c
