# This Makefile compiles the implementation in this directory.
.PHONY: avx2 ref e8 test-e8 e8-histograms sampler-bench profile-sampler-bench sign-bench profile-sign-bench profile-hawk-sign-bench e8-rejection-summary e8-validation-summary e8-sigma-verify-sweep e8-sigma-sign-sweep e8-clean check-upstream
.POSIX:

all: build avx2 ref e8

build: src/*
	./build.py
avx2:
	make -C Optimized_Implementation/avx2
ref:
	make -C Reference_Implementation

# Experimental HAWK-E8-CM: everything lives in e8/ (see e8/README.md).
e8: build
	make -C e8
test-e8: build
	make -C e8 test-e8
e8-histograms e8-rejection-summary e8-validation-summary e8-sigma-verify-sweep e8-sigma-sign-sweep sampler-bench profile-sampler-bench sign-bench profile-sign-bench profile-hawk-sign-bench:
	@make -s -C e8 --no-print-directory $@
e8-clean:
	make -C e8 clean

# Upstream HAWK must stay byte-identical to the upstream merge commit.
UPSTREAM = 1b9fef5
UPSTREAM_PATHS = LICENSE.txt build.py src extra intermediate Reference_Implementation Optimized_Implementation
check-upstream:
	@if git diff --quiet $(UPSTREAM) -- $(UPSTREAM_PATHS) \
		&& test -z "$$(git ls-files --others --exclude-standard -- $(UPSTREAM_PATHS))"; \
	then \
		echo "check-upstream: upstream HAWK files match $(UPSTREAM)"; \
	else \
		echo "check-upstream: upstream HAWK files differ from $(UPSTREAM):"; \
		git diff --stat $(UPSTREAM) -- $(UPSTREAM_PATHS); \
		git ls-files --others --exclude-standard -- $(UPSTREAM_PATHS); \
		exit 1; \
	fi
