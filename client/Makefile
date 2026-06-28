HFILES  := distcache.h ../src/stream.h ../src/ring.h ../src/nodeid.h ../src/url.h ../src/json.h
CFILES  := distcache.c
OBJ     := distcache.o stream.o ring.o nodeid.o url.o json.o
EXEC    := test_distcache
LIB     := libdistcache.a

CFLAGS  := -std=c17 -Wmissing-declarations -Wall -Wextra -MMD -pthread -I../src
LDFLAGS := -lcurl -pthread

ifdef DEBUG
    CFLAGS += -O0 -g -fsanitize=address,undefined
else
    CFLAGS += -O3 -DNDEBUG
endif


all: $(LIB) $(EXEC)

$(LIB): $(OBJ)
	ar rcs $@ $^

stream.o: ../src/stream.c ../src/stream.h
	$(CC) $(CFLAGS) -c -o $@ $<

ring.o: ../src/ring.c ../src/ring.h
	$(CC) $(CFLAGS) -c -o $@ $<

nodeid.o: ../src/nodeid.c ../src/nodeid.h
	$(CC) $(CFLAGS) -c -o $@ $<

url.o: ../src/url.c ../src/url.h
	$(CC) $(CFLAGS) -c -o $@ $<

json.o: ../src/json.c ../src/json.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(EXEC): test_distcache.c distcache.c ../src/stream.c ../src/ring.c ../src/nodeid.c ../src/url.c ../src/json.c
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
