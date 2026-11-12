#!/usr/bin/env bash
# ==============================================================================
# crash_test.sh - Automated Crash & Reboot Harness for xv6 effect()
#
# Assignment Day 2: Phases 7 & 8
#
# The harness:
# 1. Creates a known starting state: 'crash_file.txt' containing "A".
# 2. Submits an effect with fixed key (0xCAFE0000ULL + point) and append payload "B".
# 3. Injects a crash at a selected write point via crashat(point).
# 4. Reboots xv6 using the exact same fs.img.
# 5. Replays the exact same request: effect(key, EFFECT_APPEND, "crash_file.txt", "B", 1).
# 6. Reads the file and verifies final contents: MUST BE EXACTLY "AB" (length 2).
#    Never "A" (lost write) and never "ABB" (duplicate write).
# 7. Records the crash point and result in crash_report.txt.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LOG_DIR="crash_logs"
REPORT_FILE="crash_report.txt"
FS_CLEAN="fs.img.clean"
TIMEOUT_SECS=6

mkdir -p "$LOG_DIR"

# Detect QEMU binary
if command -v qemu-system-i386 >/dev/null 2>&1; then
    QEMU="qemu-system-i386"
elif command -v qemu-system-x86_64 >/dev/null 2>&1; then
    QEMU="qemu-system-x86_64"
elif command -v qemu >/dev/null 2>&1; then
    QEMU="qemu"
else
    echo "Error: QEMU executable not found!" >&2
    exit 1
fi

# QEMU execution helper using named pipe (FIFO) to ensure zero dropped characters
run_qemu() {
    local cmd="$1"
    local outfile="$2"
    local max_wait="${3:-$TIMEOUT_SECS}"

    rm -f "$outfile"

    local fifo
    fifo="$(mktemp -u /tmp/xv6_fifo.XXXXXX 2>/dev/null || echo "/tmp/xv6_fifo.$RANDOM")"
    mkfifo "$fifo"

    # Start QEMU with stdin attached to FIFO
    "$QEMU" -nographic \
        -drive file=fs.img,index=1,media=disk,format=raw \
        -drive file=xv6.img,index=0,media=disk,format=raw \
        -smp 2 -m 512 < "$fifo" > "$outfile" 2>&1 &
    local qemu_pid=$!

    # Open FIFO for writing asynchronously
    exec 3> "$fifo"

    # Wait for xv6 shell prompt "$ " before sending command
    local waited=0
    while kill -0 "$qemu_pid" 2>/dev/null; do
        if grep -q "\$ " "$outfile" 2>/dev/null; then
            break
        fi
        sleep 0.1
        waited=$((waited + 1))
        if [ "$waited" -ge 50 ]; then
            break
        fi
    done

    # Brief delay after prompt to ensure gets() is waiting, then send command
    sleep 0.1
    echo "$cmd" >&3 2>/dev/null || true

    # Monitor output for completion, prompt, or panic
    local count=0
    while kill -0 "$qemu_pid" 2>/dev/null; do
        if grep -q -E "panic:|CRASH:|CRASH_TEST_RESULT:|INIT: PASS|INIT: FAIL" "$outfile" 2>/dev/null; then
            sleep 0.4
            kill -9 "$qemu_pid" 2>/dev/null || true
            break
        fi
        sleep 0.2
        count=$((count + 1))
        if [ "$count" -ge $((max_wait * 5)) ]; then
            kill -9 "$qemu_pid" 2>/dev/null || true
            break
        fi
    done
    wait "$qemu_pid" 2>/dev/null || true

    # Clean up FIFO
    exec 3>&- 2>/dev/null || true
    rm -f "$fifo" 2>/dev/null || true
}

# Ensure xv6 kernel and clean disk image are built
prepare_clean_fs() {
    if [ ! -f "kernel" ] || [ ! -f "xv6.img" ]; then
        echo "Building xv6 kernel..."
        make -j"$(nproc 2>/dev/null || echo 2)"
    fi

    # Always generate a pristine baseline disk image
    if [ ! -f "$FS_CLEAN" ]; then
        echo "Creating pristine baseline disk image ($FS_CLEAN)..."
        rm -f fs.img
        make fs.img
        cp fs.img "$FS_CLEAN"
    fi
}

