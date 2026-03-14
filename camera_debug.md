# Phase 9 — Camera Debug History (V17–V93)

## Confirmed Hardware Facts

| Fact | Evidence |
|------|----------|
| D0/D1 physically crossed on Pi Zero 2W FFC | V17(LSM=1)→WP+4096 vs V19(LSM=0)→WP=IBSA0 |
| DMA bus alias must be 0x40000000 (ARM AXI) | 0xC0000000 (VC GPU bus) → silent DMA fail (V18) |
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
**Status**: Code written, NOT YET TESTED ON HARDWARE. Last git commit = V89 (a817135).
V90-V93 changes are in working tree, uncommitted.

Fixes:
1. CTRL = 0x080F02 (U_CTRL_BASE, no lane_bits) → CPM=0=CSI-2 ✓
2. CLKGATE = 0x5A000015 (shift+OR algo with password, 2-lane)

Expected: CTRL=0x00080F03 in diagnostic, STA>0 (FS/FE set), IBWP advancing.

**Known inconsistency in code**: `hardware_sim.h` lines 59-64 still describe BIT(3)/BIT(4) as
"lane enable bits" (V90 interpretation). Comments are wrong per V93. The simulator runtime
check (line ~1002) correctly checks CPM=0, but the #define comments are misleading.
Also: UnicamSimState.clkgate_enabled comment still says "0x3F802004" (should be 0x3F802000).
