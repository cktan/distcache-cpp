include ../Makefile.global

CXX ?= c++

CXXFLAGS := -std=c++20 -Wall -Wextra -MMD -pthread \
            -I$(HOME)/.local/include -I../src

ifdef DEBUG
    CXXFLAGS += -O0 -g -fsanitize=address,undefined
    SAN       := -fsanitize=address,undefined
else
    CXXFLAGS += -O3 -DNDEBUG
    SAN       :=
endif

LDFLAGS  := $(SAN) -L$(HOME)/.local/lib -lswimc17 -pthread
LIB      := ../src/libdistcache.a

TESTBINS := $(patsubst %.cpp,%,$(wildcard test_*.cpp))

all: $(DOCTEST_H) $(TESTBINS)

$(TESTBINS): %: %.cpp $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ $< $(LIB) $(LDFLAGS)

-include $(TESTBINS:%=%.d)

test: all
	@for t in $(TESTBINS); do \
	    ./$$t && echo "$$t: PASS" || { echo "$$t: FAIL"; exit 1; }; \
	done
	@for s in test_*.sh; do \
	    [ -f "$$s" ] && { bash "$$s" && echo "$$s: PASS" || { echo "$$s: FAIL"; exit 1; }; }; \
	done; true

format:
	clang-format -i *.cpp

clean:
	rm -f $(TESTBINS) *.d

distclean: clean

.PHONY: all test format clean distclean
