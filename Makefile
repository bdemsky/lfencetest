CFLAGS ?= -O2 -Wall

all: lfence_test cxl_test cxl_litmus

lfence_test: lfence_test.c gpu_nc.h
	$(CC) $(CFLAGS) -o $@ lfence_test.c

cxl_test: cxl_test.c gpu_nc.h
	$(CC) $(CFLAGS) -o $@ cxl_test.c

cxl_litmus: cxl_litmus.c gpu_nc.h
	$(CC) $(CFLAGS) -pthread -o $@ cxl_litmus.c

clean:
	rm -f lfence_test cxl_test cxl_litmus

.PHONY: all clean
