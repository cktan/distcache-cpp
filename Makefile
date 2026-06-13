.NOTPARALLEL:

DIRS       := src test
BUILDDIRS  := $(DIRS:%=build-%)
CLEANDIRS  := $(DIRS:%=clean-%)
TESTDIRS   := $(DIRS:%=test-%)
FORMATDIRS := $(DIRS:%=format-%)

all: $(BUILDDIRS)

$(BUILDDIRS):
	$(MAKE) -C $(@:build-%=%)

test: $(TESTDIRS)

$(TESTDIRS):
	$(MAKE) -C $(@:test-%=%) test

format: $(FORMATDIRS)

clean: $(CLEANDIRS)

$(CLEANDIRS):
	$(MAKE) -C $(@:clean-%=%) clean

$(FORMATDIRS):
	$(MAKE) -C $(@:format-%=%) format

test-rebalance:
	$(MAKE) -C test test-rebalance

test-s3cluster:
	$(MAKE) -C test test-s3cluster

.PHONY: $(BUILDDIRS) $(CLEANDIRS) $(TESTDIRS) $(FORMATDIRS)
.PHONY: all test format clean test-rebalance test-s3cluster
