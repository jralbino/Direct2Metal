#!/bin/bash
# Capture a Linux/libcamera ground-truth baseline for byte-for-byte and
# register-for-register comparison against the bare-metal pipeline.
#
# Run this on a Raspberry Pi running Pi OS with libcamera + rpicam-apps
# installed and an IMX708 camera attached. The Pi must boot Linux (NOT
# kernel8.img). After this script finishes you will have a directory
# `linux_capture/` containing everything needed for offline diff against
# the bare-metal capture report.
#
# Usage:
#   sudo bash tools/linux_baseline.sh
#
# Output:
#   linux_capture/unicam_pre.txt    Unicam regs BEFORE streaming starts
#   linux_capture/unicam_run.txt    Unicam regs WHILE streaming
#   linux_capture/unicam_run2.txt   second sample to detect drift
#   linux_capture/imx708_run.txt    IMX708 i2c regs WHILE streaming
#   linux_capture/frame.dng         libcamera-captured RAW10 of same scene
#   linux_capture/v4l2_info.txt     v4l2-ctl format / control info
#   linux_capture/dmesg_unicam.txt  kernel ring buffer (filtered for csi/unicam)

set -u

OUT=linux_capture
mkdir -p "$OUT"

# Unicam1 BCM2837 MMIO base; same as bare-metal MMIO_BASE+0x801000
U1_BASE=0x3F801000
declare -A REGS=(
    [CTRL]=0x000  [STA]=0x004   [ANA]=0x008   [PRI]=0x00C
    [CLK]=0x010   [CLT]=0x014   [DAT0]=0x018  [DAT1]=0x01C
    [DAT2]=0x020  [DAT3]=0x024  [DLT]=0x028   [CMP0]=0x02C
    [ICTL]=0x100  [ISTA]=0x104  [IDI0]=0x108  [IPIPE]=0x10C
    [IBSA0]=0x110 [IBEA0]=0x114 [IBLS]=0x118  [IBWP]=0x11C
    [IHWIN]=0x120 [IVWIN]=0x128 [MISC]=0x400
)

read_mmio() {
    # Try devmem (busybox), then devmem2, then fallback python.
    local addr="$1"
    if command -v devmem >/dev/null 2>&1; then
        devmem "$addr" 32 2>/dev/null
    elif command -v devmem2 >/dev/null 2>&1; then
        devmem2 "$addr" w 2>/dev/null | awk '/Read at/ {print $NF}'
    else
        python3 -c "
import mmap, struct, sys
addr = int(sys.argv[1], 16)
page = addr & ~0xFFF
off  = addr & 0xFFF
fd = open('/dev/mem', 'rb')
m = mmap.mmap(fd.fileno(), 4096, prot=mmap.PROT_READ, offset=page)
print('0x%08X' % struct.unpack('<I', m[off:off+4])[0])
" "$addr"
    fi
}

dump_unicam() {
    local label="$1"
    {
        echo "# Unicam1 register dump: $label"
        echo "# base=$U1_BASE  $(date -Is)"
        for name in CTRL STA ANA PRI CLK CLT DAT0 DAT1 DAT2 DAT3 DLT CMP0 \
                    ICTL ISTA IDI0 IPIPE IBSA0 IBEA0 IBLS IBWP IHWIN IVWIN MISC; do
            local off=${REGS[$name]}
            local addr=$(printf '0x%X' $((U1_BASE + off)))
            local val=$(read_mmio "$addr")
            printf "  %-6s offset=%-7s val=%s\n" "$name" "$off" "${val:-ERR}"
        done
    } > "$OUT/$label"
}

# Try to find an i2c bus that has the IMX708 (0x1A). Pi v3 cam is usually i2c-10.
detect_imx_bus() {
    for bus in 10 11 0 1 2 3 4 5 6 7 8 9; do
        if [ -e "/dev/i2c-$bus" ] && i2cdetect -y "$bus" 2>/dev/null | grep -q "1a"; then
            echo "$bus"
            return
        fi
    done
    echo ""
}

dump_imx708() {
    local bus="$1"
    local label="$2"
    if [ -z "$bus" ]; then
        echo "# imx708 i2c bus not found, skipping $label" > "$OUT/$label"
        return
    fi
    {
        echo "# IMX708 i2c register dump: bus=$bus addr=0x1a"
        echo "# $(date -Is)"
        # Set of registers worth comparing — must match what bare-metal writes
        for reg in 0x0100 0x0101 0x0114 0x0136 0x0137 0x0202 0x0203 \
                   0x0204 0x0205 0x020E 0x020F 0x0220 0x0310 \
                   0x0340 0x0341 0x0342 0x0343 0x0344 0x0345 0x0346 0x0347 \
                   0x0348 0x0349 0x034A 0x034B 0x034C 0x034D 0x034E 0x034F \
                   0x0350 0x0900 0x0901 0x0902 0x3400 0x0B8E 0x0B94 \
                   0x0112 0x0113 0x3F56 0x3F57 0x3F58 0x3F59; do
            # IMX708 uses 16-bit register addresses, so use i2cset/i2cget
            # in the right mode. The i2cget byte-data mode does only 8-bit
            # reg, so we use i2ctransfer for 16-bit register addressing.
            local rh=$((reg >> 8 & 0xFF))
            local rl=$((reg & 0xFF))
            local val
            val=$(i2ctransfer -y "$bus" w2@0x1a "$(printf '0x%02X' $rh)" "$(printf '0x%02X' $rl)" r1@0x1a 2>/dev/null)
            printf "  reg=%s  val=%s\n" "$reg" "${val:-ERR}"
        done
    } > "$OUT/$label"
}

echo "[1/6] Pre-stream Unicam reg snapshot..."
dump_unicam unicam_pre.txt

echo "[2/6] Starting libcamera stream in background (1536x864 RAW10, 5s)..."
# rpicam-vid streams continuously; we kick it off, wait for it to start
# producing, then dump regs while it runs.
rpicam-vid --mode 1536:864:10:P -t 5000 -o "$OUT/stream.h264" --nopreview \
    > "$OUT/rpicam_log.txt" 2>&1 &
RPICAM_PID=$!
sleep 2

echo "[3/6] During-stream Unicam reg snapshot..."
dump_unicam unicam_run.txt
sleep 1
dump_unicam unicam_run2.txt

BUS=$(detect_imx_bus)
echo "[4/6] During-stream IMX708 i2c snapshot (bus=${BUS:-NONE})..."
dump_imx708 "$BUS" imx708_run.txt

echo "[5/6] Waiting for rpicam-vid to finish..."
wait "$RPICAM_PID" 2>/dev/null || true

echo "[6/6] Capturing single RAW frame for byte-level comparison..."
rpicam-still --raw --mode 1536:864:10:P --shutter 10000 --gain 4 \
    -o "$OUT/frame.dng" --nopreview \
    > "$OUT/rpicam_still_log.txt" 2>&1

# Extra context
v4l2-ctl --list-devices > "$OUT/v4l2_info.txt" 2>&1 || true
echo "" >> "$OUT/v4l2_info.txt"
v4l2-ctl --device=/dev/video0 --all >> "$OUT/v4l2_info.txt" 2>&1 || true

dmesg | grep -iE "unicam|csi|bcm2835|imx708" > "$OUT/dmesg_unicam.txt" 2>&1 || true

echo
echo "Done. Linux baseline saved to: $OUT/"
ls -la "$OUT"
