#!/bin/sh
# check_gain.sh - the firmware's gain loop (gain_ctl.c) on model receivers.
# Run by `make check`. What the model is, and what it is not: sim_gain.c.
#
# Each line is a site (which gain of the model the tuner's highest step
# stands for), a traffic pattern and a starting point, over 12 simulated
# hours, with what must come out: the share of the best fixed gain's
# positions the loop decoded, where it ended, and how often it moved.
cd "$(dirname "$0")/.." || exit 1
M=gain/madrid_model.txt
if [ ! -x ./sim_gain ]; then echo "STOP: ./sim_gain is not built - run make in harness/ first"; exit 1; fi
if [ ! -f "$M" ]; then echo "STOP: $M is missing - make it with: experiments/gain_sweep.py madrid.sigmf-meta --model $M"; exit 1; fi

fail=0
run() {
    out=$(./sim_gain "$M" --hours 12 "$@")
    rc=$?
    echo "$out" | head -1
    if [ $rc -ne 0 ]; then echo "$out" | grep FAIL; echo "  ^ sim_gain $*"; fail=1; fi
}
for seed in 1 2 3; do
    # Too much gain at the top step: come down to where the yield is best.
    run --seed $seed --site 30 --traffic day --min-yield 0.97 --final 19.7 27.0 --max-changes 15
    run --seed $seed --site 21 --traffic day --min-yield 0.97 --final 29.7 38.6 --max-changes 15
    run --seed $seed --site 12 --traffic day --min-yield 0.97 --final 38.6 44.5 --max-changes 15
    run --seed $seed --site 6 --traffic day --min-yield 0.97 --final 44.5 49.6 --max-changes 8
    # The top step is right, or still short: stay there and do not move.
    run --seed $seed --site 0 --traffic day --min-yield 0.99 --final 49.6 49.6 --max-changes 0
    run --seed $seed --site -12 --traffic day --min-yield 0.99 --final 49.6 49.6 --max-changes 0
    # From the lowest step, with hours of no traffic at all in the night.
    run --seed $seed --site 21 --traffic night --start floor --min-yield 0.95 --final 25.4 38.6 --max-changes 15
    run --seed $seed --site -6 --traffic night --start floor --min-yield 0.97 --final 49.6 49.6 --max-changes 10
    # One aircraft in fifty, and one in two hundred.
    run --seed $seed --site 21 --traffic 0.02 --min-yield 0.95 --final 29.7 38.6 --max-changes 20
    run --seed $seed --site 12 --traffic 0.005 --min-yield 0.93 --final 36.4 45.0 --max-changes 30
done
if [ $fail -ne 0 ]; then echo "gain loop: FAIL"; exit 1; fi
echo "gain loop: OK (30 runs of 12 h)"
