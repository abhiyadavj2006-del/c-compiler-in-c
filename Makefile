CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O2

cc0: src/main.c
	$(CC) $(CFLAGS) -o cc0 src/main.c

test: cc0
	./run_tests.sh

clean:
	rm -f cc0 a.out tests/*.out tests/*.s

.PHONY: test clean
