HFILES  := distcache.h
CFILES  := distcache.c
OBJ     := $(CFILES:.c=.o)
EXEC    := test_distcache
LIB     := libdistcache.a

CFLAGS  := -std=c17 -Wmissing-declarations -Wall -Wextra -MMD -pthread
LDFLAGS := -lcurl -pthread

ifdef DEBUG
    CFLAGS += -O0 -g -fsanitize=address,undefined
else
    CFLAGS += -O3 -DNDEBUG
endif


all: $(LIB) $(EXEC)

$(LIB): $(OBJ)
	ar rcs $@ $^

$(EXEC): test_distcache.c distcache.c
	$(CC) $(CFLAGS) -DDC_TESTING -o $@ $^ $(LDFLAGS)

*.o: $(HFILES)

-include $(OBJ:%.o=%.d) $(EXEC:%=%.d)

check test: $(EXEC)
	./$(EXEC)

clean:
	rm -f *.o *.d *.a $(EXEC)

distclean: clean

format:
	clang-format -i *.[ch]

.PHONY: all check test clean distclean format
