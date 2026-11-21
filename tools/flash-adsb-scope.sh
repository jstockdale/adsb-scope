#!/usr/bin/env bash
# flash-adsb-scope.sh - install an ADS-B Scope firmware kit on a T-Display-P4
# with esptool, keeping the NVS (settings, Meshtastic identity and keys)
# whenever the device's flash layout allows it.
#
#   bash flash-adsb-scope.sh                 # check the device, then install
#   bash flash-adsb-scope.sh --dry-run       # check and say what it would do; write nothing
#   bash flash-adsb-scope.sh --port /dev/ttyACM1 --build plain
#
# What it does, in order:
#   1. finds the unpacked firmware kit and esptool, and checks the kit's checksums
#   2. reads the first 64 kB of the device's flash (boot loader, partition
#      table, NVS) and keeps it as a backup under ~/adsb-scope-backups/
#   3. compares the device's partition table with the kit's:
#        same layout            -> writes the application only (0x10000)
#        other layout, NVS at   -> writes boot loader, partition table and
#        the same place            application as three pieces; the NVS
#                                  between them is not touched
#        NVS elsewhere / none   -> the merged image from 0x0, which resets
#                                  the NVS - only after you type YES
#   4. reads the NVS back and compares it with the backup, then resets the
#      device into the new firmware
#
# Nothing is written before step 3 has decided, and every step that fails
# stops the run: it never reports a flash that did not happen.
# Needs: bash, esptool (v4 or v5), and the usual od / head / tail / cmp.

# ---- edit these, or pass the options below ---------------------------------
PORT="/dev/ttyACM0"     # serial port of the T-Display-P4            (--port)
BUILD="default"         # "default" or "plain"                        (--build)
BAUD=460800             # try 115200 if the transfer fails            (--baud)
NVS="keep"              # "keep", or "reset" for a clean install      (--nvs)
KIT=""                  # unpacked kit folder; empty = look for it    (--kit)
KIT_NAME="adsb-scope-fw-261118-r10-coh6"   # the folder / tarball looked for
# -----------------------------------------------------------------------------

adsb_flash_say()  { printf '%s\n' "$*"; }
adsb_flash_stop() { printf '\nSTOP: %s\n' "$*" >&2; }

# Hex bytes of a file as words, one per byte.
adsb_flash_hex() { od -An -v -tx1 "$1" | tr -s ' \n' ' '; }