# Human-readable description for a given crash point
get_point_description() {
    local pt="$1"
    case "$pt" in
        1) echo "log_write 1 (staging data block in memory)" ;;
        2) echo "log_write 2 (staging inode block in memory)" ;;
        3) echo "log_write 3 (staging effect-table block in memory)" ;;
        4) echo "write_log 1 (writing data block to disk log)" ;;
        5) echo "write_log 2 (writing inode block to disk log)" ;;
        6) echo "write_log 3 (writing effect block to disk log)" ;;
        7|101) echo "commit: before write_head (PRE-COMMIT point)" ;;
        8|102) echo "commit: after write_head (POST-COMMIT / COMMIT POINT)" ;;
        9) echo "install_trans 1 (writing home data block)" ;;
        103) echo "install_trans (named crash point)" ;;
        10) echo "install_trans 2 (writing home inode block)" ;;
        11) echo "install_trans 3 (writing home effect-table block)" ;;
        12|104) echo "commit: after install_trans, before log clear" ;;
        13|105) echo "commit: after log clear write_head" ;;
        *) echo "crash point $pt" ;;
    esac
}

get_expected_phase() {
    local pt="$1"
    if [ "$pt" -le 7 ] || [ "$pt" -eq 101 ]; then
        echo "PRE-COMMIT"
    else
        echo "POST-COMMIT"
    fi
}

# Run a single crash test cycle for a given point
run_crash_cycle() {
    local point="$1"
    local desc
    local phase
    desc="$(get_point_description "$point")"
    phase="$(get_expected_phase "$point")"

    local init_log="$LOG_DIR/crash_point_${point}_init.log"
    local crash_log="$LOG_DIR/crash_point_${point}_run.log"
    local replay_log="$LOG_DIR/crash_point_${point}_replay.log"

    echo "----------------------------------------------------------------------"
    echo "Testing Crash Point $point [$phase]: $desc"
    echo "----------------------------------------------------------------------"

    # Step 1: Create known starting state (file containing "A")
    cp "$FS_CLEAN" fs.img
    run_qemu "crashtest init" "$init_log" 5

    if ! grep -q "INIT: PASS" "$init_log"; then
        echo "  [ERROR] Failed to initialize starting state!"
        cat "$init_log"
        return 1
    fi
    echo "  Step 1: Initial state verified: 'crash_file.txt' contains 'A'."

    # Steps 2 & 3: Submit effect append "B" with crash injection
    echo "  Step 2 & 3: Injecting crash at point $point..."
    local crash_triggered=0
    run_qemu "crashtest run $point" "$crash_log" 5

    if grep -q "CRASH:" "$crash_log" || grep -q "panic: crashat" "$crash_log"; then
        local crash_msg
        crash_msg="$(grep -o "CRASH:.*" "$crash_log" | head -n 1 || echo "panic: crashat")"
        echo "  -> Crash triggered successfully: $crash_msg"
        crash_triggered=1
    else
        echo "  [WARNING] Point $point did not trigger panic (transaction may have finished)."
    fi

    # Steps 4, 5, 6: Reboot xv6 using SAME fs.img, replay request, verify contents
    echo "  Step 4 & 5: Rebooting xv6 with same fs.img and replaying request..."
    run_qemu "crashtest replay $point" "$replay_log" 5

    local replay_ret="UNKNOWN"
    local final_len="UNKNOWN"
    local final_content="UNKNOWN"
    local status="FAIL"

    if grep -q "REPLAY_RET:" "$replay_log"; then
        replay_ret="$(grep "REPLAY_RET:" "$replay_log" | awk '{print $2}' | tr -d '\r')"
    fi
    if grep -q "FINAL_LEN:" "$replay_log"; then
        final_len="$(grep "FINAL_LEN:" "$replay_log" | awk '{print $2}' | tr -d '\r')"
    fi
    if grep -q "FINAL_CONTENT:" "$replay_log"; then
        final_content="$(grep "FINAL_CONTENT:" "$replay_log" | awk '{print $2}' | tr -d '\r')"
    fi

    if [ "$crash_triggered" -eq 1 ] && grep -q "CRASH_TEST_RESULT: PASS" "$replay_log"; then
        status="PASS"
        echo "  Step 6: Verified final content: '$final_content' (len=$final_len, replay_ret=$replay_ret)."
        echo "  -> RESULT: PASS (Exactly once 'AB', never 'A' and never 'ABB')"
    else
        status="FAIL"
        echo "  Step 6: VERIFICATION FAILED! Content='$final_content', len=$final_len."
        echo "  -> RESULT: FAIL"
        cat "$replay_log"
    fi

    # Step 7: Record result in report
    printf "%-6s | %-12s | %-8s | %-13s | %-5s | %s\n" \
        "$point" "$phase" "$status" "$final_content (len=$final_len)" "$replay_ret" "$desc" >> "$REPORT_FILE"

    if [ "$status" = "PASS" ]; then
        return 0
    else
        return 1
    fi
}

