I want to design a known nodeid pool, where:
- every known nodeid has an entry in the pool
- registering a nodeid gets an index into the pool
- given the index, we can obtain the nodeid quickly
- make a type for this index so compiler helps us catch errors
- use this index everywhere else in the code for both src/* and client/*.c replacing
  raw nodeid in data structures etc.
- do this in C
