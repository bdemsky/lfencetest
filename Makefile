CFLAGS ?= -O2 -Wall

lfence_test: lfence_test.c gpu_nc.h
	$(CC) $(CFLAGS) -o $@ lfence_test.c

clean:
	rm -f lfence_test

.PHONY: clean
