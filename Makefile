CFLAGS ?= -O2 -Wall -Wextra -pthread

redis-clone: server.c hashtable.c avl.c heap.c threadpool.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f redis-clone
