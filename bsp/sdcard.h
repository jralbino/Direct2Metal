/* File: src/sdcard.h
 * V116 — Minimal EMMC + FAT32 frame save
 *
 * Saves one 480×480 PPM image to FRAME.PPM on the FAT32 boot partition.
 *
 * Setup (run once on PC before first boot):
 *   python3 -c "
 *     hdr = b'P6\n480 480\n255\n'
 *     open('FRAME.PPM','wb').write(hdr + bytes(480*480*3))
 *   "
 * Copy FRAME.PPM to the Pi SD card boot partition (same folder as kernel8.img).
 * After boot, the firmware overwrites FRAME.PPM with the first live camera frame.
 * Read back on Linux: cp /boot/firmware/FRAME.PPM /tmp/ && eog /tmp/FRAME.PPM
 */
#ifndef SDCARD_H
#define SDCARD_H

#include <stdbool.h>
#include <stdint.h>

/* Initialize EMMC and locate the FAT32 boot partition.
 * Returns true on success. Must be called before sdcard_save_ppm(). */
bool sdcard_init();

/* Write 480×480 RGB PPM to FRAME.PPM on the boot partition.
 * Calls debayer_raw10_row_rgb() row-by-row; no large intermediate buffer needed.
 * Returns true on success. */
bool sdcard_save_ppm();

#endif /* SDCARD_H */
