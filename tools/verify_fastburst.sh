#!/bin/bash
# CI / hardware check for the fast-burst capture structure (FSD v2.56/v2.59).
# There is no unit-test harness in this project (verification is empirical on
# real hardware, see CLAUDE.md), so this drives the live device: it waits for the
# next motion event and asserts, from the device's own counters (lastFast /
# lastFrames in /api/status), that the event captured 4 fast-burst frames plus
# 1..capture_count slow ones.
#
#   Usage:  tools/verify_fastburst.sh [DEVICE_IP] [TIMEOUT_S]
#   Exit:   0 = a motion event with the expected 4-fast + N-slow structure
#           1 = a mismatch (wrong fast/slow split)
#           2 = no motion event within the timeout (inconclusive, not a failure)
set -u
IP="${1:-192.168.1.111}"
TIMEOUT="${2:-1800}"
EXPECT_FAST=4          # FAST_BURST_N in classify.h

jq_get(){ sed -n "s/.*\"$1\":\([0-9-]*\).*/\1/p"; }   # tiny extractor, no jq dependency

base=$(curl -s -m 8 "http://$IP/api/status" | jq_get events)
[ -z "$base" ] && { echo "FAIL: device $IP unreachable"; exit 1; }
echo "waiting up to ${TIMEOUT}s for a motion event on $IP (current count $base)..."

deadline=$(( $(date +%s) + TIMEOUT ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  s=$(curl -s -m 8 "http://$IP/api/status")
  ev=$(printf '%s' "$s" | jq_get events)
  if [ -n "$ev" ] && [ "$ev" -gt "$base" ]; then
    fast=$(printf '%s' "$s"  | jq_get lastFast)
    frames=$(printf '%s' "$s"| jq_get lastFrames)
    slow=$(( frames - fast ))
    echo "event captured: lastFrames=$frames  lastFast=$fast  (=> slow=$slow)"
    if [ "$fast" -eq "$EXPECT_FAST" ] && [ "$slow" -ge 1 ]; then
      echo "PASS: $EXPECT_FAST fast + $slow slow frame(s) — fast-burst structure OK"
      exit 0
    fi
    # A capture that lost SD writes can shorten either group; only a wrong fast
    # count (not a short slow tail from an early-leaving bird) is a real failure.
    echo "FAIL: expected $EXPECT_FAST fast frames, got $fast (frames=$frames)"
    exit 1
  fi
  sleep 5
done
echo "INCONCLUSIVE: no motion event within ${TIMEOUT}s"
exit 2
