CFLAGS ?= -O2 -Wall

all: gpu_stale unrolled_test

gpu_stale: gpu_stale.c gpu_nc.h
	$(CC) $(CFLAGS) -o $@ gpu_stale.c

unrolled.c: gen_unrolled.py
	python3 gen_unrolled.py

unrolled_test: unrolled_test.c unrolled.c unrolled.h gpu_nc.h
	$(CC) $(CFLAGS) -o $@ unrolled_test.c unrolled.c

clean:
	rm -f gpu_stale unrolled_test unrolled.c

.PHONY: all clean