# Print a partition table file and set PT_COUNT, PT_NVS_OFF, PT_NVS_SIZE (decimal; -1 = no NVS).
adsb_flash_read_table() {
    local -a b
    local i o type sub off size label j c
    read -r -a b <<< "$(adsb_flash_hex "$1")"
    PT_COUNT=0; PT_NVS_OFF=-1; PT_NVS_SIZE=-1
    for ((i = 0; i + 32 <= ${#b[@]}; i += 32)); do
        [ "${b[i]}${b[i+1]}" = "aa50" ] || break
        type=$((16#${b[i+2]})); sub=$((16#${b[i+3]}))
        off=$(( 16#${b[i+7]}${b[i+6]}${b[i+5]}${b[i+4]} ))
        size=$(( 16#${b[i+11]}${b[i+10]}${b[i+9]}${b[i+8]} ))
        label=""
        for ((j = 12; j < 28; j++)); do
            c=${b[i+j]}
            [ "$c" = "00" ] && break
            o=$(printf '%o' "$((16#$c))")
            label+=$(printf "\\$o")
        done
        printf '      %-10s type %02x/%02x  at 0x%06x  size 0x%06x\n' "$label" "$type" "$sub" "$off" "$size"
        if [ "$type" -eq 1 ] && [ "$sub" -eq 2 ] && [ "$PT_NVS_OFF" -lt 0 ]; then
            PT_NVS_OFF=$off; PT_NVS_SIZE=$size
        fi
        PT_COUNT=$((PT_COUNT + 1))
    done
    [ "$PT_COUNT" -gt 0 ] || adsb_flash_say "      (no partition table)"
}

# Bytes [offset, offset+length) of a file to another file.
adsb_flash_slice() { tail -c "+$(( $2 + 1 ))" "$1" | head -c "$3" > "$4"; }

adsb_flash_main() {
    local port="$PORT" build="$BUILD" baud="$BAUD" nvs="$NVS" kit="$KIT" kit_name="$KIT_NAME"
    local dry=0 yes=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --port)  port="$2"; shift 2 ;;
            --build) build="$2"; shift 2 ;;
            --baud)  baud="$2"; shift 2 ;;
            --nvs)   nvs="$2"; shift 2 ;;
            --kit)   kit="$2"; shift 2 ;;
            --dry-run) dry=1; shift ;;
            --yes)   yes=1; shift ;;
            -h|--help)
                adsb_flash_say "usage: bash flash-adsb-scope.sh [--port DEV] [--build default|plain] [--baud N]"
                adsb_flash_say "                                [--nvs keep|reset] [--kit DIR] [--dry-run] [--yes]"
                return 0 ;;
            *) adsb_flash_stop "unknown option '$1' (--help lists them)"; return 2 ;;
        esac
    done
    case "$build" in default|plain) ;; *) adsb_flash_stop "BUILD must be 'default' or 'plain', not '$build'"; return 2 ;; esac
    case "$nvs" in keep|reset) ;; *) adsb_flash_stop "NVS must be 'keep' or 'reset', not '$nvs'"; return 2 ;; esac

    # ---- 1. tools ------------------------------------------------------------
    local t
    for t in od tr head tail cmp mktemp date; do
        command -v "$t" >/dev/null 2>&1 || { adsb_flash_stop "'$t' not found - it is part of coreutils on every Linux and macOS"; return 1; }
    done

    # esptool: on the PATH under either name, as a module of the current
    # Python, or in ESP-IDF's own Python environment.
    local -a esptool=() cand
    local c found_idf
    for c in "esptool" "esptool.py" "python3 -m esptool" "python -m esptool"; do
        read -r -a cand <<< "$c"
        if "${cand[@]}" version >/dev/null 2>&1; then esptool=("${cand[@]}"); break; fi
    done
    if [ ${#esptool[@]} -eq 0 ]; then
        for found_idf in "${IDF_PYTHON_ENV_PATH:-/nonexistent}/bin/python" "$HOME"/.espressif/python_env/*/bin/python; do
            if [ -x "$found_idf" ] && "$found_idf" -m esptool version >/dev/null 2>&1; then
                esptool=("$found_idf" -m esptool); break
            fi
        done
    fi
    if [ ${#esptool[@]} -eq 0 ]; then
        adsb_flash_stop "esptool not found. Install it into the Python you are using:   pip install esptool
      (or run this in a shell where ESP-IDF's export.sh has been sourced)"
        return 1
    fi
    local ver major
    ver=$("${esptool[@]}" version 2>/dev/null | grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' | tail -1)
    major=${ver%%.*}
    case "$major" in ''|*[!0-9]*) major=4 ;; esac
    # esptool 5 spells commands and option values with hyphens; 4 with underscores.
    local cmd_read="read_flash" cmd_write="write_flash" no_reset="no_reset"
    if [ "$major" -ge 5 ]; then cmd_read="read-flash"; cmd_write="write-flash"; no_reset="no-reset"; fi
    adsb_flash_say "esptool:  ${esptool[*]}  (version ${ver:-unknown})"

    # ---- 1b. the kit ------------------------------------------------------------
    local here="" d tb
    [ -n "${BASH_SOURCE[0]:-}" ] && [ -f "${BASH_SOURCE[0]}" ] && here=$(cd "$(dirname "${BASH_SOURCE[0]}")" 2>/dev/null && pwd)
    if [ -z "$kit" ]; then
        for d in "$here" "$PWD" "$here/$kit_name" "$PWD/$kit_name" "$HOME/Downloads/$kit_name" "$HOME/$kit_name"; do
            if [ -n "$d" ] && [ -f "$d/default/adsb-scope.bin" ] && [ -f "$d/SHA256SUMS" ]; then kit="$d"; break; fi
        done
    fi
    if [ -z "$kit" ]; then
        for d in "$here" "$PWD" "$HOME/Downloads" "$HOME"; do
            tb="$d/$kit_name.tar.gz"
            if [ -n "$d" ] && [ -f "$tb" ]; then
                adsb_flash_say "unpacking $tb"
                tar xzf "$tb" -C "$d" || { adsb_flash_stop "could not unpack $tb"; return 1; }
                kit="$d/$kit_name"; break
            fi
        done
    fi
    if [ -z "$kit" ] || [ ! -d "$kit" ]; then
        adsb_flash_stop "firmware kit not found. Looked for the folder $kit_name (or $kit_name.tar.gz)
      next to this script, in the current folder, in ~/Downloads and in ~.
      Say where it is:   bash flash-adsb-scope.sh --kit /path/to/$kit_name"
        return 1
    fi
    kit=$(cd "$kit" && pwd) || { adsb_flash_stop "cannot enter $kit"; return 1; }
    local app="$kit/$build/adsb-scope.bin" bl="$kit/$build/bootloader.bin" pt="$kit/$build/partition-table.bin" merged=""
    for d in "$kit/$build"/jstockdale-adsb_receiver-t_display_p4-*.bin; do [ -f "$d" ] && merged="$d"; done
    for d in "$app" "$bl" "$pt" "$kit/SHA256SUMS"; do
        [ -f "$d" ] || { adsb_flash_stop "$d is missing - is $kit a complete, unpacked firmware kit?"; return 1; }
    done
    [ -n "$merged" ] || { adsb_flash_stop "no merged image (jstockdale-adsb_receiver-t_display_p4-*.bin) in $kit/$build"; return 1; }
    adsb_flash_say "kit:      $kit  ($build)"
    if command -v sha256sum >/dev/null 2>&1; then
        (cd "$kit" && sha256sum -c SHA256SUMS --quiet) || { adsb_flash_stop "the kit's checksums do not match - unpack the tarball again"; return 1; }
    elif command -v shasum >/dev/null 2>&1; then
        (cd "$kit" && shasum -a 256 -c SHA256SUMS >/dev/null) || { adsb_flash_stop "the kit's checksums do not match - unpack the tarball again"; return 1; }
    else
        adsb_flash_say "note:     no sha256sum here - the kit's checksums were not verified"
    fi

    # ---- 1c. the port ------------------------------------------------------------
    if [ ! -e "$port" ]; then
        local others
        others=$(ls /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial* 2>/dev/null | tr '\n' ' ')
        adsb_flash_stop "serial port $port not found.  Ports here: ${others:-none}
      Pick one with:   bash flash-adsb-scope.sh --port /dev/ttyACM1"
        return 1
    fi
    if [ ! -r "$port" ] || [ ! -w "$port" ]; then
        adsb_flash_stop "no permission to use $port.  On Linux:   sudo usermod -aG dialout \$USER   (then log out and in)"
        return 1
    fi
    adsb_flash_say "port:     $port at $baud baud"

    # ---- 2. read the device ---------------------------------------------------
    local tmp log
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/adsb-flash.XXXXXX") || { adsb_flash_stop "cannot create a temporary folder"; return 1; }
    log="$tmp/esptool.log"
    adsb_flash_say ""
    adsb_flash_say "== reading the first 64 kB of the device's flash =="
    PYTHONUNBUFFERED=1 "${esptool[@]}" --chip esp32p4 -p "$port" -b "$baud" --after "$no_reset" "$cmd_read" 0x0 0x10000 "$tmp/first-64k.bin" 2>&1 | tee "$log"
    if [ "${PIPESTATUS[0]}" -ne 0 ] || [ ! -s "$tmp/first-64k.bin" ]; then
        adsb_flash_stop "could not read the device on $port. Nothing was written.
      If it never connects: hold BOOT, tap RESET, release BOOT, and run this again.
      If it connects and then fails: try   --baud 115200"
        rm -rf "$tmp"; return 1
    fi
    if [ "$(wc -c < "$tmp/first-64k.bin")" -ne 65536 ]; then
        adsb_flash_stop "the read returned $(wc -c < "$tmp/first-64k.bin") bytes instead of 65536. Nothing was written."
        rm -rf "$tmp"; return 1
    fi
    local mac backup
    mac=$(grep -Eio '([0-9a-f]{2}:){5}[0-9a-f]{2}' "$log" | head -1 | tr -d ':' | tr 'A-F' 'a-f')
    backup="$HOME/adsb-scope-backups/${mac:-unknown}-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$backup" && cp "$tmp/first-64k.bin" "$backup/first-64k.bin" || { adsb_flash_stop "cannot write the backup to $backup. Nothing was written to the device."; rm -rf "$tmp"; return 1; }
    adsb_flash_slice "$tmp/first-64k.bin" $((0x8000)) $((0xC00)) "$tmp/dev-table.bin"
    adsb_flash_say ""
    adsb_flash_say "device:   MAC ${mac:-unknown}"
    adsb_flash_say "backup:   $backup/first-64k.bin  (the first 64 kB as they were)"

    # ---- 3. decide ------------------------------------------------------------
    local want_off want_size
    adsb_flash_say ""
    adsb_flash_say "   the kit's partition table:"
    adsb_flash_read_table "$pt"; want_off=$PT_NVS_OFF; want_size=$PT_NVS_SIZE
    adsb_flash_say "   the device's partition table:"
    adsb_flash_read_table "$tmp/dev-table.bin"
    if [ "$want_off" -ne $((0x9000)) ] || [ "$want_size" -ne $((0x6000)) ]; then
        adsb_flash_stop "this kit's NVS is not at 0x9000 / 0x6000, which this script was written for. Nothing was written."
        rm -rf "$tmp"; return 1
    fi

    # The device's NVS, as a file of its own: from the 64 kB already read if
    # it lies in there, with one more read if it does not.
    local nvs_file=""
    if [ "$PT_NVS_OFF" -ge 0 ]; then
        nvs_file=$(printf '%s/nvs-0x%x.bin' "$backup" "$PT_NVS_OFF")
        if [ $((PT_NVS_OFF + PT_NVS_SIZE)) -le 65536 ]; then
            adsb_flash_slice "$tmp/first-64k.bin" "$PT_NVS_OFF" "$PT_NVS_SIZE" "$nvs_file"
        else
            adsb_flash_say ""
            adsb_flash_say "== the device's NVS lies beyond the first 64 kB: reading it for the backup =="
            if ! "${esptool[@]}" --chip esp32p4 -p "$port" -b "$baud" --after "$no_reset" "$cmd_read" "$(printf '0x%x' "$PT_NVS_OFF")" "$(printf '0x%x' "$PT_NVS_SIZE")" "$nvs_file"; then
                adsb_flash_stop "could not read the device's NVS for the backup. Nothing was written."
                rm -rf "$tmp"; return 1
            fi
        fi
        adsb_flash_say "backup:   $nvs_file  (the NVS partition alone)"
    fi

    local plan why
    if [ "$nvs" = "reset" ]; then
        plan="merged"; why="a clean install was asked for (NVS=reset)"
    elif cmp -s "$tmp/dev-table.bin" "$pt"; then
        plan="app"; why="the device already has the kit's layout"
    elif [ "$PT_COUNT" -eq 0 ]; then
        plan="merged"; why="the device has no partition table: nothing to keep"
    elif [ "$PT_NVS_OFF" -eq "$want_off" ] && [ "$PT_NVS_SIZE" -eq "$want_size" ]; then
        plan="parts"; why="the layout differs, but the NVS is where the kit's is"
    else
        plan="merged"; why="the device's NVS is not where the kit's is: it cannot be kept"
    fi

    adsb_flash_say ""
    case "$plan" in
        app)    adsb_flash_say "PLAN:  write the application only, at 0x10000 - $why."
                adsb_flash_say "       NVS (settings, Meshtastic identity), boot loader and partition table stay as they are." ;;
        parts)  adsb_flash_say "PLAN:  write boot loader (0x2000), partition table (0x8000) and application (0x10000)"
                adsb_flash_say "       as three pieces - $why."
                adsb_flash_say "       The NVS at 0x9000-0xefff is not touched: settings and Meshtastic identity are kept."
                adsb_flash_say "       (Settings saved by a newer settings version than this firmware knows fall back to defaults.)" ;;
        merged) adsb_flash_say "PLAN:  write the merged image from 0x0 - $why."
                adsb_flash_say "       THIS RESETS THE NVS: settings go back to defaults and the device gets a new"
                adsb_flash_say "       Meshtastic identity and key pair.${nvs_file:+ The old NVS is in the backup above.}" ;;
    esac

    if [ "$dry" -eq 1 ]; then
        adsb_flash_say ""
        adsb_flash_say "dry run: nothing was written. Run again without --dry-run to install."
        rm -rf "$tmp"; return 0
    fi
    if [ "$plan" = "merged" ] && [ "$PT_COUNT" -gt 0 ] && [ "$yes" -ne 1 ]; then
        local answer=""
        if ! { : < /dev/tty; } 2>/dev/null; then
            adsb_flash_stop "this needs a yes, and there is no terminal to ask on. Nothing was written.
      Run it again with   --yes   to reset the NVS and write the merged image."
            rm -rf "$tmp"; return 1
        fi
        printf '\nType YES to reset the NVS and write the merged image, anything else to stop: '
        read -r answer < /dev/tty || answer=""
        if [ "$answer" != "YES" ]; then
            adsb_flash_stop "stopped: the answer was not YES. Nothing was written."
            rm -rf "$tmp"; return 1
        fi
    fi

    # ---- 4. write ---------------------------------------------------------------
    adsb_flash_say ""
    adsb_flash_say "== writing =="
    local rc
    case "$plan" in
        app)    "${esptool[@]}" --chip esp32p4 -p "$port" -b "$baud" --after "$no_reset" "$cmd_write" 0x10000 "$app"; rc=$? ;;
        parts)  "${esptool[@]}" --chip esp32p4 -p "$port" -b "$baud" --after "$no_reset" "$cmd_write" 0x2000 "$bl" 0x8000 "$pt" 0x10000 "$app"; rc=$? ;;
        merged) "${esptool[@]}" --chip esp32p4 -p "$port" -b "$baud" "$cmd_write" 0x0 "$merged"; rc=$? ;;
    esac
    if [ "$rc" -ne 0 ]; then
        adsb_flash_stop "esptool failed while writing (exit status $rc). THE DEVICE MAY BE HALF-WRITTEN.
      Run this script again; if the transfer keeps failing, add   --baud 115200
      The first 64 kB as they were before this run are in $backup/first-64k.bin"
        rm -rf "$tmp"; return 1
    fi

    # ---- 5. check, and start the new firmware ------------------------------------
    if [ "$plan" = "merged" ]; then
        adsb_flash_say ""
        adsb_flash_say "DONE: $(basename "$merged") written from 0x0; the NVS was reset. The device has restarted."
        rm -rf "$tmp"; return 0
    fi
    adsb_flash_say ""
    adsb_flash_say "== reading the NVS back, then restarting the device =="
    "${esptool[@]}" --chip esp32p4 -p "$port" -b "$baud" "$cmd_read" 0x8000 0x7000 "$tmp/after.bin"; rc=$?
    if [ "$rc" -ne 0 ] || [ "$(wc -c < "$tmp/after.bin" 2>/dev/null || echo 0)" -ne $((0x7000)) ]; then
        adsb_flash_say ""
        adsb_flash_say "WRITTEN, NOT CHECKED: esptool wrote and verified the firmware, but the read-back that"
        adsb_flash_say "compares the NVS with the backup failed, and the device may not have restarted."
        adsb_flash_say "Press RESET on the device. The NVS before this run is in $nvs_file"
        rm -rf "$tmp"; return 1
    fi
    adsb_flash_slice "$tmp/after.bin" 0 $((0xC00)) "$tmp/after-table.bin"
    adsb_flash_slice "$tmp/after.bin" $((0x1000)) $((0x6000)) "$tmp/after-nvs.bin"
    local ok=1
    cmp -s "$tmp/after-table.bin" "$pt" || { ok=0; adsb_flash_say "PROBLEM: the partition table on the device is not the kit's."; }
    cmp -s "$tmp/after-nvs.bin" "$nvs_file" || { ok=0; adsb_flash_say "PROBLEM: the NVS changed during the flash."; }
    rm -rf "$tmp"
    adsb_flash_say ""
    if [ "$ok" -ne 1 ]; then
        adsb_flash_say "The firmware was written, but the check above did not pass. To put the NVS back:"
        adsb_flash_say "   ${esptool[*]} --chip esp32p4 -p $port -b $baud $cmd_write 0x9000 $nvs_file"
        return 1
    fi
    if [ "$plan" = "app" ]; then
        adsb_flash_say "DONE: $build/adsb-scope.bin written at 0x10000."
    else
        adsb_flash_say "DONE: boot loader, partition table and $build/adsb-scope.bin written."
    fi
    adsb_flash_say "      Partition table is the kit's; the NVS is byte for byte what it was. The device has restarted."
    return 0
}

if [ -z "${BASH_VERSION:-}" ]; then
    echo "STOP: this script needs bash. Run it as:   bash flash-adsb-scope.sh" >&2
else
    adsb_flash_main "$@"
fi
