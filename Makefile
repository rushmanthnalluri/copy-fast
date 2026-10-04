CC ?= gcc
CFLAGS ?= -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -O3 -g -Iinclude
LDFLAGS ?= -lpthread -luring

SRCS = src/checksum.c \
       src/buffer_pool.c \
       src/sparse.c \
       src/metadata.c \
       src/resume.c \
       src/signals.c \
       src/progress.c \
       src/utils.c \
       src/backend_naive.c \
       src/backend_pipeline.c \
       src/backend_uring.c \
       src/backends.c \
       src/work_queue.c

OBJS = $(SRCS:.c=.o)
MAIN_OBJ = src/main.o

BIN = copyfast
BENCH_BIN = benchmark

all: $(BIN) $(BENCH_BIN)

$(BIN): $(OBJS) $(MAIN_OBJ)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@cp $@ bin/$(BIN) 2>/dev/null || true

$(BENCH_BIN): bench/benchmark.o $(OBJS)
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@cp $@ bin/$(BENCH_BIN) 2>/dev/null || true

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

test: $(BIN)
	@chmod +x tests/test_suite.sh
	./tests/test_suite.sh

bench: $(BENCH_BIN) $(BIN)
	@chmod +x bench/run_benchmarks.sh
	./bench/run_benchmarks.sh

valgrind: $(BIN)
	valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=1 \
		./$(BIN) -Q -s 64K Makefile /tmp/valgrind_test.out
	@rm -f /tmp/valgrind_test.out
	@echo "Valgrind check passed: zero memory leaks!"

clean:
	rm -f $(OBJS) $(MAIN_OBJ) bench/benchmark.o $(BIN) $(BENCH_BIN) bin/$(BIN) bin/$(BENCH_BIN)
	rm -rf /tmp/copyfast_test_* /tmp/bench_*

install: $(BIN)
	install -m 755 $(BIN) /usr/local/bin/

.PHONY: all test bench valgrind clean install
