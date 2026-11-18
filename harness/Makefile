# harness/Makefile - host builds of the firmware decoder and its checks.
#   make          build the three decoder variants + unit test
#   make check    table proofs, C-level fixer tests, stream-equivalence test,
#                 (including samples lost mid-stream), MQTT feeder batch
#                 tests, sample ring tests (two threads)
#   make old_batch_size IDF_PATH=...   size of the pre-r7 feeder's batches,
#                 built with ESP-IDF's own cJSON (optional, not part of check)
#   make bench    fixer micro-benchmark (historical brute force vs syndrome)
APP    := ../main/examples/adsb_receiver
CC     ?= gcc
# Needs numpy (and scipy for the experiments). In a shell where ESP-IDF's
# export.sh has been sourced, python3 is the IDF interpreter, which normally
# lacks them: use  make check PYTHON=/usr/bin/python3  or a fresh shell.
PYTHON ?= python3
CFLAGS := -std=gnu17 -O2 -Wall
LDLIBS := -lm
BINS   := host_mode_s host_mode_s_df18off host_mode_s_baseline test_fix_c test_batch test_ring

all: $(BINS)

$(APP)/mode_s_syndromes.h: gen_syndrome_tables.py
	$(PYTHON) gen_syndrome_tables.py

host_mode_s: host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(APP)/mode-s.h $(APP)/mode_s_syndromes.h
	$(CC) $(CFLAGS) -I$(APP) -o $@ host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(LDLIBS)

host_mode_s_df18off: host_mode_s.c $(APP)/mode-s.c $(APP)/mode-s.h $(APP)/mode_s_syndromes.h
	$(CC) $(CFLAGS) -I$(APP) -DMODE_S_DF18_FIX=0 -o $@ host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(LDLIBS)

host_mode_s_baseline: host_mode_s.c baseline/mode-s.c baseline/mode-s.h
	$(CC) $(CFLAGS) -include time.h -Ibaseline -DBASELINE -o $@ host_mode_s.c baseline/mode-s.c $(LDLIBS)

test_fix_c: test_fix_c.c $(APP)/mode-s.c $(APP)/mode-s.h $(APP)/mode_s_syndromes.h
	$(CC) $(CFLAGS) -I$(APP) -o $@ test_fix_c.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(LDLIBS)

test_batch: feeder/test_batch.c $(APP)/mqtt_batch.c $(APP)/mqtt_batch.h $(APP)/mqtt_feeder.h
	$(CC) $(CFLAGS) -Wextra -I$(APP) -o $@ feeder/test_batch.c $(APP)/mqtt_batch.c $(LDLIBS)

test_ring: ring/test_ring.c $(APP)/sample_ring.c $(APP)/sample_ring.h
	$(CC) $(CFLAGS) -Wextra -pthread -I$(APP) -o $@ ring/test_ring.c $(APP)/sample_ring.c $(LDLIBS)

CJSON := $(IDF_PATH)/components/json/cJSON
old_batch_size: feeder/old_batch_size.c $(APP)/mqtt_batch.c $(APP)/mqtt_batch.h
	@test -n "$(IDF_PATH)" || { echo "STOP: IDF_PATH is not set. Use: make old_batch_size IDF_PATH=/path/to/esp-idf"; exit 1; }
	@test -f "$(CJSON)/cJSON.c" || { echo "STOP: $(CJSON)/cJSON.c not found - IDF_PATH must point at an ESP-IDF v5.4.1 checkout"; exit 1; }
	$(CC) $(CFLAGS) -I$(APP) -I$(CJSON) -o $@ feeder/old_batch_size.c $(APP)/mqtt_batch.c $(CJSON)/cJSON.c $(LDLIBS)

check: all
	$(PYTHON) gen_syndrome_tables.py /tmp/mode_s_syndromes.check.h
	cmp /tmp/mode_s_syndromes.check.h $(APP)/mode_s_syndromes.h
	./test_fix_c
	$(PYTHON) check_stream.py
	$(PYTHON) feeder/check_feeder.py ./test_batch
	./test_ring

bench: all
	$(PYTHON) bench_fix.py

clean:
	rm -f $(BINS) old_batch_size *.so

.PHONY: all check bench clean
