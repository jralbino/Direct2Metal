# Phase 9 — Camera Debug History (V17–V93)

## Confirmed Hardware Facts

| Fact | Evidence |
|------|----------|
| D0/D1 physically crossed on Pi Zero 2W FFC | V17(LSM=1)→WP+4096 vs V19(LSM=0)→WP=IBSA0 |
| DMA bus alias: IBSA0 must be 0xC0000000 (VC bus) | Pi OS IBWP=0xCBD95000=0xC0000000\|phys; 0x40000000 (V18–V104) = wrong bus alias for Unicam (V105) |
| IDI0=0x00 required | IDI0=0x2A filtered ALL packets (V20) — sensor sends non-RAW8 embedded data first |
| IPIPE=1 required | IPIPE=0 disconnects CSI-2→DMA routing |
| BSC1 probe first | BSC0 has spurious DONE from VPU boot scan |
| ICTL write disrupts DMA | ICTL=0x03 in V21 → ICTL readback=0, WP=IBSA0 (trigger-not-retain) |
| ICTL must remain 0x00 | V22 confirmed: registers all correct, IDI0=0, IPIPE=1, ICTL=0 |
| MCLK dtoverlay sets wrong freq | dtoverlay=imx708 → CM_GP2 SRC=XOSC, DIVI=585, DIVF=3840 → **32.6 kHz** |
| Sensor generates 55fps MIPI | V25 frame_count=55 at t=500ms → MIPI IS transmitting |
| D-PHY CLK register frozen | CLK=0xCCC00002 never changes PRE→POST stream, V22-V28 |
| VPU left ANA=0x777 | Pre-init dump V26 — bits 0-2,4-6,8-10 set (more than our 0x43) |
| VPU left CLT=0 | Pre-init dump V27 — timing never written by VPU |

## MCLK Root Cause and Fix (V23/V24)

- `dtoverlay=imx708` sets `CM_GP2CTL=0x291` (SRC=XOSC=19.2MHz), `CM_GP2DIV` DIVI=585, DIVF=3840 → **~32.6 kHz** (not 24 MHz!)
- V17 WP+4096 was because sensor ran at ~611 kHz MIPI (32.6kHz MCLK × PLL factor) — garbage LP-speed signal Unicam accidentally accepted
- **V24 fix**: before XSHUTDOWN assert, reconfigure CM_GP2 → SRC=PLLD (500MHz), DIVI=20, DIVF=853, MASH=1 → **~24.0 MHz**
- CM_GP2CTL=662=0x296 confirmed: BUSY=1, ENAB=1, MASH=1, SRC=6=PLLD ✓
- CM_GP2DIV=82773=0x14355 confirmed: DIVI=20, DIVF=853 ✓

## Version-by-Version Summary