# Run automated sweep across all crash points
run_sweep() {
    prepare_clean_fs

    echo "======================================================================"
    echo "       XV6 EFFECT() TRANSACTION CRASH & RECOVERY TEST HARNESS        "
    echo "======================================================================"
    echo ""

    # Initialize report file header
    {
        echo "================================================================================================"
        echo "                           XV6 CRASH RECOVERY SWEEP REPORT                                      "
        echo "================================================================================================"
        echo "Date: $(date)"
        echo "Initial State: crash_file.txt containing 'A'"
        echo "Operation: effect(0xCAFE0000ULL + point, EFFECT_APPEND, 'crash_file.txt', 'B', 1)"
        echo "Expected Outcome: Exactly 'AB' after reboot & replay for every crash point"
        echo "------------------------------------------------------------------------------------------------"
        printf "%-6s | %-12s | %-8s | %-13s | %-5s | %s\n" \
            "POINT" "PHASE" "RESULT" "FINAL CONTENT" "RET" "CRASH LOCATION"
        echo "------------------------------------------------------------------------------------------------"
    } > "$REPORT_FILE"

    # Sweep all relevant crash points:
    # 1-3: staging in log_write
    # 4-6: writing blocks in write_log
    # 7: before write_head (pre-commit)
    # 8: after write_head (commit point)
    # 9-11: installing home blocks
    # 12: after home install, before clear
    # 13: after log clear
    # 101: explicit named PRE-COMMIT
    # 102: explicit named POST-COMMIT
    local points=(1 2 3 4 5 6 7 8 9 10 11 12 13 101 102 103 104 105)

    local passed=0
    local total=${#points[@]}

    for pt in "${points[@]}"; do
        if run_crash_cycle "$pt"; then
            passed=$((passed + 1))
        fi
        echo ""
    done

    echo "======================================================================"
    echo "                         FINAL SWEEP RESULTS                          "
    echo "======================================================================"
    cat "$REPORT_FILE"
    echo "----------------------------------------------------------------------"
    echo "Summary: $passed / $total crash points passed."
    if [ "$passed" -eq "$total" ]; then
        echo "ALL CRASH POINTS PASSED! Exactly-once semantics verified across crashes."
    else
        echo "SOME CRASH POINTS FAILED. Check $REPORT_FILE and logs in $LOG_DIR/ for details."
    fi
    echo "======================================================================"
}

# Entrypoint: handle arguments
if [ $# -eq 0 ] || [ "$1" = "sweep" ] || [ "$1" = "all" ]; then
    run_sweep
else
    prepare_clean_fs
    # Initialize report file header if not existing
    if [ ! -f "$REPORT_FILE" ]; then
        printf "%-6s | %-12s | %-8s | %-13s | %-5s | %s\n" \
            "POINT" "PHASE" "RESULT" "FINAL CONTENT" "RET" "CRASH LOCATION" > "$REPORT_FILE"
    fi
    run_crash_cycle "$1"
fi
