# This Makefile compiles the implementation in this directory.
.PHONY: avx2 ref test-e8 sampler-bench e8-rejection-summary
.POSIX:

all: build avx2 ref

build: src/*
	./build.py
avx2:
	make -C Optimized_Implementation/avx2
ref:
	make -C Reference_Implementation
test-e8: build
	make -C Reference_Implementation test-e8
sampler-bench:
	@make -s -C Reference_Implementation --no-print-directory sampler-bench
e8-rejection-summary:
	@make -s -C Reference_Implementation --no-print-directory e8-rejection-summary
