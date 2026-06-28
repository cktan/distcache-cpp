CXX ?= c++
CC  ?= cc

CXXFLAGS := -std=c++20 -Wall -Wextra -MMD -pthread \
            -I$(HOME)/.local/include
CFLAGS   := -std=c11 -Wall -Wextra -MMD

ifdef DEBUG
    CXXFLAGS += -O0 -g -fsanitize=address,undefined
    CFLAGS   += -O0 -g -fsanitize=address,undefined
    SAN       := -fsanitize=address,undefined
else
    CXXFLAGS += -O3 -DNDEBUG
    CFLAGS   += -O3 -DNDEBUG
    SAN       :=
endif

LDFLAGS := $(SAN) -L$(HOME)/.local/lib -lswimc17 -pthread -lcurl

EXEC    := distcache
LIB     := libdistcache.a
MAINOBJ := main.o
LIBOBJ  := stream.o ring.o nodeid.o url.o json.o $(patsubst %.cpp,%.o,$(filter-out main.cpp,$(wildcard *.cpp)))

all: $(EXEC)

$(LIB): $(LIBOBJ)
	ar -rcs $@ $^

ifeq ($(strip $(LIBOBJ)),)
$(EXEC): $(MAINOBJ)
	$(CXX) -o $@ $^ $(LDFLAGS)
else
$(EXEC): $(MAINOBJ) $(LIB)
	$(CXX) -o $@ $^ $(LDFLAGS)
endif

-include $(MAINOBJ:.o=.d) $(LIBOBJ:.o=.d)

test: all

format:
	clang-format -i *.cpp *.h *.c

clean:
	rm -f *.o *.d *.a $(EXEC)

distclean: clean

.PHONY: all test format clean distclean
