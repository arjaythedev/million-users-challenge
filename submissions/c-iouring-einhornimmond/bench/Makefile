# The load generator. Needs zlib's header (zlib1g-dev / zlib-devel).
CFLAGS ?= -O2 -Wall -Wextra
cload: cload.c
	$(CC) $(CFLAGS) -o $@ $< -lz -lm -lpthread
clean:
	rm -f cload