| Version | IDI0 | ICTL | CTRL | Bus alias | MCLK | WP result | ISTA_FE | Notes |
|---------|------|------|------|-----------|------|-----------|---------|-------|
| V71 | 0x2A | 0x27 | 0x080F03 | 0x40000000 | 24 MHz | pending | pending | **3 LINUX DRIVER FIXES**: (1) MISC=0x240 restored (FL0/FL1 SET — V68's clearing was WRONG). (2) CTRL gets PFT=0xF(bits[11:8]) + OET=128(bits[20:12]) = 0x080F02/03. (3) ICTL LIP=BIT(5) after CPE enable → ICTL=0x27 (Load Image Pointers, arms DMA). (4) IPIPE=0x00 (not 0x80, PPM_NONE for native RAW8). Sensor: 1-lane RAW8 unchanged. |
| V70 | 0x2A | 0x07 | 0x0002→3 | 0x40000000 | 24 MHz | WP=0 | ✗ | 1-lane+no SLS, MISC=0x000, CTRL=0x0003. D0hi oscillates HS↔LP-11 ✓, CLKhi=3328 ✓. Removing SLS made NO difference. ISTA=0. Root cause: missing PFT/OET in CTRL + MISC wrongly cleared + no LIP bit. |
| V69 | 0x2A | 0x07 | 0x0002→3 | 0x40000000 | 24 MHz | WP=0 | ✗ | 2-lane sensor (0x0114=0x01) + no SLS, CLKGATE=0x5A00000D. D0hi=D1hi=LP-11 ALL prints. 2-lane with our init table fails. ISTA=0. |
| V68 | 0x2A | 0x07 | 0x0042→43 | 0x40000000 | 24 MHz | WP=0 | ✗ | **CLKGATE PASSWORD + MISC=0**: (1) CLKGATE: `0x15`→`0x5A000005` ✓. (2) MISC: `0x240`→`0x000` (WRONG — FL0/FL1 should be SET per Linux driver). Also: IHWIN=IVWIN=0 (crop disabled), PRI=0xE85 (AXI priority). |
| V67 | 0x2A | 0x07 | 0x0042→43 | 0x40000000 | 24 MHz | WP=0 | ✗ | DLT2=CLT2=6→15. D0hi still oscillates HS↔LP-11, ISTA=0. Timing fix made NO difference → ths_settle not root cause. Root causes found from research: CLKGATE password missing + MISC FL0/FL1 forcing LP. |
| V66 | 0x2A | 0x07 | 0x0042→43 | 0x40000000 | 24 MHz | WP=0 | ✗ | **1-LANE + RESTORED D-PHY**: D0hi=2560(HS) ✓ oscillating HS↔LP-11, CLKhi=3328(HS) constant ✓, frame_count=52 ✓. BUT ISTA=0, STA=0, WP=0. **ROOT CAUSE: DLT2=6 gives 60ns ths_settle < 91.7ns MIPI minimum at 900Mbps.** BCM2837 samples before byte sync = Frame Start never detected. |
| V65 | 0x2A | 0x00 | 0x0002→? | 0x40000000 | 24 MHz | n/a | ✗ | User's version: CLK=0x01, DAT0=0x01 (stripped D-PHY), ICTL=0x00000000, sensor 2-lane. D-PHY deaf to all signals. |
| V64 | 0x2A | 0x00 | 0x0002→? | 0x40000000 | 24 MHz | n/a | ✗ | User's version: same D-PHY stripping + ICTL=0. |
| V40 | 0x2A | 0x07 | 0x0002→3 | 0x40000000 | 24 MHz | WP=0 | ✗ | **ALL CRITICAL BUGS FIXED**: DLT 0x020→0x028, CMP0 written, IDI0=0x2A, IPIPE=0x80, IBSA1 wrong writes removed, MISC FL0/FL1 written, clean CTRL (no OET bit14). D0hi=D1hi=51712 (LP-11) in 2-lane — data lanes stuck despite correct timing. |
| V39 | 0x00 | 0x07 | 0x4002→3 | 0x40000000 | 24 MHz | +0 | ✗ | IDI0=0x00 was WRONG (means "match FS only", not accept-all). DLT still at 0x020 (=DAT2). ISTA=0. |
| V38 | 0x2A | 0x07 | 0x4002→3 | 0x40000000 | 24 MHz | +0 | ✗ | LSM=1 restored (CTRL=16387=0x4003 confirmed). **IDENTICAL to V37**. LSM ruled out. D0hi=D1hi=51712 always. ISTA=0, WP=0. |
| V37 | 0x2A | 0x07 | 0x0002→3 | 0x40000000 | 24 MHz | +0 | ✗ | 2-lane restored (0x0114=0x01) + CAM1=100MHz + **LSM=0**. frame_count=55 ✓, CLKhi oscillates 3328↔52480 (gated clock ✓). BOTH D0hi=D1hi=51712 (LP-11) throughout ALL 30 prints — neither data lane ever goes HS. ISTA=0, WP=0. |
| V36 | 0x2A | 0x07 | 0x0002→3 | 0x40000000 | 24 MHz | +0 | ✗ | CAM1=100MHz (DIVI=5, CTL=662 DIV=20480 confirmed). D0hi now oscillates HS↔LP-11 (gated clock visible). ISTA=0 — CAM1 clock NOT the root cause |
| V35 | 0x2A | 0x07 | 0x0002→3 | 0x40000000 | 24 MHz | +0 | ✗ | LSM=0 (1-lane mode): D0hi=2560(HS), D1hi=51712(LP-11) — SAME as V34(LSM=1). LSM does NOT affect D0hi/D1hi physical pad reports. Physical D0=sensor D0 (no cross). ISTA=0 with both LSM values → issue is not lane routing |
| V34 | 0x2A | 0x07 | 0x4002→3 | 0x40000000 | 24 MHz | +0 | ✗ | 1-lane sensor(0x0114=0): CLKhi=3328 continuous HS ✓; D0hi=2560(0x0A00=HS!) from print2 ✓; D1hi=51712(LP-11) → sensor D0 IS reaching BCM2837 phys D1 (via FFC cross + LSM=1 swap). ISTA=0, WP=0 — CSI-2 protocol engine silent despite HS |
| V33 | 0x2A | 0x07 | 0x4002→3 | 0x40000000 | 24 MHz | +0 | ✗ | IDI0=42 ✓, PRI=0, D0hi=D1hi=51712 BOTH LANES LP-11 always even when CLKhi=3328 (HS). Data lanes stuck in LP-11. |
| V29 | 0x00 | 0x07→4 | 0x4001/5 | 0x40000000 | 24 MHz | +0 | ✗ | ANA fixed! CLKhi changed (3328≠52480) = D-PHY lock. IPIPE=0 disabled DMA. ICTL readback=4 (CPE clears FSIE/FEIE) |
| V33 | 0x2A | 0x07 | 0x4002→3 | 0x40000000 | 24 MHz | pending | pending | IDI0=0x2A(RAW8 DT) — first valid test with correct ANA+MCLK+IPIPE; + DAT lane monitoring |
| V32 | 0x00 | 0x07 | 0x4002→3 | 0x40000000 | 24 MHz | +0 | ✗ | IPIPE=2 ✓, CE 0→1 edge ✓ — STA=0 ISTA=0 WP=0; IDI0=0 keeps engine idle |
| V31 | 0x00 | 0x07 | 0x4003 | 0x40000000 | 24 MHz | +0 | ✗ | ICTL=7 ✓ (CPR pulse-then-clear confirmed!), CLKhi=3328, D0/D1 active — WP still 0; IPIPE=3 was UNPACK10 wrong (→ stride mismatch aborts DMA), CE never pulsed 0→1 |
| V30 | 0x00 | 0x07 | 0x4001/5 | 0x40000000 | 24 MHz | +0 | ✗ | IPIPE=3 confirmed ✓, CLT=0x0602 ✓, DLT ✓, CLKhi=3328 ✓ — WP still 0; CPR=BIT(2) permanently set suspected |
| V17 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 32.6 kHz (wrong) | +4096 | ✗ | Garbage LP-speed MIPI received |
| V18 | 0x00 | 0x00 | 0x0001 | 0xC0000000 | 32.6 kHz | =IBSA0 | ✗ | Wrong bus alias + IPIPE=0 + LSM=0 |
| V19 | 0x00 | 0x00 | 0x0001 | 0x40000000 | 32.6 kHz | =IBSA0 | ✗ | LSM=0 → D-PHY can't lock |
| V20 | 0x2A | 0x00 | 0x4001 | 0x40000000 | 32.6 kHz | =IBSA0 | ✗ | IDI0=0x2A filtered ALL packets |
| V21 | 0x00 | 0x03 | 0x4001 | 0x40000000 | 32.6 kHz | =IBSA0 | ✗ | ICTL=0x03 disrupted DMA; readback=0 |
| V22 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 32.6 kHz | =IBSA0 | ✗ | All regs correct; PRE=POST CLK/DAT |
| V23 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 32.6 kHz | =IBSA0 | ✗ | Added GPCLK2 diagnostic, found 32.6kHz |
| V24 | 0x00 | 0x00 | 0x4001 | 0x40000000 | **24 MHz** | =IBSA0 | ✗ | MCLK fixed, CLK frozen, fc=? |
| V25 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 24 MHz | =IBSA0 | ✗ | frame_count=55 → sensor IS transmitting |
| V26 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 24 MHz | =IBSA0 | ✗ | No reset, ANA=0x777 preserved, CLK frozen |
| V27 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 24 MHz | =IBSA0 | ✗ | Zero D-PHY writes — CLK STILL frozen |
| V28 | 0x00 | 0x00 | 0x4001 | 0x40000000 | 24 MHz | pending | pending | Fix clkgate 0x3F802004 + CLE+calib |

## What V27 Proved (Definitive)

With **zero writes** to ANA, CLK, CLT, DAT0, DAT1 (VPU state fully preserved), CLK register still never changes PRE→POST stream. This eliminates all our register writes as the cause.

**The D-PHY is not receiving 450 Mbps HS signals at the hardware level**, independent of software configuration.

## V28 Hypotheses Being Tested

1. **Clock gate wrong address**: Linux DT specifies 0x7E802004 (= 0x3F802004) as the Unicam clock gate. We were writing to 0x3F803000. V28 writes both.
2. **CLE bit missing with calibration**: VPU set CLK bit 1 (calibration) but not bit 0 (CLE). V28 adds CLE (bit 0) while preserving VPU's calibration bits.
3. **Unknown registers**: Dump offset 0x00C and 0x020 (possible DLT — data lane timing).

## V29 — ANA Power-Up + CLPD/DLPD Fix (confirmed root causes)

Root causes confirmed by cross-referencing `vc4-regs-unicam.h` + `bcm2835-unicam.c`:

| Bug | Root cause | Fix |
|-----|-----------|-----|
| ANA=0x777 | APD=BIT(0)=1, BPD=BIT(1)=1, AR=BIT(2)=1 → D-PHY fully powered down | Write 0x774, delay ≥1ms, write 0x770 |
| CLK=0xCCC00002 | CLPD=BIT(1)=1 → clock lane power down; our `\|=0x01` left CLPD=1 | `(old & 0xFFFF0000)\|0x1D` (CLE\|CLLPE\|CLHSE\|CLTRE, CLPD=0) |
| DAT0/1 | DLPD=BIT(1)=1 (same pattern) | `(old & 0xFFFF0000)\|0x1D` |
| CLT format | `(50<<16)\|15` wrote to reserved bits[31:16]; CLT2[15:8]=0 | `(6<<8)\|2` = 0x0602 |
| DLT missing | Offset 0x020 never written | `(6<<8)\|2` = 0x0602 |
| IPIPE=1 | UNPACK6, wrong for RAW8 | IPIPE=0 (PUM_NONE) |
| ISTA bits swapped | BIT(0)=FS, BIT(1)=FE (we had them reversed) | ISTA_FS=BIT(0), ISTA_FE=BIT(1) |
| ICTL=0x00 | No interrupts → ISTA never fires | ICTL=0x07 (FSIE\|FEIE\|IBOB) in enable_cpe |
| Mailbox device 12 | Was UNICAM0/CSI-0; we use UNICAM1/CSI-1 | device_id=13 |
| Clock gate after setup | 0x3F802004 must be written BEFORE ANA/CLK writes | Move before setup_unicam_block |

## V31 Confirmed / V32 Hypothesis

- V31 confirmed: ICTL=7 ✓ (CPR pulse-then-clear fixed the clearing!), CLKhi=3328, IPIPE=3 readback, CLT/DLT correct
- V31 remaining: WP=0, STA=0, ISTA=0 throughout (CLKhi oscillates 52480↔3328 between frames)
- V32 confirmed: IPIPE=2 ✓, CE correctly pulsed 0→1 ✓ — STA=0 ISTA=0 WP=0 still
- New root cause: **IDI0=0x00 keeps Unicam protocol engine in idle state**
  - IDI0=0x00 was declared "required" from invalid comparison: V20(IDI0=0x2A) vs V22(IDI0=0x00) — both had ANA=0x777 powered down!
  - Linux driver writes the actual CSI-2 data type (0x2A for RAW8) to IDI0
  - IDI0=0 may mean "no DT configured → engine stays idle" → STA=0, ISTA=0, WP=0
- **V33 fix**: IDI0=0x2A (RAW8 DT, first proper test with correct ANA+MCLK+IPIPE) + DAT0/DAT1 monitoring in capture loop + PRI register dump

## V40 — Critical Root Causes Found from vc4-regs-unicam.h (ALL Fixed)

Complete register map obtained from RPi Linux source `vc4-regs-unicam.h`. Multiple simultaneous bugs found:

### [A] DLT Wrong Offset — PRIMARY ROOT CAUSE
- `#define U_DLT 0x020` was WRONG. 0x020 = DAT2 (data lane 2 register, unused in 2-lane config).
- Real DLT (Data Lane Timing) = **0x028**.
- With DLT1=DLT2=0 (hardware reset): ths_settle=0ns → BCM2837 samples HS data immediately on preamble start → HS-SYNC byte 0xB8 corrupted → D-PHY never locks → D0hi/D1hi stay LP-11 → ISTA=0.
- **This explains V33–V39 symptoms: CLKhi goes HS (clock works) but D0hi/D1hi stay LP-11.**

### [B] IDI0 Meaning Corrected
- IDI0=0x00 does NOT mean "accept all packets". It means VC=0, DT=0x00 (Frame Start short pkt only).
- V39 was wrong. Correct value: IDI0=0x2A (VC=0, DT=0x2A = RAW8).

### [C] IPIPE Value Wrong
- PUM enum: 0=NONE, 1=UNPACK6, **2=UNPACK7**, 3=UNPACK8, 4=UNPACK10. PPM bits[9:7]: 0=none, 1=PACK8.
- IPIPE=2 (V33-V39) = PUM_UNPACK7 — wrong for RAW8.
- Linux bcm2835-unicam format table for RAW8: `unpack=PUM_NONE, pack=PPM_PACK8`
- Correct: IPIPE = 0 | (1<<7) = **0x80**.

### [D] CMP0 Never Written
- Linux always writes CMP0 (0x02C): `PCE=1|GI=1|CPH=1|VC=0|DT=1` (DT=1=Frame End short pkt).
- This fires STA.PI0=BIT(15) as a parallel FE path (since IDI0 filters short pkts).
- Linux ISR: `fe = (ISTA_FE || STA_PI0)`. We only checked ISTA_FE.

### [E] IBSA1/IBEA1/IBLS1 at Wrong Offsets
- 0x120=IHWIN, 0x124=IHSTA, 0x128=IVWIN (image window crop registers, NOT IBSA1/IBEA1/IBLS1).
- V17-V39 wrote `bus_base/bus_end/FRAME_W` to crop registers → potentially zeroed capture window.
- True IBSA1=0x304 (double-buffer section). Use single-buffer (omit IBSA1 writes).

### [F] MISC Never Written
- MISC (0x400): Linux writes FL0=BIT(6)|FL1=BIT(9) = 0x240 always.

### [G] CTRL BIT(14) = OET
- CTRL BIT(14) falls in OET_MASK[20:12] (Output Enable Timing), not "LSM".
- V34-V39 inadvertently set OET bit. V40 uses clean values: 0x0006/0x0002/0x0003.

## V33 Critical Finding — Data Lanes Stuck in LP-11

- `CLKhi` oscillates 52480 (LP-11) ↔ 3328 (HS) → clock lane IS receiving HS bursts ✓
- `D0hi=D1hi=51712=0xCA00` NEVER change across ALL iterations — both data lanes permanently LP-11
- Data lanes do NOT go HS even when clock does → BCM2837 D-PHY data frontend not seeing HS signal
- `PRE-STREAM D0=0x0A00 → POST-STREAM D0=0xCA00`: D0 DID react to stream_on (LP-11 assertion), but never goes HS
- `D1=0xCA00` before AND after stream_on — D1 never reacted at all

**V34 CONFIRMED** (1-lane, LSM=1):
- D0hi=2560 (0x0A00 = HS) from print 2 onward; D1hi=51712 (LP-11) always
- LSM=1 swaps hardware lane reporting: D0hi = physical D1 = sensor D0 (active) ✓
- Sensor D0 IS reaching BCM2837 physical D1 and IS in HS mode ✓
- BUT: ISTA_FS=0, WP=0 — CSI-2 decoder never decodes any packet
- CLKhi=3328 continuous throughout (1-lane mode → continuous HS clock vs gated in 2-lane)

**V35 CONFIRMED** (LSM=0, same result as V34/LSM=1):
- D0hi=2560(HS), D1hi=51712(LP-11) identical with both LSM values
- LSM does NOT physically swap D-PHY state registers; D0hi always = physical D0
- Physical D0 = sensor D0 (no FFC cross! original V17 "cross" conclusion was wrong)
- LSM=0 is correct for 1-lane (protocol uses physical D0 = sensor D0 as lane-0)
- But ISTA=0 with LSM=0 too → failure is NOT about lane routing

**V36 hypothesis**: CAM1 clock at 250MHz (PLLD/2) is too fast. DLT1=2, DLT2=6 calibrated for Linux's 100MHz (PLLD/5). At 250MHz: DLT1=2 → 8ns HS-SETTLE (too short, <20ns at 100MHz). BCM2837 samples HS bits before HS-SYNC preamble ends → misses 0xB8 → CSI-2 decoder never locks → ISTA=0.

## If V28 Still Fails — Remaining Hypotheses

- **Physical**: MIPI signals not reaching BCM2837 pads (broken FFC, wrong orientation)
- **VPU mailbox required**: CSI port activation via specific VPU mailbox command
- **Power domain wrong**: device_id=12 in mailbox may be TRANSPOSER, not IMAGE (=6)
- **Non-continuous vs continuous clock**: sensor may use gated clock; Unicam may need config

## Key Register Addresses

| Register | Address | Notes |
|----------|---------|-------|
| CM_GP2CTL | 0x3F101080 | GPCLK2 control (MCLK) |
| CM_GP2DIV | 0x3F101084 | GPCLK2 divisor |
| CM_CAM1CTL | **0x3F101048** | Unicam1 pixel clock control (0x3F101058=DSI0, WRONG!) |
| UNICAM1_BASE | 0x3F801000 | Unicam1 registers |
| UNICAM1_CLKGATE (old) | 0x3F803000 | Clock gate (wrong?) |
| UNICAM1_CLKGATE_DT | 0x3F802004 | Clock gate per Linux DT |
| U_ANA | base+0x008 | D-PHY analog (VPU sets 0x777) |
| U_CLK | base+0x010 | Clock lane (VPU sets bit1) |
| U_CLT | base+0x014 | Clock lane timing (VPU=0) |
| U_DAT0/1 | base+0x018/1C | Data lanes (VPU sets bit1) |

## IMX708 Diagnostic Registers

| Register | Address | Notes |
|----------|---------|-------|
| Chip ID hi/lo | 0x0016/0x0017 | Expected 0x07/0x08 |
| Mode select | 0x0100 | 0x01=streaming, 0x00=standby |
| Frame count | 0x0005 | Increments each frame; 55 in 500ms = ~110fps |
| Data format | 0x0112/0x0113 | 0x08/0x08 = RAW8 |
| Lane count | 0x0114 | 0x01 = 2 lanes |
| Binning enable | 0x0900 | 0x01 = enabled |
| Binning type | 0x0901 | Readback 0x34 (expected 0x22?) |

## V76–V81 History (2026-03-08 to 2026-03-09)

### V76 — Complete Unicam pipeline registers added (QEMU sim introduced)
**Root cause (all V17–V75)**: IDI0/IPIPE/IBEA0/IBLS/MISC/CLKGATE/ANA-before-CPR all MISSING.
- IDI0=0 → CSI-2 engine only sees FS short packets, ignores all RAW8 pixel data
- IPIPE never written → DMA state undefined
- IBEA0 missing → DMA has no end address
- IBLS missing → line stride = 0
- MISC FL0|FL1 missing → frame limit broken
- CLKGATE (0x3F802004) never written → clock gate open?
- ANA power-up was missing (regression from V72)

**V76 additions**: CLKGATE=0x5A000005, ANA=0xFF4→0xFF0, IDI0=0x2A, IPIPE=0x00,
IBEA0=bus_addr+FRAME_SZ, IBLS=FRAME_W, MISC=0x240, CLT/DLT=0x0602.
QEMU hardware-faithful simulator introduced (validates register sequence).

**Hardware V76 result**: IMX708 probe OK, frame_count=52, CLKhi=HS ✓, D0hi oscillates ✓
BUT: STA=0x00000000, ISTA=0, IBWP==IBSA0 throughout → protocol decoder silent.

### V77 — CM_CAM1CTL wrong address fixed
**Bug**: CM_CAM1CTL configured at 0x3F101058 (= CM_DSI0ECTL, configures DSI0 display clock!).
**Fix**: CM_CAM1CTL is at **0x3F101048** (offset 0x048 from CM base = BCM2835_CLOCK_CAM1).
**Hardware V77 result**: CM_CAM1CTL before:0 → after:662 ✓. ANA=0x770. STA=0 still persists.

### V78 — ANA CTATADJ/PTATADJ termination bias issue suspected
Hardware showed ANA=0x770 (CTATADJ=7, PTATADJ=7). Hypothesis: fields=7 = weak 100Ω termination.
Noted: Linux was believed to use ANA=0xFF0 (CTATADJ=PTATADJ=0xF = max bias).

### V79 — ANA=0xFF0 + simulator improvements
**Change**: ANA=0x774→0xFF4, 0x770→0xFF0 (CTATADJ=PTATADJ=0xF).
Simulator: added CM_CAM1CTL check, ANA CTATADJ/PTATADJ < 0xF check, ICTL LIP self-clearing,
STA synthesis (0 when error, FS|FE when ok), CLKhi/D0hi lane counter modelling.
**Critical**: ana_powered_up must use `!(val & 0x3)` not `val < 0x777` (0xFF0 > 0x777 numerically).
**Hardware V79 result**: ANA=0x00000FF0 ✓ confirmed. BUT STA=0 every 100ms throughout 500ms.
D1hi changes (18944→51712) after stream_on — some physical activity. Root cause still unknown.

### V80 — DLT timing increase + IMX708 0x0114 readback + VPU ANA readback
- ANA conditional: read VPU value first. If APD/BPD=0 (calibrated), preserve CTATADJ/PTATADJ.
  Rationale: dtoverlay=imx708 might calibrate D-PHY → overwriting destroys calibration.
- CLT/DLT: 0x0602 (settle=6, 53.7ns) → 0x1502 (settle=21, 188ns > MIPI spec 91.7ns min).
- Diagnostic: STA/ISTA/WP every 100ms in polling loop.
- IMX708: readback 0x0114 after write to verify 1-lane override applied.
**Hardware V80 result**:
  - `0x0114=0` (1-lane override IS working ✓)
  - `ANA (VPU): 0x00000777` (VPU left it powered DOWN — no calibration!)
  - ANA=0x00000FF0, CLT/DLT=0x00001502 ✓
  - STA=0x00000000 every 100ms through 400ms. IBWP=IBSA0. CLK=0x2D00001D, DAT0=0xE000001D.
  - D0hi=57344 post-stream (sensor active on D0) ✓. Still STA=0.

### V81 — ROOT CAUSE FOUND AND FIXED: CLK/DAT0 must be AFTER CPR (2026-03-09)
**Root cause**: In V80 (and all V76–V80), CLK=0x1D and DAT0=0x1D were written in STEP 3,
BEFORE the CPR pulse in STEP 5. CPR (Clock Pipe Reset, CTRL BIT(2)) resets the D-PHY
frontend registers including CLK/DAT0/DAT1 back to VPU boot default: 0x02 = CLPD/DLPD
= clock/data lane POWERED DOWN. After CPR, CLK lane was in power-down → no MIPI clock
recovery possible → STA=0 forever, regardless of any other register values.

Confirmed by cross-referencing Linux bcm2835-unicam.c unicam_start_rx() from rpi-6.6.y:
CLK/DAT0 is step 10 of 16, written well after CPR (step 3) and CTRL_BASE (step 4).

**V81 changes**:
1. CLK/DAT0/DAT1 moved to STEP 10 (after CPR, CTRL_BASE, PRI, IHWIN, ICTL, CLT/DLT, CMP0)
2. ANA DDL lock delay: 200K NOPs → 1M NOPs (~1ms, matching Linux usleep_range(1000,2000))
3. IBLS written before IBSA0/IBEA0 (Linux order)
4. IPIPE written before IDI0 (Linux order)
5. CTRL &= ~CPE explicit after CPR clear
6. Simulator: CLK/DAT0 write before CPR_pulsed → specific error message
7. Diagnostic: CLK register value now printed in init complete message

**QEMU V81 result**: ALL preconditions pass. CLK=0xC000001D (hi-counter from VPU + 0x1D).
ICTL=0x07 (LIP self-cleared). CLKhi/D0hi change after stream_on. ISTA_FEI synthesized.

**Note on ANA**: Research confirmed Linux uses 0x774→0x770 (CTATADJ=PTATADJ=7), not 0xFF0.
Our 0xFF4→0xFF0 (stronger bias) should also work. The V79 hypothesis that "0x770=weak
termination=STA=0" was incorrect — the real cause was CLK/DAT0 ordering (fixed in V81).

**Commit**: 835677e

## V82–V93 Fix History

### V82 — settle=6 (CLT=DLT=0x0602) — Linux exact timing
IMX708 at ~1.1–1.8 Gbps: HS preamble completes in ~154ns. SYNC byte arrives at t≈154ns.
- settle=21 (0x1502, 210ns): receiver arms AFTER SYNC gone → STA=0 forever
- settle=6  (0x0602, 60ns): receiver arms in preamble → catches SYNC ✓
V81 (CLK after CPR) was necessary; V82 (settle=6) was sufficient. Together = first version with both fixes.

### V83 — CLKGATE without CM_PASSWD
CLKGATE is a CSI1 peripheral register, not a CM register. Write raw value (not 0x5A000000|val).

### V84 — ANA = 0x770 (Linux exact)
Reverted from 0xFF0 (wrong strong bias) to 0x770 (CTATADJ=PTATADJ=7, Linux value).

### V85 — VPU mailbox SET_POWER_STATE(Unicam1=0x0D)
Linux calls pm_runtime_get_sync() before init. Added GET + SET power state via VPU mailbox.
VPU returns state=0x2 — harmless (VPU manages Unicam power via dtoverlay=imx708).

### V86 — mbox[4]=0 (req_resp_indicator, not buf_size)
Linux always sets the req_resp field to 0 for requests. Previously sent mbox[4]=8 → parse error.

### V87 — DC CIVAC flush mbox[] before mbox_call
mbox[] is Normal-cacheable RAM. With D-cache enabled, must DC CIVAC + DSB SY + ISB before VPU call.

### V88 — CLKGATE = lane count (0x01/0x02)
Switched from bitmask 0x05 to lane count. NOTE: this was still wrong — V93 fixes to shift+OR+password.

### V89 — 2-lane mode test (confirmed D1 physically connected)
D1hi=57344 (HS) in POST-STREAM → D1 IS physically connected. V73 "silent" was CPR bug, not hardware.

### V90 — WRONG FIX: UNICAM_DAT_LANES_SHIFT=3 added to CTRL (introduced CCP2 bug)
**Hardware result**: CTRL=0x00080F1B, D1hi=51712 on both lanes (HS active). STA=0 STILL.
Invented "UNICAM_DAT_LANES_SHIFT=3" doesn't exist in Linux. BIT(3)=CPM=1→CCP2 mode.
This is the ROOT CAUSE of STA=0 in V90, V91, V92.

### V91 — IDI0=0x2B (RAW10) + FRAME_W=1920
**Hardware result from V90**: D1hi=51712 HS both lanes confirmed. STA=0.
Root cause identified: IMX708 has NO RAW8 mode. Sensor sends DT=0x2B (RAW10).
IDI0=0x2A (RAW8 filter) drops EVERY pixel packet → STA=0 forever.
Fix: IDI0=0x2B, FRAME_W=1920 (1536px×10bit/8), FRAME_SZ=1,658,880.
**Hardware result**: IDI0=0x2B confirmed, IBLS=0x780=1920 confirmed. STA=0 STILL (CPM=CCP2 bug not yet found).

### V92 — CLKGATE address 0x3F802004 → 0x3F802000
**Hardware result**: CLKGATE readback=0 (write-only, normal). CLK=0x0D00001D (HS confirmed).
D0hi=D1hi=51712 (LP-11, caught in blanking). STA=0 STILL.

### V93 — ROOT CAUSE FIX: CPM=0 (CSI-2) + CLKGATE=0x5A000015
Fixes:
1. CTRL = 0x080F02 (U_CTRL_BASE, no lane_bits) → CPM=0=CSI-2 ✓
2. CLKGATE = 0x5A000015 (shift+OR algo with password, 2-lane)

**Hardware V93 result (2-lane, CPR active)**:
- CTRL=0x00080F03 ✓ (CPM=0=CSI-2 confirmed)
- D-PHY post-stream: CLK=0xE000001D, DAT0=0xE000001D, DAT1=0xE000001D (ALL lanes HS active) ✓
- Frame count via I2C = 52 (sensor streaming at 52fps) ✓
- **STA=0x00000000 PERSISTENT** — CSI-2 byte-sync never achieves lock
- IBWP=IBSA0 (zero DMA writes)
- STA is W1C sticky → STA=0 at every 100ms poll = ZERO Frame Start ever received in 500ms

### V94 — DIAGNOSTIC: 1-lane mode
**Hypothesis**: STA=0 could be 2-lane specific (Unicam lane-count register we haven't found).

Changes:
- IMX708: 0x0114=0x00 (1-lane override)
- Unicam: DAT1=0x00 (D1 disabled), CLKGATE=0x5A000005 (1-lane formula)
- CMP0 write removed (not in Linux start_rx, BIT(31) may interfere)

**Hardware V94 result**:
- 0x0114=0 confirmed (1-lane) ✓
- D1hi=51712=0xCA00 constant (LP-11, D1 disabled correctly) ✓
- D0hi=57344=0xE000 post-stream (D0 HS active) ✓
- frame_count=52 in 1-lane too (sensor outputs same fps either way)
- **STA=0x00000000 STILL** — issue is NOT 2-lane specific; it's fundamental

**Conclusion**: 1-lane and 2-lane both fail identically. Lane count ruled out.

### V95 — Skip CPR (preserve VPU DDL calibration hypothesis)
**Hypothesis**: Our CPR pulse destroys the D-PHY DDL calibration that VPU's dtoverlay=imx708
performs at boot. config.txt notes: "D-PHY analog frontend calibration REQUIRED for MIPI lock."

New data from V95 diagnostics:
- `ANA (VPU): 0x00000777` — VPU left D-PHY powered DOWN (APD=BPD=AR=1)
- `CLK pre-write (VPU state): 0xCD000002` — VPU left CLK in power-down (lower=0x02)
- `DAT0=0x0A000002` — VPU left DAT0 in power-down

Changes:
- CPR skipped entirely (STEP 3 commented out)
- ANA: direct 0x770 write (no 0x774 AR intermediate), to avoid disturbing VPU calibration
- Diagnostic: print CLK/DAT0 VPU state before our writes

**Hardware V95 result**:
- Fast-poll not added yet; periodic 100ms polls: STA=0 throughout
- D0hi=0xE000 post-stream (D0 HS active) ✓
- **STA=0x00000000 STILL** — CPR skip does NOT fix the issue

**Conclusion**: CPR (or no CPR) is irrelevant. VPU DDL calibration hypothesis falsified.

### V96 — AR pulse restored + no CPR (untested combination)
**Hypothesis**: The 0x774→0x770 AR pulse sequence triggers D-PHY DLL calibration. Skipping it
(V95's direct 0x770) leaves the DLL uncalibrated. We had NEVER tested AR pulse + no CPR together.

New data from V96 diagnostics:
- `VPU init state: CTRL=0x00000000 STA=0x00000000 CLT=0x00000000 DLT=0x00000000`
  → VPU leaves ALL Unicam MMIO registers at 0. dtoverlay=imx708 does NOT configure Unicam.
- Fast-poll (50ms, 1ms resolution) immediately after stream_on: `STA_accum=0x00000000`
  → Not even ONE Frame Start event in first 50ms. CSI-2 decoder is completely deaf.

Changes:
- ANA: restored 0x774→0x770 sequence (Linux exact, AR calibration pulse)
- CPR: still skipped
- Init diagnostics: CTRL/STA/CLT/DLT readout before any writes
- Capture loop: fast-poll 50ms at ~1ms resolution before regular 100ms polling

**Hardware V96 result**:
- VPU init state: CTRL=0 STA=0 CLT=0 DLT=0 → confirmed VPU doesn't touch Unicam
- ANA (VPU): 0x777 → we write 0x774→0x770 ✓
- D0hi=0xE000 post-stream (D0 HS active) ✓, D1hi=0xCA00 (D1 disabled) ✓
- Fast-poll: **STA_accum=0x00000000, ISTA_accum=0x00000000** (50ms, 50 samples)
- Periodic polls: STA=0, ISTA=0 throughout 500ms
- **STA=0x00000000 STILL** — AR pulse + no CPR does NOT fix the issue

**Conclusion**: All ANA sequences (AR/no-AR) and CPR states (CPR/no-CPR) exhausted. STA=0 in all.

## Exhausted Hypotheses Matrix

| ANA sequence | CPR | Lane | Result |
|---|---|---|---|
| 0x774→0x770 (AR pulse) | Yes | 2-lane | STA=0 (V84–V93) |
| 0x774→0x770 (AR pulse) | Yes | 1-lane | STA=0 (V94) |
| 0x770 direct (no AR) | No | 1-lane | STA=0 (V95) |
| 0x774→0x770 (AR pulse) | No | 1-lane | STA=0 (V96) |
| 0x770 direct (no AR) | No | 2-lane | NOT YET TESTED |

## Remaining Hypotheses (V97+)

### 1. CM_CAM1 wrong frequency (HIGHEST PRIORITY)
Our CM_CAM1=100MHz (PLLD/5=500/5). Linux might use a different rate. The CLT/DLT settle
counters are timed by CM_CAM1. If Linux uses 200MHz, our settle=6 at 100MHz = 60ns but
Linux's settle=6 at 200MHz = 30ns. At 690Mbps 1-lane: T_HS-ZERO-min ≈ 46ns. If the
settle timer counts from start of HS-0 (not LP-00), 60ns > 46ns → misses SYNC → STA=0.

**V97 action**: Try CLT=DLT=0x0302 (settle=3, 30ns at 100MHz) and 0x0102 (10ns).
Alternatively: set CM_CAM1 to 200MHz (PLLD/2.5 or PLLD/2) and keep settle=6.

### 2. Linux /dev/mem register dump (DEFINITIVE)
Boot Pi OS on the Pi Zero 2W, run camera with libcamera, dump Unicam1 registers at
0x3F801000 via /dev/mem while streaming. Compare ALL register values with ours.

```bash
# On Pi OS with camera working:
sudo apt install python3
sudo python3 -c "
import mmap, struct, os
f = open('/dev/mem', 'rb')
m = mmap.mmap(f.fileno(), 4096, offset=0x3F801000, access=mmap.ACCESS_READ)
regs = ['CTRL','STA','ANA','?','CLK','CLT','DAT0','DAT1','?','?','DLT']
for i,name in enumerate(regs):
    val = struct.unpack('<I', m[i*4:(i+1)*4])[0]
    print(f'[0x{i*4:03X}] {name:6s} = 0x{val:08X}')
# Also print key CSI2 regs
for off,name in [(0x100,'ICTL'),(0x104,'ISTA'),(0x108,'IDI0'),(0x10C,'IPIPE'),
                 (0x110,'IBSA0'),(0x114,'IBEA0'),(0x118,'IBLS'),(0x11C,'IBWP'),
                 (0x400,'MISC')]:
    val = struct.unpack('<I', m[off:off+4])[0]
    print(f'[0x{off:03X}] {name:6s} = 0x{val:08X}')
"
```

### 3. IMX708 register verification
Verify that k_imx708_common[] and k_imx708_init[] are writing correctly by reading back
multiple registers after initialization (not just 0x0114 and 0x0112).
Key registers to check: 0x0101 (mode), 0x0110 (CSI clock), 0x012D (lane speed).

### 4. 2-lane + no CPR (untested)
V96 fixed 1-lane. 2-lane + no CPR has also never been tested. Unlikely to help given
V94 ruled out lane count, but eliminates one cell from the matrix.

## V97 — A2: Settle timing sweep (FALSIFIED)

**Hypothesis**: CLT/DLT settle counter clocks from CM_CAM1. At 100MHz, settle=6=60ns.
If settle timer starts from HS-0 (not LP-00), 60ns > 46ns T_HS-ZERO-min → misses SYNC.

**V97 changes:**
- Settle sweep: try CLT=DLT for settle values 5,4,3,2,1 at 100MHz while sensor streams
- Each value tested ~100ms (5 frame intervals at 52fps)
- A3: IMX708 extended readback (bin_type, fmt, mode)
- V98 bug fix: watchdog_kick() added inside sweep loop (V97 crashed at settle=5 due to watchdog)

**Hardware V97/V98 results:**
- A3 confirmed: `fmt=0x00000A0A` ✓, `bin_en=1` ✓, `bin_type=0x00000022` ✓
  → sensor IS correctly configured 2×2 binned RAW10. Discards sensor misconfiguration hypothesis.
- Settle sweep: STA=0 for ALL values (settle=5=50ns, 4=40ns, 3=30ns, 2=20ns, 1=10ns)
  → Even at 10ns settle (essentially instant), CSI-2 decoder sees ZERO Frame Start packets.

**Conclusion**: Settle timing is DEFINITIVELY NOT the root cause.
The D-PHY byte aligner cannot achieve byte sync regardless of when the receiver arms.

## V99 — V97c: CM_CAM1=250MHz (digital decoder frequency)

**Hypothesis**: CM_CAM1 clocks the CSI-2 digital decoder (not just the settle counter).
At 690Mbps 1-lane, byte clock = 86.25MHz. If the Unicam digital decoder requires ≥200MHz
for correct pipeline operation (some designs have minimum operating frequency constraints),
running at 100MHz might cause the decoder to malfunction.

Also: at 250MHz, settle=6 = 6×4ns = 24ns — shorter than T_HS-ZERO-min=46ns, meaning we
arm correctly inside the HS-0 preamble (unlike 100MHz where settle=6=60ns > 46ns if timer
starts from HS-0 reference point).

**V99 changes:**
- CM_CAM1: PLLD/5=100MHz → PLLD/2=250MHz (DIVI=2, integer division, no MASH needed)
- CLT=DLT: kept at 0x0602 (settle=6 = 24ns at 250MHz — now correctly inside T_HS-ZERO window)
- Settle sweep removed (proven useless in V97/V98)
- If V99 STA>0 → CM_CAM1 frequency was the root cause
- If V99 STA=0 → ALL code-level hypotheses exhausted → MUST do A1 (Linux /dev/mem dump)

**Hardware V99 results (TESTED):**
```
CM_CAM1CTL after: 662 (BUSY=1)
CM_CAM1DIV=8192 = 0x2000 = (2<<12) → 250 MHz CONFIRMED
CLK=0x2D00001D (CLKhi=11520, HS active) ✓
DAT0=0xE000001D (D0hi=57344, HS bursts) ✓
DAT1=0xCA000000 (D1hi=51712, LP-11, 1-lane expected) ✓
CLKGATE readback=0x00000005 (NEW: register stores low bits without password; previously always 0)
STA=0x00000000 PERSISTENT ✗ — same as every version since V81
V99 fast-poll (50ms): STA_accum=0x00000000 ISTA_accum=0x00000000
```

**Conclusion**: CM_CAM1 frequency was NOT the root cause. 250 MHz ≡ 100 MHz in terms of result.

**Complete Exhausted Hypothesis Matrix (12 scenarios, all STA=0):**

| ANA | CPR | Lanes | Settle | CM_CAM1 | STA |
|-----|-----|-------|--------|---------|-----|
| 0x774→0x770 | Yes | 2-lane | 6 | 100MHz | 0 (V84–V93) |
| 0x774→0x770 | Yes | 1-lane | 6 | 100MHz | 0 (V94) |
| 0x770 direct | No | 1-lane | 6 | 100MHz | 0 (V95) |
| 0x774→0x770 | No | 1-lane | 6 | 100MHz | 0 (V96) |
| 0x774→0x770 | No | 1-lane | 5,4,3,2,1 | 100MHz | 0 (V97/V98) |
| 0x774→0x770 | No | 1-lane | 6 | 250MHz | 0 (V99) |

---

## New Hypothesis: VPU Power Domain (UNRESOLVED)

The mailbox power state response has been declared "harmless" without verification.

- GET_POWER_STATE(0x0D): returns `state=0x02`
- SET_POWER_STATE(0x0D, 0x03): returns `state=0x02` (unchanged)

If `bit0=power_on (1=on)` and `bit1=device_exists`, then:
- 0x02 = bit0=0 (OFF) + bit1=1 (exists) → **Unicam1 is NOT powering on**
- Our SET(0x03) should return 0x03 (on+wait) if successful → returning 0x02 = FAILURE

If Unicam1 digital decoder is power-gated:
- MMIO registers accessible ✓ (AXI bus fabric is always-on)
- CLKhi/D0hi counters change ✓ (D-PHY analog frontend is always-on, separate rail)
- STA=0 ✓ (CSI-2 decode engine = frozen/power-gated)

This would explain ALL observed behavior perfectly.

**Test without Pi OS (V100 diagnostic)**:
- Poll GET_POWER_STATE for multiple device IDs (0x00, 0x01, 0x0C=CAM0, 0x0D=CAM1) to understand response encoding
- If CAM0 (0x0C) returns 0x03 after SET but CAM1 (0x0D) still returns 0x02 → the VPU refuses to power on CAM1 on this firmware/hardware combo

---

## V100 Results — Power Domain Sweep + CPR Post-Stream (2026-03-17)

### Power Domain Sweep

| Device | ID | GET state | After SET(0x03) | Meaning |
|--------|-----|-----------|-----------------|---------|
| SD   | 0x00 | 0x01 | — | ON? (bit0=1 but not bit1) |
| UART | 0x01 | 0x00 | — | OFF/unknown |
| USB  | 0x03 | 0x00 | — | OFF/unknown |
| CAM0 | 0x0C | 0x02 | 0x02 | OFF+exists |
| CAM1 | 0x0D | 0x02 | 0x02 | OFF+exists — SET had no effect |
| DSP0 | 0x0E | 0x02 | 0x02 | OFF+exists |

**Conclusion:** SET_POWER_STATE(CAM1, 0x03) returns unchanged 0x02. The VPU firmware accepted the
message but power-on had no visible effect. AMBIGUOUS: SD returns 0x01 (not 0x03), so encoding
may differ from published spec. Cannot determine if 0x02 means "truly power-gated" without
/dev/mem comparison showing working Linux state.

### CPR Post-Stream (V100/V101)

**FALSIFIED**: CPR post-stream → STA=0 both before and after CPR in every frame.
CPR at any time (pre- or post-stream) makes no difference.

**Bug discovered in V100, fixed in V101**: CPR clears ICTL and MISC:
- ICTL: 0x07 → 0x04 (lost FSIE + FEIE)
- MISC: 0x240 → 0x000 (lost FL0 + FL1)
V101 fix: restore ICTL=0x07, MISC|=0x240, re-assert CPE, trigger LIP strobe after CPR.
V101 confirmed: ICTL=0x00000007 MISC=0x00000240 in post-CPR state ✓

---

## V101 Results — ICTL/MISC Fix Confirmed, STA Still 0 (2026-03-17)

```
V101 CPR-post-stream: post-CPR state:
  CLK=0x0D00001D DAT0=0xE000001D ICTL=0x00000007 MISC=0x00000240 CTRL=0x00080F03
V101 fast-poll (50ms): STA_accum=0x00000000 ISTA_accum=0x00000000
```

Frame dumps (Frames 1–5, identical):
- CTRL=0x00080F03, ANA=0x00000770, CLK=0x2D00001D (CLKhi=11520 HS)
- DAT0=0x2600001D (D0hi=9728 HS), DAT1=0xCA000000 (D1 disabled, upper=LP counter)
- IDI0=0x0000002B, IPIPE=0x00000000, IBLS=0x00000780
- STA=0x00000000, ISTA=0x00000000, IBWP=0x42240200=IBSA0 (zero DMA)
- CM_CAM1CTL=662 (BUSY=1), CM_CAM1DIV=8192 (DIVI=2 → 250MHz) ✓

**Note on DAT1=0xCA000000**: We write 0x00 (1-lane disabled). Upper 16 bits = 0xCA00 = 51712 = D1hi
lane counter (live status in upper bits, config in lower bits). This is normal — D1 physically
present in LP-11 even when disabled in config.

**Conclusion after V101 (14 scenarios, STA=0 in all):** All software configuration variables are
correct and all code paths exhausted. The CSI-2 byte decoder never receives a valid SYNC. The
only remaining actions are physical (A4: FFC cable) and diagnostic (A1: /dev/mem in Pi OS).

---

## COMPLETE Exhausted Hypothesis Matrix (V84–V101)

| ANA | CPR | Lanes | Settle | CM_CAM1 | CPR post-stream | ICTL/MISC | STA |
|-----|-----|-------|--------|---------|-----------------|-----------|-----|
| 0x774→0x770 | Yes | 2-lane | 6 | 100MHz | No | Partial | 0 (V84–V93) |
| 0x774→0x770 | Yes | 1-lane | 6 | 100MHz | No | Partial | 0 (V94) |
| 0x770 direct | No | 1-lane | 6 | 100MHz | No | Partial | 0 (V95) |
| 0x774→0x770 | No | 1-lane | 6 | 100MHz | No | Partial | 0 (V96) |
| 0x774→0x770 | No | 1-lane | 5,4,3,2,1 | 100MHz | No | Partial | 0 (V97/V98) |
| 0x774→0x770 | No | 1-lane | 6 | 250MHz | No | Partial | 0 (V99) |
| 0x774→0x770 | No | 1-lane | 6 | 250MHz | Yes (buggy) | Buggy | 0 (V100) |
| 0x774→0x770 | No | 1-lane | 6 | 250MHz | Yes (fixed) | Full ✓ | 0 (V101) |

---

## ~~A4~~ — Physical FFC cable inspection ← COMPLETADO (SIN EFECTO)

Reseated cable (audible click, correct orientation). STA=0 unchanged. Cable ruled out.

---

## ~~A1~~ — Pi OS /dev/mem Ground Truth ← COMPLETADO (2026-03-17)

Ran via `busybox devmem` (Python mmap failed with CONFIG_STRICT_DEVMEM).
**Ground truth with libcamera-vid active:**

| Register | Pi OS value | Our V101 | Difference |
|----------|-------------|----------|------------|
| CTRL | 0x00080F03 | 0x00080F03 | ✓ same |
| STA | 0x00000002 (FS set!) | 0x00000000 | ← |
| ANA | 0x00000770 | 0x00000770 | ✓ same |
| CLK | **0x06000005** | 0x2D00001D | lower=0x0005 (no CLHSE/CLTRE!) |
| CLT | 0x00000602 | 0x00000602 | ✓ same |
| DAT0 | **0xC0000005** | 0xE000001D | lower=0x0005 |
| DAT1 | **0xC0000005** | 0xCA000000 | 2-lane! both active |
| DLT | 0x00000602 | 0x00000602 | ✓ same |
| CMP0 | 0x80000301 | removed V94 | added back V102 |
| ICTL | **0x00D80007** | 0x00000007 | upper DMA enable bits |
| IDI0 | 0x0000002B | 0x0000002B | ✓ same |
| IBLS | 0x00000780 | 0x00000780 | ✓ same |
| IBWP | 0xCBD95000 (moving!) | 0x42240200=IBSA0 | DMA active in Pi OS |
| MISC | 0x00000240 | 0x00000240 | ✓ same |
| CM_CAM1CTL | 0x296 (100MHz!) | 0x662 (250MHz) | |
| CM_CAM1DIV | 0x5000 (DIVI=5=100MHz) | 0x2000 (DIVI=2) | |
| CLKGATE | 0x00000000 | 0x5A000005 | Linux does NOT write CLKGATE |

**CRITICAL FINDING**: Pi OS uses CLK/DAT = 0x0005 (CLE|CLLPE only, no CLHSE/CLTRE).
Linux bcm2835-unicam.c: `use_lp_clock=true` for IMX708 → non-continuous HS clock mode → 0x0005.
Our V84–V102 used 0x001D (added CLHSE+CLTRE, incorrect for non-continuous clock).

**Also**: Pi OS IBWP = 0xCBD95000 = 0xC0000000 | 0x0BD95000 — VideoCore bus address, NOT 0x40000000.

---

## V102 — CMP0 restore ← COMPLETADO (FALSIFICADO, 2026-03-17)

CMP0=0x80000301 restored. Readback=0x80000101 (BIT9 ignored by HW, normal). STA=0.

---

## V103 — Match Pi OS exactly ← COMPLETADO (FALSIFICADO, 2026-03-17)

**Changes**: CLK=0x0005, DAT0=0x0005, DAT1=0x0005 (2-lane), CM_CAM1=100MHz, CLKGATE not written, ICTL=0x00D80007.

**Hardware V103 result:**
```
CTRL=0x00080F03 ✓ ANA=0x00000770 ✓
CLK=0x06000005 (lower=0x0005 ✓, upper=CLKhi counter)
DAT0=0xE0000005, DAT1=0xE0000005 (2-lane, both HS active) ✓
CM_CAM1DIV=20480=0x5000=DIVI=5=100MHz ✓
ICTL=0x00D80007 ✓ MISC=0x00000240 ✓
STA=0x00000000 PERSISTENT ✗
```

ALL Pi OS register values confirmed in hardware. STA=0 persists.

**Conclusion**: The issue is NOT any Unicam register value. Something else differs.

---

## V104 — MIPI clock mode 0x0310=0x00 ← COMPLETADO (FALSIFICADO, 2026-03-17)

**Hypothesis**: k_imx708_init had `{0x0310, 0x01}` = continuous HS clock. Pi OS with CLK=0x0005 (CLLPE, no CLHSE) expects non-continuous clock. Mismatch → CLK HS receiver not enabled → STA=0.

**Change**: `{0x0310, 0x01}` → `{0x0310, 0x00}` (non-continuous HS clock).

**Hardware V104 result:**
```
[IMX708] V104: 0x0310(clk_mode)=0 (expect 0=non-continuous) ✓
CLK=0x06000005, DAT0=0xE0000005, DAT1=0xE0000005 (2-lane, both HS) ✓
STA=0x00000000 PERSISTENT ✗
```

`0x0310=0x00` confirmed. CLK lane now in non-continuous mode (CLKhi drops post-stream as expected). STA=0 remains. Clock mode was NOT the cause.

---

## V105 — IBSA0 bus address 0x40000000 → 0xC0000000 ← EN PROGRESO (2026-03-17)

**Hypothesis**: Pi OS IBWP = 0xCBD95000 = `0xC0000000 | 0x0BD95000` (physical). Unicam is on the VideoCore bus. BCM2837 Linux DT: `dma-ranges = <0xC0000000 0x0 0x40000000>`. The correct DMA bus address for Unicam is `0xC0000000 | phys_addr`, NOT `0x40000000 | phys_addr` (L2-bypass ARM AXI alias).

V18 noted "0xC0000000 → silent DMA fail" but that was with multiple OTHER bugs (MCLK=32kHz, IPIPE=0, etc.) masking the result. With V103's correct configuration, the bus address is the last untested variable.

If Unicam requires valid IBSA0 before the CSI-2 capture engine starts (DMA-readiness gate on STA.FS), using bus address 0x40240200 (unmapped from VC bus perspective) would keep STA=0.

**Change**: `bus_addr = 0xC0000000u | phys_addr` (was `0x40000000u`). Sim PASS ✓.

**Expected UART confirmation**: `IBSA0=0xC2xxxxxx`.

---

## Exhausted Hypothesis Matrix (V84–V104, ALL STA=0)

| Version | ANA | CLK/DAT | Lanes | CM_CAM1 | 0x0310 | IBSA0 alias | STA |
|---------|-----|---------|-------|---------|--------|-------------|-----|
| V84–V93 | 0x770 | 0x001D | 2-lane | 100MHz | 0x01 | 0x40M | 0 |
| V94 | 0x770 | 0x001D | 1-lane | 100MHz | 0x01 | 0x40M | 0 |
| V95 | 0x770 | 0x001D | 1-lane | 100MHz | 0x01 | 0x40M | 0 |
| V96–V98 | 0x770 | 0x001D | 1-lane | 100MHz | 0x01 | 0x40M | 0 |
| V99 | 0x770 | 0x001D | 1-lane | 250MHz | 0x01 | 0x40M | 0 |
| V100–V101 | 0x770 | 0x001D | 1-lane | 250MHz | 0x01 | 0x40M | 0 |
| V102 | 0x770 | 0x001D | 1-lane | 250MHz | 0x01 | 0x40M | 0 |
| V103 | 0x770 | **0x0005** | **2-lane** | **100MHz** | 0x01 | 0x40M | 0 |
| V104 | 0x770 | 0x0005 | 2-lane | 100MHz | **0x00** | 0x40M | 0 |
| V105 | 0x770 | 0x0005 | 2-lane | 100MHz | 0x00 | **0xC0M** | 0 |
| V106 | 0x770 | 0x0005 | 2-lane | 100MHz | 0x00 | 0xC0M | 0 |
| V107 | 0x770 | 0x0005 | 2-lane | 100MHz | 0x00 | 0xC0M | 0 |
| **V108** | 0x770 | 0x0005 | 2-lane | 100MHz | 0x00 | 0xC0M | **0xD001 ✓** |

---

## V106 — CLKGATE @0x3F802004 (CSI1, not CSI0)

**Date**: 2026-03-17

**Discovery**: bcm2835-peripherals.dtsi defines separate clock gates:
- `csi0: reg = <0x7e802000 0x4>` → ARM `0x3F802000`
- `csi1: reg = <0x7e802004 0x4>` → ARM `0x3F802004`

V92 wrongly "corrected" from 0x3F802004 to 0x3F802000 (CSI0). All CLKGATE writes since V92
went to the wrong peripheral.

**HW result**: CLKGATE readback=0x00000015 (value retained at correct address). STA=0 persists.
CLKGATE address was necessary but not sufficient.

---

## V107 — CLKGATE write post-CPE (Linux ordering)

**Date**: 2026-03-17

Moved CLKGATE write from Step 0B (before CPR) to Step 14B (after CPE), matching Linux
`unicam_start_rx()` order: CPR → registers → CPE → clk_write() → MISC → LIP.

**HW result**: CLKGATE post-CPE confirmed. STA=0 persists. Ordering alone not the issue.

---

## V108 — SET_DOMAIN_STATE + SET_CLOCK_RATE ★★★ ROOT CAUSE ★★★

**Date**: 2026-03-17/18

**Discovery**: Linux bcm2835-unicam.c uses `pm_runtime` → `raspberrypi-genpd` →
`SET_DOMAIN_STATE` (mailbox tag 0x00038030), NOT `SET_POWER_STATE` (tag 0x00028001).
- domain=14 = RPI_POWER_DOMAIN_UNICAM1 (DT index 13 + 1)
- Also: SET_CLOCK_RATE(clock_id=4, rate=250MHz) for CORE clock

`SET_POWER_STATE` always returned state=0x02 — wrong tag namespace entirely.
Without domain power, CSI-2 digital decoder was frozen: MMIO accessible, D-PHY active,
but decoder logic unpowered → STA=0, ISTA=0, IBWP stuck.

**HW result — BREAKTHROUGH**:
```
DOMAIN GET resp=0x80000000 domain=14 on=0 OK    ← was OFF!
DOMAIN SET resp=0x80000000 domain=0 on=1 OK     ← now ON
CORE CLK SET resp=0x80000000 rate=250000000 OK

STA_accum=0x0000D001   ← FIRST TIME STA > 0 IN 21 VERSIONS!
ISTA_accum=0x00000005  ← FSI (BIT0) + LCI (BIT2)
```

- STA BIT(0) = Frame Start received ✓
- STA BIT(15) = PI0 (CMP0 Frame End match) ✓
- ISTA BIT(0) = FSI ✓, BIT(2) = LCI ✓
- IBWP advances from 0xC2240200 through buffer — DMA receives real pixel data
- **Noise visible on HDMI screen** — actual sensor data reaching framebuffer

**FEI (BIT1) never fires**: CPR-post-stream diagnostic (from V100 era) fires a D-PHY reset
mid-reception. Before CPR: PI0 present in STA. After CPR: PI0 gone, FEI never fires.
The CPR destroys frame boundary detection.

**⚠️ CODE LOSS INCIDENT**: During simulator testing, `git checkout src/camera_unicam.cpp`
reverted the file to V93 (last committed version), losing ALL V105–V108 changes.
`hardware_sim.h` (V108 fields) survived because it had been read but not checked out.
The file was manually regenerated from conversation context. **Lesson: always commit
before running `git checkout` on modified files.**

---

## V109 — Remove CPR-post-stream + fast-poll

**Date**: 2026-03-18

V108 proved that PI0 (Frame End via CMP0) fires BEFORE the CPR diagnostic, and disappears
AFTER. The CPR mid-reception resets D-PHY → loses frame boundary → FEI never arrives.

**Changes**:
1. No CPR in capture path (completely removed)
2. Clear ISTA/STA (W1C) before each frame capture
3. Fast-poll 50ms: if FEI or PI0 fires during fast-poll, return success immediately
4. All V108 fixes preserved (SET_DOMAIN_STATE, SET_CLOCK_RATE, CLKGATE post-CPE, bus 0xC0000000)

**HW result**: PI0 captured consistently in fast-poll. Pipeline total ~1591ms.
Debayer time ~1100ms due to DMA overwrite (sensor at 52fps continuously overwrites buffer during read).

---

## V110 — DMA stop/restart + RAW10 debayer (A5)

**Date**: 2026-03-18

**Root cause of 1100ms debayer**: DMA continuously streaming at 52fps. Each frame is ~19ms,
so during the ~1100ms debayer read, the buffer is overwritten ~58 times. The debayer reads
a mix of dozens of different frames → visual corruption + black bars.

**Changes**:
1. `stop_unicam_dma()`: clear CPE after PI0/FEI detection → freeze buffer immediately
2. `restart_unicam_dma()`: re-enable CPE + CLKGATE + MISC FL + LIP before next capture
3. `invalidate_frame_dcache()`: DC CIVAC (clean+invalidate) entire frame buffer after DMA stop
4. `camera_debayer.cpp`: complete rewrite from RAW8 to RAW10 packed format
   - `raw10_msb8()`: extract MSB 8 bits from RAW10 packed (group*5+pos)
   - `debayer_raw10_to_chw320()`: RAW10 → float32 CHW 320×320 for YOLO (NEON)
   - `debayer_raw10_to_fb()`: RAW10 → ARGB 480×480 letterboxed in 640×480
   - Center-crop 864×864 from 1536×864, nearest-neighbor scale
5. FS→FE capture protocol: wait FS, clear flags, wait FE/PI0, stop DMA

**HW result**:
- RGB time: **29ms** (down from 1100ms) ✓
- Total pipeline: **526ms** ✓
- PI0 captured consistently
- Orientation: **correct** ✓
- **Barras negras persisten** — horizontal black bands in the image
- **Monocromático** — no color differentiation, grayscale-like output

**Analysis of remaining issues**:
- Black bars: possibly embedded data lines from sensor (first N lines are metadata, not pixels),
  or incorrect byte stride calculation, or partial frame DMA.
- Monochrome: possibly wrong Bayer pattern phase (IMX708 binned might not be RGGB),
  or all RGGB channels reading similar values from same byte position.

---

## V111 — Diagnostic hex dump for black bars + monochrome

**Date**: 2026-03-18

**Changes**: Added post-capture diagnostics to `unicam_capture_frame()` after DMA stop + cache invalidation:

1. **IBWP delta**: print `IBWP - IBSA0` vs expected `FRAME_SZ` (1658880 = 1920×864)
   - If delta < FRAME_SZ: DMA didn't write a complete frame
   - If delta > FRAME_SZ: buffer overflow / wrap-around

2. **Hex dump raw[0..19]**: first 20 bytes of frame buffer
   - Expected RAW10: varied non-zero values in groups of 5 (bytes 0-3 = MSB8, byte 4 = LSBs)
   - If all zeros: cache invalidation failed (DC CIVAC wrote back stale BSS zeros)
   - If all 0xFF or repeated: data format mismatch

3. **Hex dump raw[row100,0..19]**: same at row 100 (skip possible embedded data lines)

4. **nonzero_sample**: 100 diagonal samples — count non-zero bytes
   - 0/100 = cache problem; 100/100 = data present

5. **RGGB pixel decode** at (336,0) and (400,200):
   - If R≈Gr≈Gb≈B: wrong Bayer phase or data not RGGB
   - If values differ: Bayer pattern correct, color issue is in debayer/display

**Hypotheses to falsify**:
- H1: DC CIVAC writes back stale zeros over DMA data → nonzero_sample=0
- H2: Data format is RAW10 unpacked (2 bytes/pixel), not packed (5 bytes/4 pixels) → hex dump shows pattern
- H3: Bayer pattern phase wrong (not RGGB in binned mode) → RGGB values all similar
- H4: Embedded data lines → first rows are metadata, not pixel data

Sim PASS ✓. kernel8.img ready to flash.

### V111 Hardware Results (2026-03-18)

**Capture**: PI0 captured consistently across 5 frames. 52fps confirmed.

**DIAG output** (consistent across all 5 frames):
```
[DIAG] IBWP=0xC238A200 IBSA0=0xC2240200 delta=1351680 expected=1658880
[DIAG] raw[0..19]: 0x11 0x12 0x11 0x12 0x89 0x11 0x12 0x11 0x12 0x...
[DIAG] raw[row100,0..19]: 0x11 0x14 0x11 0x14 0x33 0x12 0x14 0x12 0x14 0x...
[DIAG] nonzero_sample=100/100
[DIAG] RGGB@(336,0): R=18 Gr=20 Gb=20 B=18
[DIAG] RGGB@(400,200): R=18 Gr=20 Gb=20 B=17
```

**Hypothesis results**:
- **H1 FALSIFIED**: nonzero_sample=100/100 → DC CIVAC is NOT writing stale zeros. Data is present.
- **H2 FALSIFIED**: 5-byte repeating pattern `[0x11 0x12 0x11 0x12 0xNN]` is textbook RAW10 packed (4 MSB8 + 1 packed LSB byte). Format is correct.
- **H3 PARTIALLY CONFIRMED**: RGGB values nearly identical (R≈18, Gr≈20, Gb≈20, B≈18) — but this is because ALL pixels are at black level, not because Bayer phase is wrong. Pattern is R < G > B consistent with RGGB (green channels slightly higher = expected dark current difference).
- **H4 NOT TESTED**: Row 0 and row 100 show identical values — if row 0 were embedded data, it would have different byte patterns. Not conclusive.

**Key findings**:

1. **DARK IMAGE — sensor at black level**: MSB8 values 17-20 → 10-bit values ~68-80. IMX708 black level ≈ 64 (10-bit). Signal is only 4-16 DN above pedestal. Despite 96% exposure time (0x0486/0x04B6 = 1158/1206 lines) and 1.12x gain (0x0070), sensor outputs near-darkness. Values are UNIFORM across entire frame (row 0 = row 100 = row 200, col 336 = col 400) → no spatial variation = no photons detected.

2. **SHORT FRAME — 704 of 864 lines**: delta=1351680 / 1920 bytes/line = exactly 704 lines. 160 lines missing (864 - 704 = 160). PI0 fires at 704 lines → frame end detected early. Consistent across all 5 frames (identical delta every time).

3. **5th byte varies between frames** (0x89, 0x59, 0xDD, 0xAA, 0xEA): these are the packed 2-bit LSBs of dark current noise — expected behavior for near-black pixels with shot noise.

**New hypotheses**:
- **H5**: Sensor exposure/gain registers not taking effect (registers written in standby but not applied after stream_on)
- **H6**: Sensor is in test/standby mode due to undocumented register interaction
- **H7**: Optical path blocked (lens/FFC mechanical issue) — less likely (FFC verified A4)
- **H8**: Short frame due to embedded data lines or MIPI HS timing mismatch causing line loss

---

## V112 — Test pattern + max gain + extended multi-row DIAG

**Date**: 2026-03-18

**Goal**: Determine whether the dark image is caused by the sensor (optical/exposure) or the data path (CSI-2/DMA corruption).

**Plan**:

### Change 1: IMX708 test pattern (color bars)
Write `0x0600 = 0x02` (color bar test pattern) after mode registers, before stream_on.
- **If we see varied pixel values (color bars)**: data path is correct. Problem is sensor optical/exposure. → Fix: boost gain, verify exposure registers, check lens.
- **If we still see dark/uniform pixels**: data path is corrupting or dropping pixel data. → Investigate Unicam IDI0 filtering, IBLS alignment, DMA coherency.

### Change 2: Boost analog gain to maximum
Write `0x0204 = 0x03, 0x0205 = 0xC0` → ANALOG_GAIN = 0x03C0 = 960.
Gain = 1024/(1024-960) = 16x (was 1.12x at 0x0070).
This is secondary to the test pattern — will take effect when test pattern is disabled.

### Change 3: Extended multi-row DIAG
Sample raw bytes at rows 0, 100, 350, 700, 800 to characterize:
- Whether bottom rows (700, 800) are zeros (short frame → DMA didn't reach them)
- Whether there's any spatial variation in pixel values
- Print delta in lines (`delta / FRAME_W`) for clarity

### Change 4: Print test pattern register readback
After writing 0x0600, read it back and print to confirm I2C write took effect.

**Expected UART output for PASS (test pattern working)**:
```
[IMX708] V112: test_pattern=2 (expect 2=color bars)
[DIAG] delta=N lines (expected 864)
[DIAG] raw[0..19]: varied values with clear pattern structure
[DIAG] RGGB@(336,0): R≠Gr≠B (distinct color bar values)
```

**Expected UART output for FAIL (data path issue)**:
```
[DIAG] raw[0..19]: 0x11 0x12 0x11 0x12 ... (unchanged from V111)
[DIAG] RGGB@(336,0): R≈18 Gr≈20 Gb≈20 B≈18 (still at black level)
```

---

## V113 — Gain 16× activo, test_pattern=0, imagen verde (2026-03-18)

```
[IMX708] test_pattern=0 (off)  gain=0x03C0 (16×) ✓
[POST-STREAM] ISTA=5 WP=0xC238A200  (704 líneas, igual V111)
[CAM] frame OK (wrap, 0 lines overwritten)
```

Imagen: predominantemente verde, barras horizontales del frame anterior en el bottom.
Pipeline: 526ms estable. Reset periódico por watchdog timeout entre frames.

**Hallazgos:**
- Frame corto 704/864 líneas persiste — PI0/LCI dispara antes del Frame End real
- Verde dominante con 16× gain → datos presentes pero Bayer phase incorrecto
- libcamera reportó `SBGGR10_1X10/RAW` → sensor es **BGGR, no RGGB**
- ISTA=5 = FSI(BIT0)+LCI(BIT2) — FEI(BIT1) nunca aparece en ningún frame
- Barras del frame anterior = bottom 160 líneas stale por short frame
- Watchdog reset: `watchdog_kick()` no está dentro del polling loop de captura

---

## V114 — Plan: watchdog + IBWP wait + Bayer BGGR

### Fix 1: Watchdog en capture polling loop
```cpp
for (int t = 0; t < 500; t += 10) {
    watchdog_kick();
    if (ISTA & (FEI | PI0)) break;
    delay_ms(10);
}
```

### Fix 2: Frame length — esperar IBWP >= IBSA0 + FRAME_SZ
```cpp
uint32_t deadline = get_time_ms() + 100;
while (get_time_ms() < deadline) {
    watchdog_kick();
    if (UNICAM_READ(IBWP) >= ibsa0 + FRAME_SZ) { complete = true; break; }
    delay_ms(2);
}
uint32_t lines = (UNICAM_READ(IBWP) - ibsa0) / FRAME_W;
uart_printf("[DIAG] frame lines: %u/864\n", lines);
```

Si IBWP nunca llega a IBSA0+FRAME_SZ → sensor genuinamente envía 704 líneas.
Si llega → PI0 disparaba prematuramente.

### Fix 3: Bayer phase BGGR (libcamera: SBGGR10_1X10)
En debayer swap canales R↔B. BGGR:
```
(row par,   col par)   → B  (era R en RGGB)
(row par,   col impar) → G
(row impar, col par)   → G
(row impar, col impar) → R  (era B en RGGB)
```

---

## V114 — HW: IBWP polling → 864/864 líneas, YOLO detecta objetos reales

IBWP polling reemplaza CMP0/PI0. Frame completo: `wrap, 0 lines overwritten`.
YOLO detecta bench (class 13, ~54% confidence) en frame real de cámara.
Frame corto (704/864 líneas) estaba causado por PI0 prematuro en CMP0 (line 704).

**UART output representativo:**
```
[CAM] frame OK (wrap, 0 lines overwritten)
>>> OBJETOS DETECTADOS <<<
  bench: 54.2%  [cx=160 cy=290 w=320 h=58]
```

---

## V115 — HW: LIP-after-FS probado, barras persisten parcialmente (2026-03-18)

LIP se re-dispara después de FSI para resetear IBWP a IBSA0. Se confirma:
`wrap, 0 lines overwritten` en cada frame → buffer limpio.

**UART output (V115 HW, frames 67-68 luego reset):**
```
=== YOLOv5n DECODER ENGINE [Frame: 67] ===
[CAM] FSI detected — LIP re-triggered
[CAM] frame OK (wrap, 0 lines overwritten)
...
=== YOLOv5n DECODER ENGINE [Frame: 68] ===
[CAM] FSI detected — LIP re-triggered
[CAM] frame OK (wrap, 0 lines overwritten)
... (reset del sistema por watchdog timeout)
=== Direct2Metal: MOTOR IA EN TIEMPO REAL ===
=== YOLOv5n DECODER ENGINE [Frame: 1] ===
```

**Hallazgos:**
- Reset periódico: watchdog a 4s, throttling térmico en Zero 2W puede superar ese límite
- Barras horizontales: posiblemente artefactos de color BGGR, no staleness (buffer limpio)
- Imagen verde dominante: BGGR no corregido en V115

---

## V116 — Plan + Implementación (2026-03-18)

### Bug 1 — Watchdog reset → RESUELTO
**Root cause**: Un solo `watchdog_kick()` al inicio del loop. Inferencia ~526ms + throttling térmico puede superar 4s.

**Fix V116**: Timeout 4s → 8s + 3 kicks adicionales durante backbone/neck:
```cpp
if (get_timer_freq() != 62500000UL) watchdog_init(8000);  // 8s
// En run_yolo_complete():
watchdog_kick();  // al inicio del frame
watchdog_kick();  // después de L4 (mid-backbone)
watchdog_kick();  // después de backbone (SPPF)
watchdog_kick();  // después de neck
```

### Bug 2 — Color verde dominante → RESUELTO
**Root cause**: V109-V115 asumía RGGB. libcamera reporta `SBGGR10_1X10/RAW` para IMX708 en modo 1536×864 → primer pixel es B, no R.

**Fix V116** en `camera_debayer.cpp` — todos los paths (`debayer_raw10_to_chw320`, `debayer_raw10_to_fb`, `debayer_raw10_row_rgb`):
```cpp
// Antes (RGGB — incorrecto):
uint8_t R  = raw10_msb8(row0, src_x);
uint8_t Gr = raw10_msb8(row0, src_x + 1);
uint8_t Gb = raw10_msb8(row1, src_x);
uint8_t B  = raw10_msb8(row1, src_x + 1);

// Después (BGGR — correcto):
uint8_t B  = raw10_msb8(row0, src_x);
uint8_t Gb = raw10_msb8(row0, src_x + 1);
uint8_t Gr = raw10_msb8(row1, src_x);
uint8_t R  = raw10_msb8(row1, src_x + 1);
```

### Feature — SD card frame save → IMPLEMENTADO
`src/sdcard.cpp` (nuevo): EMMC BCM2837 @0x3F300000 + FAT32 mínimo + PPM P6.
- `sdcard_init()`: CMD0→CMD8→ACMD41→CMD2→CMD3→CMD7→CMD16 → MBR → BPB
- `sdcard_save_ppm()`: escanea root dir por "FRAME   PPM" (8.3), escribe fila por fila (512B staging buffer), llama `debayer_raw10_row_rgb()` por fila
- Preparar SD card: `python3 -c "hdr=b'P6\n480 480\n255\n'; open('FRAME.PPM','wb').write(hdr+bytes(480*480*3))"`
- Leer resultado: `cp /boot/firmware/FRAME.PPM /tmp/ && eog /tmp/FRAME.PPM`

**Build V116**: sim OK (3 frames, QEMU exit 0).

**Pendiente**: flash HW y verificar colores correctos + no watchdog resets.

