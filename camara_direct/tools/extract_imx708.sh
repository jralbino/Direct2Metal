#!/bin/bash
# extract_imx708.sh
#
# Captures EVERYTHING needed to reproduce libcamera's IMX708 pipeline in the
# bare-metal Direct2Metal project. Complementary to linux_baseline.sh, which
# only takes register snapshots; this script also captures the full I2C
# init trace, the ISP tuning JSON, the kernel driver mode tables, and DNG
# metadata, in BOTH full-res and bare-metal-matched modes.
#
# Run on a Raspberry Pi (Zero 2 W or any Pi with imx708 working under Pi OS
# Bookworm) with the IMX708 attached. Must be run as root for ftrace + i2c
# raw access.
#
#   sudo bash tools/extract_imx708.sh
#
# Output: imx708_extract_<timestamp>/  + imx708_extract_<timestamp>.tar.gz

set -u

TS=$(date +%Y%m%d_%H%M%S)
OUT="imx708_extract_${TS}"
mkdir -p "$OUT"/{full_4608x2592,binned_1536x864,registers,tuning,driver,pipeline,meta}

log() { echo "[$(date +%H:%M:%S)] $*" | tee -a "$OUT/meta/run.log"; }
have() { command -v "$1" >/dev/null 2>&1; }

# Pick the right capture binary (Bookworm = rpicam-*, older = libcamera-*)
if have rpicam-still;   then STILL=rpicam-still;   VID=rpicam-vid
elif have libcamera-still; then STILL=libcamera-still; VID=libcamera-vid
else log "ERROR: neither rpicam-still nor libcamera-still found"; exit 1
fi
log "Using capture binaries: STILL=$STILL  VID=$VID"

# ---------------------------------------------------------------------------
# I2C bus auto-detect (Pi v3 cam usually i2c-10; Pi 5 may be i2c-11/0/4)
# ---------------------------------------------------------------------------
detect_imx_bus() {
    for bus in 10 11 0 1 2 3 4 5 6 7 8 9; do
        [ -e "/dev/i2c-$bus" ] || continue
        i2cdetect -y "$bus" 2>/dev/null | grep -q " 1a " && { echo "$bus"; return; }
    done
}
BUS=$(detect_imx_bus)
log "IMX708 I2C bus detected: ${BUS:-NONE}"

# ---------------------------------------------------------------------------
# ftrace setup — captures EVERY i2c write/read while we run a capture.
# This is the gold mine: it gives the full register-by-register init that
# the kernel driver does, in order, for whatever mode we ask for.
# ---------------------------------------------------------------------------
TRACE=/sys/kernel/debug/tracing
trace_cleanup() {
    [ -d "$TRACE" ] || return
    echo 0 > "$TRACE/tracing_on"           2>/dev/null || true
    echo 0 > "$TRACE/events/i2c/enable"    2>/dev/null || true
    echo nop > "$TRACE/current_tracer"     2>/dev/null || true
}
trap trace_cleanup EXIT

