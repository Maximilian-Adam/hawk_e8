# This Makefile compiles the implementation in this directory.
.POSIX:

CC = c99
CFLAGS = -Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always
LIBS = 

OBJ = build/hawk_kgen.o build/hawk_sign.o build/hawk_vrfy.o build/ng_fxp.o build/ng_hawk.o build/ng_mp31.o build/ng_ntru.o build/ng_poly.o build/ng_zint31.o build/sha3.o
E8_OBJ = build/e8_math.o build/e8_vrfy.o build/e8_sign.o
E8_SAMPLER_OBJ = build/e8_sampler.o
PROGS = bin/test_self bin/speed bin/test_codec bin/test_sampler
E8_PROGS = bin/test_e8_math bin/test_e8_public bin/test_e8_verify bin/test_e8_sign bin/test_e8_sampler bin/test_e8_sign_sampler
E8_HIST_PROGS = bin/e8_histograms
E8_BENCH_PROGS = bin/e8_sampler_bench
E8_REJECTION_PROGS = bin/e8_rejection_summary

HEAD = hawk.h hawk_inner.h hawk_config.h sha3.h
NG_HEAD = ng_config.h ng_inner.h sha3.h
E8_CFLAGS = $(CFLAGS) -DHAWK_ENABLE_E8_EXPERIMENTAL=1

all: build bin $(PROGS)
test-e8: build bin $(E8_PROGS)
	./bin/test_e8_math
	./bin/test_e8_public
	./bin/test_e8_verify
	./bin/test_e8_sign
	./bin/test_e8_sampler
	./bin/test_e8_sign_sampler
e8-histograms: build bin data $(E8_HIST_PROGS)
	./bin/e8_histograms
sampler-bench: build bin $(E8_BENCH_PROGS)
	@./bin/e8_sampler_bench > e8_sampler_bench.csv
	@printf 'wrote e8_sampler_bench.csv\n'
e8-rejection-summary: build bin $(E8_REJECTION_PROGS)
	@./bin/e8_rejection_summary > e8_rejection_summary.csv
	@printf 'wrote e8_rejection_summary.csv\n'

build:
	-mkdir -p build
bin:
	-mkdir -p bin
data:
	-mkdir -p data

clean:
	-rm -f $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(PROGS) $(E8_PROGS) $(E8_HIST_PROGS) $(E8_BENCH_PROGS) $(E8_REJECTION_PROGS)
	-rm -f e8_hist_public.csv e8_hist_signatures.csv e8_sampler_bench.csv e8_rejection_summary.csv
	-rm -f data/e8_hist_public.csv data/e8_hist_signatures.csv

# Binaries:

# C
bin/test_self: tests/test_self.c $(OBJ)
	$(CC) $(CFLAGS) -o bin/test_self tests/test_self.c $(OBJ) $(LIBS)
bin/speed: tests/speed.c $(OBJ)
	$(CC) $(CFLAGS) -o bin/speed tests/speed.c $(OBJ) $(LIBS)
bin/test_codec: tests/test_codec.c $(OBJ)
	$(CC) $(CFLAGS) -o bin/test_codec tests/test_codec.c $(filter-out build/hawk_kgen.o,$(OBJ)) $(LIBS)
bin/test_sampler: tests/test_sampler.c $(OBJ)
	$(CC) $(CFLAGS) -o bin/test_sampler tests/test_sampler.c $(filter-out build/hawk_sign.o,$(OBJ)) $(LIBS) -lm
bin/test_e8_math: tests/test_e8_math.c build/e8_math.o
	$(CC) $(E8_CFLAGS) -o bin/test_e8_math tests/test_e8_math.c build/e8_math.o $(LIBS)
bin/test_e8_public: tests/test_e8_public.c $(OBJ) build/e8_math.o
	$(CC) $(E8_CFLAGS) -o bin/test_e8_public tests/test_e8_public.c $(OBJ) build/e8_math.o $(LIBS)
bin/test_e8_verify: tests/test_e8_verify.c build/e8_math.o build/e8_vrfy.o build/sha3.o
	$(CC) $(E8_CFLAGS) -o bin/test_e8_verify tests/test_e8_verify.c build/e8_math.o build/e8_vrfy.o build/sha3.o $(LIBS)
bin/test_e8_sign: tests/test_e8_sign.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) -o bin/test_e8_sign tests/test_e8_sign.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) -lm
bin/test_e8_sampler: tests/test_e8_sampler.c build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) -o bin/test_e8_sampler tests/test_e8_sampler.c build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) -lm
bin/test_e8_sign_sampler: tests/test_e8_sign_sampler.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) -o bin/test_e8_sign_sampler tests/test_e8_sign_sampler.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) -lm
bin/e8_histograms: tests/e8_histograms.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) -o bin/e8_histograms tests/e8_histograms.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) -lm
bin/e8_sampler_bench: tests/e8_sampler_bench.c $(filter-out build/hawk_sign.o,$(OBJ)) build/e8_math.o $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) -o bin/e8_sampler_bench tests/e8_sampler_bench.c $(filter-out build/hawk_sign.o,$(OBJ)) build/e8_math.o $(E8_SAMPLER_OBJ) $(LIBS) -lm
bin/e8_rejection_summary: tests/e8_rejection_summary.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ)
	$(CC) $(E8_CFLAGS) -o bin/e8_rejection_summary tests/e8_rejection_summary.c $(OBJ) $(E8_OBJ) $(E8_SAMPLER_OBJ) $(LIBS) -lm

# Object files:
build/hawk_kgen.o: hawk_kgen.c $(HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk_kgen.o hawk_kgen.c
build/hawk_sign.o: hawk_sign.c $(HEAD) modq.h
	$(CC) $(CFLAGS) -c -o build/hawk_sign.o hawk_sign.c
build/hawk_vrfy.o: hawk_vrfy.c $(HEAD)
	$(CC) $(CFLAGS) -c -o build/hawk_vrfy.o hawk_vrfy.c
build/ng_fxp.o: ng_fxp.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/ng_fxp.o ng_fxp.c
build/ng_hawk.o: ng_hawk.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/ng_hawk.o ng_hawk.c
build/ng_mp31.o: ng_mp31.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/ng_mp31.o ng_mp31.c
build/ng_ntru.o: ng_ntru.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/ng_ntru.o ng_ntru.c
build/ng_poly.o: ng_poly.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/ng_poly.o ng_poly.c
build/ng_zint31.o: ng_zint31.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/ng_zint31.o ng_zint31.c
build/sha3.o: sha3.c $(NG_HEAD)
	$(CC) $(CFLAGS) -c -o build/sha3.o sha3.c
build/e8_math.o: e8_math.c hawk_e8_inner.h $(HEAD)
	$(CC) $(E8_CFLAGS) -c -o build/e8_math.o e8_math.c
build/e8_vrfy.o: e8_vrfy.c hawk_e8_inner.h $(HEAD)
	$(CC) $(E8_CFLAGS) -c -o build/e8_vrfy.o e8_vrfy.c
build/e8_sign.o: e8_sign.c hawk_e8_inner.h $(HEAD)
	$(CC) $(E8_CFLAGS) -c -o build/e8_sign.o e8_sign.c
build/e8_sampler.o: e8_sampler.c hawk_e8_inner.h $(HEAD)
	$(CC) $(E8_CFLAGS) -c -o build/e8_sampler.o e8_sampler.c
