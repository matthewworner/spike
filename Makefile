CC     = cc
CFLAGS = -O2 -Wall -Wextra -std=c11 -g

SPIKE_SRCS = pager.c predictor.c tracer.c spike_index.c spike_api.c
SPIKE_HDRS = pager.h predictor.h tracer.h spike_index.h spike_api.h

all: synthetic analyze benchmark pager_test spike_index_test libspike.dylib

synthetic: synthetic.c tracer.c tracer.h
	$(CC) $(CFLAGS) -o synthetic synthetic.c tracer.c -lm

analyze: analyze.c tracer.c tracer.h
	$(CC) $(CFLAGS) -o analyze analyze.c tracer.c -lm

benchmark: benchmark.c predictor.c tracer.c tracer.h predictor.h
	$(CC) $(CFLAGS) -o benchmark benchmark.c predictor.c tracer.c -lm

pager_test: pager_test.c pager.c predictor.c tracer.c spike_index.c pager.h predictor.h tracer.h spike_index.h
	$(CC) $(CFLAGS) -o pager_test pager_test.c pager.c predictor.c tracer.c spike_index.c -lm -lpthread

spike_index_test: spike_index_test.c spike_index.c spike_index.h
	$(CC) $(CFLAGS) -o spike_index_test spike_index_test.c spike_index.c

libspike.dylib: $(SPIKE_SRCS) $(SPIKE_HDRS)
	$(CC) $(CFLAGS) -dynamiclib -o libspike.dylib $(SPIKE_SRCS) -lpthread -lm

clean:
	rm -f synthetic analyze benchmark pager_test spike_index_test *.wptr
	rm -f libspike.dylib
	rm -f /tmp/kernel_ai_test*.bin /tmp/kernel_ai_test*.wptr

.PHONY: all clean