trace_capture() {
    # $1 = output filename; $2... = command to run with trace active
    local outfile="$1"; shift
    if [ ! -d "$TRACE" ]; then
        log "  ftrace unavailable, skipping I2C trace for: $*"
        "$@" >>"$OUT/meta/run.log" 2>&1
        return
    fi
    echo 0 > "$TRACE/tracing_on"
    echo > "$TRACE/trace"
    echo nop > "$TRACE/current_tracer"
    echo 1 > "$TRACE/events/i2c/enable"
    echo 1 > "$TRACE/tracing_on"
    "$@" >>"$OUT/meta/run.log" 2>&1
    echo 0 > "$TRACE/tracing_on"
    cp "$TRACE/trace" "$outfile"
    echo 0 > "$TRACE/events/i2c/enable"
    # Filter to just IMX708 (addr=0x1a / 0x001a) for readability
    grep -E "addr=0x0*1a|addr=001a" "$outfile" > "${outfile%.txt}_imx708only.txt" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Live register dump via i2ctransfer (state AFTER the driver has configured
# the sensor — useful to compare against what your bare-metal writes).
# IMX708 has 16-bit register addresses.
# ---------------------------------------------------------------------------
dump_imx708_state() {
    local outfile="$1"
    [ -z "$BUS" ] && { echo "# no i2c bus" >"$outfile"; return; }
    {
        echo "# IMX708 live register dump (bus=$BUS addr=0x1a) $(date -Is)"
        for reg in 0x0100 0x0101 0x0112 0x0113 0x0114 0x0136 0x0137 \
                   0x0202 0x0203 0x0204 0x0205 \
                   0x020E 0x020F 0x0210 0x0211 0x0212 0x0213 0x0214 0x0215 \
                   0x0220 0x0221 0x0222 0x0310 \
                   0x0340 0x0341 0x0342 0x0343 \
                   0x0344 0x0345 0x0346 0x0347 0x0348 0x0349 0x034A 0x034B \
                   0x034C 0x034D 0x034E 0x034F 0x0350 \
                   0x0900 0x0901 0x0902 0x0381 0x0383 0x0385 0x0387 \
                   0x0B8E 0x0B8F 0x0B94 0x0B95 \
                   0x3400 0x3F56 0x3F57 0x3F58 0x3F59; do
            rh=$(( (reg >> 8) & 0xFF )); rl=$(( reg & 0xFF ))
            v=$(i2ctransfer -y "$BUS" w2@0x1a \
                $(printf '0x%02X' $rh) $(printf '0x%02X' $rl) r1@0x1a 2>/dev/null)
            printf "  %s = %s\n" "$reg" "${v:-ERR}"
        done
    } > "$outfile"
}

# ===========================================================================
# (1) MODE A — FULL-RES 4608x2592 (visual ground truth + tuning reference)
# ===========================================================================
log "[1/7] Capturing FULL-RES 4608x2592 (DNG + JPG) with I2C trace..."
trace_capture "$OUT/registers/i2c_trace_full_4608x2592.txt" \
    $STILL --mode 4608:2592:10:P --raw \
           -o "$OUT/full_4608x2592/full.jpg" \
           --immediate --nopreview \
           --shutter 0 --gain 0    # 0 = let AE/AGC pick → realistic values

# Move the auto-generated DNG (rpicam saves it next to the jpg)
[ -f "$OUT/full_4608x2592/full.dng" ] || mv "$OUT/full_4608x2592/"*.dng \
    "$OUT/full_4608x2592/full.dng" 2>/dev/null || true

dump_imx708_state "$OUT/registers/imx708_state_full.txt"

# ===========================================================================
# (2) MODE B — BINNED 1536x864 (byte-for-byte parity with bare-metal)
# ===========================================================================
log "[2/7] Capturing BINNED 1536x864 RAW (matches bare-metal mode) with I2C trace..."
trace_capture "$OUT/registers/i2c_trace_binned_1536x864.txt" \
    $STILL --mode 1536:864:10:P --raw \
           -o "$OUT/binned_1536x864/binned.jpg" \
           --immediate --nopreview \
           --shutter 10000 --gain 4
[ -f "$OUT/binned_1536x864/binned.dng" ] || mv "$OUT/binned_1536x864/"*.dng \
    "$OUT/binned_1536x864/binned.dng" 2>/dev/null || true

# Pure raw bytes (no DNG container) — directly diffable against bare-metal frame
log "      ...also grabbing raw Bayer bytes (libcamera-raw) for byte-level diff"
if have libcamera-raw; then
    libcamera-raw --mode 1536:864:10:P -t 200 --segment 1 \
        -o "$OUT/binned_1536x864/raw_%05d.raw" --nopreview \
        >>"$OUT/meta/run.log" 2>&1 || true
elif have rpicam-raw; then
    rpicam-raw --mode 1536:864:10:P -t 200 --segment 1 \
        -o "$OUT/binned_1536x864/raw_%05d.raw" --nopreview \
        >>"$OUT/meta/run.log" 2>&1 || true
fi

dump_imx708_state "$OUT/registers/imx708_state_binned.txt"

# ===========================================================================
# (3) ISP TUNING FILE (the JSON with BLC, AWB, CCM, gamma, lens shading)
# ===========================================================================
log "[3/7] Copying libcamera ISP tuning files..."
for path in \
    /usr/share/libcamera/ipa/rpi/vc4/imx708.json \
    /usr/share/libcamera/ipa/rpi/vc4/imx708_noir.json \
    /usr/share/libcamera/ipa/rpi/vc4/imx708_wide.json \
    /usr/share/libcamera/ipa/rpi/pisp/imx708.json \
    /usr/share/libcamera/ipa/raspberrypi/imx708.json
do
    [ -f "$path" ] && cp -v "$path" "$OUT/tuning/" 2>>"$OUT/meta/run.log"
done
ls /usr/share/libcamera/ipa/ -laR > "$OUT/tuning/_ipa_tree.txt" 2>/dev/null || true

# ===========================================================================
# (4) KERNEL DRIVER mode tables (the source of truth for register sequences)
# ===========================================================================
log "[4/7] Fetching kernel driver source (imx708.c mode tables)..."
KVER=$(uname -r | cut -d. -f1-2)
for branch in "rpi-${KVER}.y" rpi-6.6.y rpi-6.12.y rpi-6.1.y; do
    if curl -fsSL --max-time 20 \
        "https://raw.githubusercontent.com/raspberrypi/linux/${branch}/drivers/media/i2c/imx708.c" \
        -o "$OUT/driver/imx708_${branch}.c" 2>/dev/null; then
        log "      got driver from branch ${branch}"
        break
    fi
done
# Also grab device-tree overlay
for branch in "rpi-${KVER}.y" rpi-6.6.y rpi-6.12.y; do
    curl -fsSL --max-time 20 \
        "https://raw.githubusercontent.com/raspberrypi/linux/${branch}/arch/arm/boot/dts/overlays/imx708-overlay.dts" \
        -o "$OUT/driver/imx708-overlay_${branch}.dts" 2>/dev/null && break
done

# ===========================================================================
# (5) MEDIA-CTL pipeline graph
# ===========================================================================
log "[5/7] Dumping media-ctl pipeline + v4l2 info..."
for dev in /dev/media*; do
    [ -e "$dev" ] || continue
    name=$(basename "$dev")
    media-ctl -d "$dev" -p > "$OUT/pipeline/${name}_topology.txt" 2>&1 || true
    media-ctl -d "$dev" --print-dot > "$OUT/pipeline/${name}.dot" 2>&1 || true
    if have dot; then
        dot -Tpng "$OUT/pipeline/${name}.dot" \
            -o "$OUT/pipeline/${name}.png" 2>/dev/null || true
    fi
done
v4l2-ctl --list-devices > "$OUT/pipeline/v4l2_devices.txt" 2>&1 || true
for vd in /dev/video*; do
    [ -e "$vd" ] || continue
    v4l2-ctl --device="$vd" --all > "$OUT/pipeline/v4l2_$(basename $vd)_all.txt" 2>&1 || true
done

# ===========================================================================
# (6) DNG METADATA — gain/exposure/WB/CCM that AE/AWB actually picked
# ===========================================================================
log "[6/7] Extracting DNG metadata (the runtime values AE/AWB chose)..."
if have exiftool; then
    for dng in "$OUT"/full_4608x2592/*.dng "$OUT"/binned_1536x864/*.dng; do
        [ -f "$dng" ] || continue
        out="${dng%.dng}_exif.txt"
        exiftool -a -G1 -s "$dng" > "$out" 2>/dev/null
        # Pull the most useful subset to a single file
        exiftool -BlackLevel -WhiteLevel -CFAPattern* -AsShotNeutral \
                 -ColorMatrix1 -ColorMatrix2 -CalibrationIlluminant1 \
                 -CalibrationIlluminant2 -BaselineExposure -ExposureTime \
                 -ISO -AnalogueGain -DigitalGain -ImageWidth -ImageHeight \
                 "$dng" >> "$OUT/meta/dng_summary.txt" 2>/dev/null
        echo "---" >> "$OUT/meta/dng_summary.txt"
    done
else
    log "      exiftool not installed, skipping (apt install libimage-exiftool-perl)"
fi

# Also try dcraw quick-info if present
if have dcraw; then
    for dng in "$OUT"/full_4608x2592/*.dng "$OUT"/binned_1536x864/*.dng; do
        [ -f "$dng" ] || continue
        dcraw -v -i "$dng" >> "$OUT/meta/dcraw_info.txt" 2>&1
        echo "---" >> "$OUT/meta/dcraw_info.txt"
    done
fi

# ===========================================================================
# (7) SYSTEM CONTEXT (so we know what produced this bundle)
# ===========================================================================
log "[7/7] Capturing system context..."
{
    echo "=== uname"; uname -a
    echo "=== os-release"; cat /etc/os-release 2>/dev/null
    echo "=== libcamera version"; $STILL --version 2>&1 | head -5
    echo "=== camera list"; $STILL --list-cameras 2>&1
    echo "=== /boot/firmware/config.txt (camera lines)";
    grep -iE "camera|imx708|dtoverlay" /boot/firmware/config.txt 2>/dev/null \
        || grep -iE "camera|imx708|dtoverlay" /boot/config.txt 2>/dev/null
    echo "=== dmesg (filtered)"
    dmesg | grep -iE "imx708|unicam|csi|bcm2835-isp" | tail -200
} > "$OUT/meta/system.txt" 2>&1

# ---------------------------------------------------------------------------
# Bundle
# ---------------------------------------------------------------------------
log "Bundling into ${OUT}.tar.gz ..."
tar czf "${OUT}.tar.gz" "$OUT"
log "Done."
echo
echo "  Bundle:    ${OUT}.tar.gz   ($(du -h "${OUT}.tar.gz" | cut -f1))"
echo "  Directory: ${OUT}/"
echo
echo "Copy ${OUT}.tar.gz back to your dev machine:"
echo "  scp pi@<pi-ip>:$(pwd)/${OUT}.tar.gz ."
