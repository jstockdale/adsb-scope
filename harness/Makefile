# harness/Makefile - host builds of the firmware decoder and its checks.
#   make          build the three decoder variants + unit test
#   make check    table proofs, C-level fixer tests, stream-equivalence test,
#                 (including samples lost mid-stream), MQTT feeder batch
#                 tests, sample ring tests (two threads), the gain loop on
#                 model receivers (gain/check_gain.sh), number text and
#                 distance (test_fmt), the console-speed state machine
#                 (test_link), and - against a simulated device on a
#                 pseudo-terminal - the logger (link/check_fastlog.py, 2 min)
#                 and the web app's own serial code (link/check_page.js,
#                 4 min; needs node 18+, skipped with a notice without it)
#   make check-quick   everything in check except the two that take minutes
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
BINS   := host_mode_s host_mode_s_plain host_mode_s_df18off host_mode_s_baseline test_fix_c test_batch test_ring sim_gain test_fmt test_link sim_device

all: $(BINS)

$(APP)/mode_s_syndromes.h: gen_syndrome_tables.py
	$(PYTHON) gen_syndrome_tables.py

host_mode_s: host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(APP)/mode-s.h $(APP)/mode_s_syndromes.h
	$(CC) $(CFLAGS) -I$(APP) -o $@ host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(LDLIBS)

# The 2.4 Msps demodulator with its chip sums in the plain form (loops over
# the position tables) instead of written out: check_stream.py holds the two
# to identical output.
host_mode_s_plain: host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(APP)/mode-s.h $(APP)/mode_s_syndromes.h
	$(CC) $(CFLAGS) -I$(APP) -DMODE_S_2400_PLAIN -o $@ host_mode_s.c $(APP)/mode-s.c $(APP)/mode_s_2400.c $(APP)/mode_s_coh.c $(LDLIBS)

# The firmware's gain loop against a model receiver (gain/README in sim_gain.c)
sim_gain: gain/sim_gain.c $(APP)/gain_ctl.c $(APP)/gain_ctl.h
	$(CC) $(CFLAGS) -Wextra -I$(APP) -o $@ gain/sim_gain.c $(APP)/gain_ctl.c $(LDLIBS)

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

# Number text and great-circle distance without double arithmetic (adsb_fmt.c)
test_fmt: fmt/test_fmt.c $(APP)/adsb_fmt.c $(APP)/adsb_fmt.h
	$(CC) $(CFLAGS) -Wextra -I$(APP) -o $@ fmt/test_fmt.c $(APP)/adsb_fmt.c $(LDLIBS)

# The console's speed as a session property (console_link.c): its unit test,
# and a stand-in for the device on a pseudo-terminal that runs the same code
test_link: link/test_link.c $(APP)/console_link.c $(APP)/console_link.h
	$(CC) $(CFLAGS) -Wextra -I$(APP) -o $@ link/test_link.c $(APP)/console_link.c

sim_device: link/sim_device.c $(APP)/console_link.c $(APP)/console_link.h
	$(CC) $(CFLAGS) -Wextra -I$(APP) -o $@ link/sim_device.c $(APP)/console_link.c

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
	sh gain/check_gain.sh
	./test_fmt
	./test_link
	$(PYTHON) link/check_fastlog.py
	@if command -v node >/dev/null 2>&1; then node link/check_page.js; \
	 else echo "SKIPPED: link/check_page.js (the web app's serial code against the simulated device) - node 18 or later is not installed"; fi

check-quick: all
	$(PYTHON) gen_syndrome_tables.py /tmp/mode_s_syndromes.check.h
	cmp /tmp/mode_s_syndromes.check.h $(APP)/mode_s_syndromes.h
	./test_fix_c
	$(PYTHON) check_stream.py
	$(PYTHON) feeder/check_feeder.py ./test_batch
	./test_ring
	sh gain/check_gain.sh
	./test_fmt
	./test_link

bench: all
	$(PYTHON) bench_fix.py

clean:
	rm -f $(BINS) old_batch_size *.so

.PHONY: all check check-quick bench clean
