# This Makefile compiles the implementation in this directory.
.PHONY: avx2 ref test-e8
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
