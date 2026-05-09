# camara_direct — Progreso y Retos

**Plataforma:** Raspberry Pi Zero 2 W (BCM2837, 4×Cortex-A53). Bare-metal AArch64, sin sistema operativo.
**Cámara:** Pi Camera v3 (Sony IMX708), CSI-2 a 2 lanes.
**Estado actual:** **V162** — captura binned 1536×864 → HDMI 1920×1080, debayer en los 4 cores con core 0 entrelazado, ISP completo (BLC/WB/CCM/gamma/LSC) byte-exact con libcamera, AE en CIT con feedback P-controller, double-buffer FB. Validado en HW.

**Throughput medido (V162):**
```
TIMING wait=21 sync=0 inval=0 disp=0 total=21 ms  fps=47.61  ae_cit=1110
```
Bottleneck es el sensor (FS-to-FS = 21 ms), debayer 4-core entra dentro de la ventana de wait. Buffer DMA 1.58 MB.

**Diff vs Linux libcamera ground truth (`linux_extract/DIFF_REPORT.md`):** de 125 regs faltantes en V148, quedan **~17** y todos están en bloques deliberadamente saltados (PDAF disabled, test pattern off, 2 misc enable bits). El init I2C de V162 cierra la brecha al 86%.

**Tearing residual en movimiento:** seam horizontal visible en objetos que se mueven rápido. Causa documentada: mailbox `SET_VIRTUAL_OFFSET` (0x48009) tarda ~4 ms vs vblank HDMI ~0.67 ms → el escribe del HVS por la VPU cae en active scan. **Límite del firmware Pi3, no del bare-metal.** Detalle en §"V162 double-buffer FB". Solución requiere escritura directa al HVS (sin datasheet público) o GIC IRQ vsync — ambas diferidas.

---

## Logros

### Captura CSI-2 byte-exact vs Linux libcamera (V108 → V154)

- **V108:** root cause de 21 versiones previas — `SET_DOMAIN_STATE(domain=14)` enciende el decoder Unicam1. `SET_POWER_STATE(0x0d)` no lo hace.
- **V147:** transporte Unicam/CSI-2/DMA verificado **byte-perfect** con un test pattern sólido de 4 valores distintos por canal — todos los 704 rows coincidían exactamente con los bytes esperados.
- **V150 IMX708 init:** la tabla `k_imx708_full[]` (266 escrituras) coincide **265/265** con la traza I2C real de libcamera a 4608×2592 (`tools/diff_imx708_full.py`):

  ```
  == AE/AGC final values ==
    CIT=0x09E7  AGAIN=0x03C0(16x)  DGAIN=0x0100  FLL=0x0A5A  ORIENT=0x03  → ok
  ```

- **V150 Unicam runtime:** todos los registros D-PHY, ICTL, CMP0, IDI0 coinciden con el dump runtime de Linux. Sólo difieren `IBSA0/IBEA0/IBLS` (geometría de buffer dependiente del modo).

### V154: detección de fin-de-frame Linux-aligned (FS-a-FS)

V148–V153 usaban un heurístico "5000 lecturas estables de IBWP = VBLANK" que disparaba en falsos positivos cuando el sensor enviaba paquetes cortos PDAF/embedded en otra VC (IDI0 los filtra → IBWP no se mueve durante ms). Resultado: DMA paraba mid-frame en líneas variables (1750/1738/1742) → filas torcidas que el debayer renderizaba como **líneas magenta horizontales que se movían entre frames**.

V154 porta el protocolo FS-a-FS del proyecto padre (`camera_unicam.cpp:1409`), idéntico al que usa libcamera + `bcm2835-unicam.c`:

```
poll(ISTA) — esperar primer FSI (FS1) → trigger LIP (latch IBSA, reset WP)
            — entre FS1 y FS2: trackear max(IBWP) cada 256 iteraciones
            — esperar segundo FSI (FS2) → CPE=0 (congelar buffer)
```

El borde es la señal de inicio de frame del propio sensor — no hay falsos positivos posibles. **Imagen estable, sin líneas magenta.**

### Zero-fill del tail no cubierto por DMA

El BCM2837 Unicam tiene un límite pre-existente de cobertura ~81% (V146/V147) — algunos paquetes cortos PDAF/embedded cuentan contra el contador de líneas sin escribir al buffer. Sin tratamiento, el tail muestra datos del frame N-1 (verde fosforescente / púrpura). Solución: tras el snapshot, zero-rellenar `snap[bytes_valid..FRAME_BYTES]` → la franja inferior renderiza limpia en negro.

### Pipeline de imagen calidad libcamera

Constantes portadas verbatim del archivo de tuning libcamera (`linux_extract/tuning/imx708.json`) y del DNG real:

- **BlackLevel** = 64 (10-bit), pedestal = 16 en MSB8.
- **AsShotNeutral** = [0.4784, 1.0, 0.5629] → WB Q8 = (535, 256, 455).
- **CCM 4640K** (daylight): 9 coeficientes Q10 signed.
- **Gamma sRGB** vía LUT de 256 entradas (aproxima `gamma_curve` de libcamera).

### Bug del "line swap" resuelto

V148 mostraba bytes mezclados en escenas reales pero no en test patterns. Causa root encontrada al diff-ear contra el dump Unicam runtime de libcamera: `DAT1 = 0x06000005` (terminación HS clock-pattern) — Linux usa este valor para la lane 1, no `0xC0000005` como la lane 0. Aplicado en V150 → escenas reales limpias.

### Bug del "verde fosforescente" en blancos resuelto

NEON `vmul_n_u16` truncaba al multiplicar `R(255) × WB_R(535) = 127865`, que excede u16 max 65535. Wrap → R/B basura cerca de 0, G correcto a 255 → CCM produce R=0,G=255,B=0. Fix: ensanchar a u32 con `vmovl_u16 + vmulq_n_u32` antes del shift.

### Performance

| Versión | wait | snap | debayer | total | fps |
|---------|-----:|-----:|--------:|------:|----:|
| V150 escalar single-core | 25 ms | 18 ms | 218 ms | ~275 ms | 3.6 |
| + lookup tables (`g_row_off`, `g_byte_off`) | 25 | 18 | ~190 | ~245 | 4.1 |
| + NEON 4-wide CCM (`smlal v.4s`) | 22 | 18 | 95 | 135 | **7.40** |
| + multi-core debayer (4×Cortex-A53, 270 rows/core) | 18 | 23 | 24 | 65 | 15.15 (medido pre-FS-a-FS) |
| V154 FS-a-FS (espera real frame boundary) | 93 | 18 | 24 | 135 | **7.40** (sostenido) |
| V156 = V154 sin V155 crop, baseline actual | 93 | 18 | 24 | 135 | **7.40** |
| V157 (revertido) double-buffer software | 117 | 18 | (en paralelo) | 135 | 7.40 |
| V159 binned 1536×864 + lane-swap fix | 21 | — | 4 | 25 | **40** |
| V160 4-core debayer + band-while-wait | 21 | — | (paralelo) | 21 | **47.6** |
| V161 + ISP linux + AE on CIT | 21 | — | (paralelo) | 21 | **47.6** |
| V162 + double-buffer FB | 21 | — | (paralelo) | 21 | **47.6** |

NEON aplicado: gather scalar (no hay gather en ARMv8-A), pero pedestal subtract + WB + CCM (3×SMLAL chains) + clamp en vectores 4-wide. Gamma escalar (LUT 256 no cabe en `tbl`).

Las mediciones "15 fps" tempranas (V152) fueron tomadas con polling IBWP-stable que paraba DMA antes del frame end, por lo que el wait era artificialmente bajo. El throughput sostenido con FS-a-FS y captura completa es 7.40 fps; ver §"Período del sensor — finding V157".

### V159 lane-swap regression — origen identificado (fix pendiente)

V159 introduce una regresión visual: el "lane swap" — escenas reales muestran bytes mezclados a nivel de línea (test patterns uniformes no lo revelan). Es exactamente el bug que **V150 había resuelto en full mode** mediante el descubrimiento de `DAT1 = 0x06000005` (clock-pattern HS termination, vs `0xC0000005` data-pattern de DAT0).

**Origen identificado:** `0x0310 = 0x00` en `k_imx708_binned[]` (`src/imx708_regs.h`).

**Análisis:**

El registro `0x0310` controla la continuidad del HS clock del IMX708:
- `0x00` = no-continuo: el clock lane cae a LP (low-power) entre bursts de líneas.
- `0x01` = continuo: el clock lane se queda en HS todo el tiempo.

Los dumps libcamera (`linux_extract/registers/imx708_writes_*_unique_final.txt`) muestran `0x0310 = 0x01` en **AMBOS modos**:

```
$ grep "0x0310" linux_extract/registers/imx708_writes_*_final.txt
imx708_writes_binned_1536x864_unique_final.txt:0x0310 = 0x01
imx708_writes_full_4608x2592_unique_final.txt:0x0310 = 0x01
```

camara_direct V158 full-mode usaba `0x01` (libcamera default) y funcionaba limpio. **V159 heredó el override `0x00` del proyecto padre** al portar sus tablas binned (`Direct2Metal/src/imx708_regs.h:k_imx708_init[]`), donde el override está marcado como "V104 HW-verified" — pero ese flag corresponde al contexto de debug del padre en V104, NO a una validación libcamera.

**Cross-check con readback Unicam:**

| Reg | V158 (full, OK) | V159 (binned, lane swap) |
|------|----------------|--------------------------|
| `0x0310` | `0x01` (continuous) | `0x00` (non-continuous) |
| Unicam `DAT0` readback | `0xC0000005` | `0x06000005` |
| Unicam `DAT1` readback | `0xC0000005` | `0x02000005` ← **bit 26 perdido** |

`DAT1` perdió bit 26 (lane HS-active status). Mecanismo plausible: con `0x0310=0x00` el clock lane oscila entre HS y LP entre bursts; la auto-termination del BCM2837 en lane 1 (configurada como clock-pattern HS termination = `0x06000005`) no consigue re-engancharse limpiamente en cada burst, perdiendo sincronía. Lane 0 (data-pattern termination = `0xC0000005`) sí re-engancha. Resultado: lane 1 entrega bytes desincronizados respecto a lane 0 → "lane swap" visible.

**Fix aplicado y VALIDADO EN HW** (2026-05-07):

```c
// src/imx708_regs.h, dentro de k_imx708_binned[]:
{ 0x0310, 0x01 },   // libcamera default; era 0x00 (override V104 del padre)
```

UART log post-fix:
```
reg=0x00000310 val=0x00000001    ← era 0x00
DAT1 = 0x06000005                ← termination clock-pattern reaplicada limpia
TIMING wait=18 sync=7 ... fps=38.46
```

Misma corrección que V158 ya tenía implícita en su `k_imx708_full[]` por descartar el override del padre. Imagen real ya no muestra el lane swap de bytes mezclados.

**Lección portable:** los overrides marcados "VNNN HW-verified" del proyecto padre son contexto del padre, no validaciones libcamera. Cuando portemos cualquier tabla del padre a camara_direct, diff contra `linux_extract/registers/imx708_writes_*_unique_final.txt` y mantener el valor de libcamera salvo que un test HW de camara_direct demuestre lo contrario.

---

### Período del sensor — finding V157

V157 intentó solapar `debayer` con `wait` usando dos snap buffers + dispatch async (cores 1-3) + core 0 dedicado a wait+snap. Resultado medido en HW (2026-05-06):

```
TIMING wait=93  snap=18 sync=0 disp=0 total=112 ms  fps=8.92    ← iter 1 (con prime)
TIMING wait=117 snap=18 sync=0 disp=0 total=135 ms  fps=7.40    ← iter 2+
TIMING wait=116 snap=18 sync=0 disp=0 total=135 ms  fps=7.40
```

`sync=0` confirma que el debayer (32 ms en 3 cores) sí terminaba antes que el wait. **Pero el wait creció exactamente 24 ms = el debayer "absorbido" en V156**. Conclusión: el `wait=93 ms` que medíamos en V156 era engañoso — el período físico real del sensor a la configuración actual (`FLL=0x0A5A`, `LLP=0x3D20`, 2-lane, link clock IMX708 full-mode) es **117 ms**. V156 lo escondía porque su debayer corría DESPUÉS del rearm y antes del wait, consumiendo 24 ms del frame period antes de que la función de wait fuera siquiera llamada.

**Ley de conservación:** mientras tengamos
- 1 frame del sensor por ciclo → 117 ms inevitables, físicos,
- 1 memcpy raw_buffer → snap → 18 ms con DMA pausada,

el techo es `1 / (117+18) = 7.40 fps`, sin importar cómo se reordenen las fases en software. V157 fue revertido y V156 queda como baseline limpia para construir cualquier mejora real.

**Por dónde sí se puede subir** (listado completo en §"Próximos Retos"):

| Camino | Ganancia | Riesgo |
|--------|---------:|--------|
| (a) IBSA1/IBEA1 hardware double-buffer (elimina los 18 ms de snap) | 7.40 → 8.55 fps (+15%) | medio: latch de IBSA1 al FE no documentado en el chip |
| (b) Reducir FLL/LLP (acortar el frame period del sensor) | algunos ms si hay margen | alto: puede romper la temporización D-PHY o timing-critical de libcamera tuning |
| (c) Modo binned 1536×864 (lo que hace el padre) | ~52 fps native, snap ~2 ms | alto: nueva tabla `k_imx708_full[]`, nuevo ratio en debayer (upscale en lugar de downscale), revalidar línea/lane timing |

### Multi-core wake + per-core MMU (foundation)

`mmu.cpp` dividido en `mmu_build_table()` (privado, solo core 0) y `mmu_enable_this_core()` (público, per-core: TLBI, MAIR, TCR, TTBR0, SCTLR — todos banked). `start.S` ahora maneja cores 1-3:

- `secondary_spin`: poll en `0xD8 + core_id*8` (spin-table del armstub Pi 3+, también funciona en QEMU).
- `secondary_entry`: drop EL2→EL1 + `CPACR_EL1.FPEN=0b11` + per-core stack 16 KB + llamada a C `secondary_main(core_id)`.
- 48 KB reservados en BSS para los stacks secundarios.

**Gotcha clave:** core 0 escribe la entry-point al spin-table con MMU on (mapping cacheable), así que el store se queda en L1 y nunca llega a RAM. Cores 1-3 con D-cache off lo leen como 0 → spin para siempre. Fix: `dc civac` explícito en cada slot tras escribir, antes del `dsb sy; sev`. Sin esto el wake-up nunca dispara.

### Reset hard del IMX708 ante estados stuck

`camera_gpio_override_high()` ahora pulsa GPIOs 40/41/42/44 a LOW durante ~50 ms (descarga regulador + latch del sensor en reset) y luego HIGH durante ~10 ms (boot interno). Esto recupera el sensor cuando un boot anterior lo dejó mid-stream — el síntoma era NACK persistente en el probe I2C aunque el cable estuviera bien reasentado.

### Infrastructure

- **Diff estático IMX708** (`tools/diff_imx708_full.py`): bare-metal vs traza Linux, registro a registro.
- **Diff runtime** (`tools/diff_runtime_full.py`): UART log vs dump Linux runtime, marca diferencias esperadas vs reales.
- **Makefile con tracking de headers** (`-MMD -MP`): un cambio en `imx708_regs.h` ahora rebuilda `imx708.o` automáticamente. (Sin esto, los valores AE corregidos no llegaron al binario en V150 inicial.)
- **TIMING per-frame** vía `CNTPCT_EL0`: `wait`, `snap`, `debayer` por separado en cada UART line.

---

## Próximos Retos

### 0. ✅ Recuperar la franja negra inferior — HECHO (V154/V156)

V155 intentó esto vía crop+stretch asumiendo que la cobertura DMA estaba limitada al 67% (basado en logs de V153). Resultó que **V154 FS-a-FS ya había desbloqueado cobertura 100%** (`rows_max=0xA20=2592`) — la "limitación" era el polling IBWP-stable parando DMA mid-frame, no un límite de hardware. V156 revierte el crop de V155 → debayer con ratio nativo 6/5 que cubre los 2592 rows reales del sensor a 1080 HDMI rows con aspecto 16:9 correcto.

### 1. ✅ Multi-core debayer task pool — HECHO (V152, ~15 fps)

`debayer_to_fb` tomaba 95 ms en un Cortex-A53. Repartido en 4 bandas (270 rows/core): ~24 ms efectivo. `mc_dispatch_debayer` publica `g_task_epoch++` con RELEASE; cores 1–3 esperan en WFE, leen con ACQUIRE, ejecutan su banda, incrementan `g_task_done` con RELEASE, sev. Core 0 hace su banda en paralelo y polea `g_task_done == 3`. Memoria del `snap` en Normal Inner Shareable cacheable → SCU snooping garantiza coherencia.

### 2. ✅ FS-a-FS frame-end — HECHO (V154)

Reemplazo del polling IBWP-stable por protocolo de interrupciones del sensor (FS1 → LIP → FS2 → CPE=0). Eliminó las líneas magenta y la variabilidad de `rows_max`.

### 3. Continuous DMA via IBSA0 rotation — IMPLEMENTADO en V158

**Ganancia esperada: 7.40 → ~8.55 fps (+15%).** En implementación / pendiente de medir HW.

#### Por qué IBSA0 rotation y NO el "hardware double-buffer" via IBSA1/IBEA1

La inspección de `linux_capture_linux/unicam_run.txt` y `unicam_run2.txt` (dos snapshots reales del Unicam mientras libcamera capturaba a 4608×2592) reveló que **`bcm2835-unicam.c` NO programa IBSA1/IBEA1**:

```
unicam_run.txt    (frame N):    IBSA0=0xCC300000   IBEA0=0xCCC95000
unicam_run2.txt  (frame N+ε):   IBSA0=0xCC500000   IBEA0=0xCC695000   ← distinto!
```

IBSA1/IBEA1 (offsets 0x304/0x308 según `parent/src/hardware_sim.h`) no aparecen en ningún dump. **Las direcciones de IBSA0 cambian entre frames** → libcamera está rotando el IBSA0 cada frame, no usando ping-pong hardware.

El `Double-buffer section (single-buffer: do NOT write these)` en hardware_sim.h del padre era una conjetura: el TRM del BCM2837 no es público y la interpretación más plausible dado el comportamiento de libcamera es que **IBSA1/IBEA1 son para un segundo canal DMA independiente** (e.g. embedded data / PDAF en otro VC/DT con su propio IDI1 — no programado en nuestro pipeline). Usarlos como ping-pong tiene riesgo alto de comportamiento indefinido.

V158 sigue la receta libcamera-aligned: dos raw buffers físicos, IBSA0 reescrito + LIP en cada FS.

#### Diseño V158

Memoria:
```
RAW_A  0x01000000 .. 0x01E3D000   (14.24 MB ping)
RAW_B  0x02000000 .. 0x02E3D000   (14.24 MB pong)
```

API nueva en `unicam.{h,cpp}`:
```cpp
void     unicam_stage_dma_buffer(void* buf);      // escribe IBSA0/IBEA0; no LIPea
uint32_t unicam_wait_fs_and_lip();                // espera 1 FSI, captura IBWP, LIPea
                                                  // retorna IBWP pre-LIP (rows_max diag)
```

Loop steady-state:
```
1. unicam_stage_dma_buffer(next_buf)              // pre-stage: IBSA0 = OTRO buffer
2. unicam_wait_fs_and_lip()                       // 117 ms = período sensor
3. swap: prev_buf = active_buf; active_buf = next_buf
4. mc_wait_debayer_done()                         // sync con dispatch del iter anterior
5. dcache_invalidate_range(prev_buf, FRAME_BYTES) // DMA escribió bypassing cache
6. mc_dispatch_debayer_async(prev_buf, fb, ...)   // 32 ms en cores 1-3, oculto en el wait
```

CPE nunca se apaga. snap memcpy eliminado (18 ms). Per-frame: `max(117, 32) = 117 ms = 8.55 fps`.

Cores: 1, 2, 3 corren `secondary_main` con bandas `BAND_COUNT=3` (FB_H/3 = 360 rows/core exactos). Core 0 hace toda la I/O. El antiguo `mc_dispatch_debayer` bloqueante se reemplazó por `mc_dispatch_debayer_async` + `mc_wait_debayer_done` (mismo split que V157 que era el correcto, sólo que aquí sí da ganancia porque elimina el snap, no sólo reordena).

#### Riesgos a vigilar en HW

- **IBWP wraparound entre LIPs**: si DMA sigue corriendo durante el snap, antes IBWP llegaba a IBEA0 y wrappeaba a IBSA0 → over-escribía el comienzo del frame. Aquí el FS dispara LIP inmediatamente, por lo que IBWP se resetea ANTES de over-escribir. Pero si `wait_fs_and_lip` se atrasa (debayer-induced contention) podría haber wrap. La medición `ibwp_pre_lip` lo expone: si > FRAME_BYTES, pasó.
- **Memory contention durante el debayer**: V157 mostró que cores 1-3 leyendo 14.93 MB + escribiendo 8.3 MB durante 32 ms saturan el bus SDRAM. El DMA del Unicam es modesto (~128 MB/s) pero podría rezagarse. La medición `wait` lo detecta — si crece de ~117 ms a ~141 ms, hay que mitigar (reducir bandas a 4 cores con core 0 esperando, o pasar a binned).
- **Cache invalidation timing**: el `dc ivac` sobre 14.93 MB toma varios ms. Si pasa al mismo tiempo que un acceso de cores 1-3 al buffer recién terminado (no debería — la sync espera primero), puede haber tearing visual. Mitigación: el `mc_wait_debayer_done` antes de invalidate garantiza que cores no estén leyendo prev_buf.

### 4. Modo binned 1536×864 — IMPLEMENTADO en V159

**Ganancia esperada: 7.40 → ~24 fps** a `FLL=0x046D`. Pendiente de validar en HW.

Cambios aplicados:
- `imx708_regs.h`: nueva tabla `k_imx708_common[]` (común) + `k_imx708_binned[]` (mode-specific). Sensor lee crop 3072×1728 centrado del array 4608×2592 (`x_addr=768..3839`, `y_addr=432..2159`) y aplica binning 2×2 → output 1536×864. Bayer queda BGGR tras el flip 0x0101=0x03.
- `imx708.cpp`: init en dos fases (common, luego mode), siguiendo el patrón del kernel `bcm2835-unicam.c`.
- `unicam.cpp`: `FRAME_W=1920` (1536 px × 10 bit / 8), `FRAME_H=864`, `FRAME_SZ=0x195000` (1.58 MB).
- `main.cpp`: `SENSOR_W/H = 1536/864`, `RAW_STRIDE=1920`. Geometría debayer pasa de **downscale 6/5** (V156-V158) a **upscale 5/4**: cada bloque Bayer 2×2 binned → ~2.5 outputs. Nearest-neighbor por ahora; bilinear queda pendiente para suavizar el upscale.

Posibles ganancias adicionales sobre V159 si el debayer lo permite:
- **Reducir FLL más allá de 0x046D**: el mínimo es ~y_size+vblank. A FLL=900 se alcanzarían ~30 fps; a FLL=870 (mínimo absoluto) ~31 fps. Cambiar `0x0340/41` en `k_imx708_binned[]`.
- **Bilinear o Catmull-Rom upscale 5/4** en `debayer_band` para suavizar los bloques 2.5x.

### 5. ✅ Cobertura DMA completa — HECHO (V154)

V146/V147 atribuían el "maxWP=704/864 (~81%)" a un límite pre-existente del Unicam BCM2837. V148–V153 vieron 1750/2592 (~67%) en full mode. V154 FS-a-FS confirma que el límite era artefacto del polling IBWP-stable: con FS-a-FS la cobertura es 0xA20/0xA20 = 2592/2592 = **100%**. La memoria del proyecto sobre ese límite estaba equivocada.

### 6. Interrupciones reales (eliminar el polling de FSI) — alternativa a V160

V160 (debayer en 4 cores con band-while-wait en core 0 vía polling intercalado) consigue el mismo efecto que GIC interrupts (liberar core 0 para más trabajo) **sin tocar la infraestructura de excepciones del BCM2837** (que tiene routing de dos etapas: legacy IC en `0x3F00B000` → GIC400 en `0x40041000`/`0x40042000`, no-trivial documentar/programar fuera de Linux).

Si en el futuro hace falta procesamiento concurrente que no encaja en el patrón "una banda de filas por iter" (e.g. YOLO cuyo grafo de cómputo no se parece a un loop por filas), entonces sí justifica implementar GIC. Cosas que se necesitarían:
- Vector table AArch64 (~16 entradas × 128 bytes).
- Mapear `0x40000000+` en MMU (T0SZ 34 → 33, table[1024]).
- GICD + GICC init.
- Routing de la IRQ del Unicam (probablemente en el rango GIC SPI 64-71 vía agregación de la legacy IC).
- ISR + WFI loop para reemplazar el polling.

V160 evita todo eso y entrega la ganancia (8.55 fps → ~50 fps) con un cambio quirúrgico al loop principal.

### 6b. ✅ V160 implementado: 4-core debayer con band-while-wait

Cambio:
- `BAND_COUNT 3 → 4`. `BAND_H = FB_H/4 = 270` (1080/4 exacto).
- `band_for_secondary(core_id-1)` → `band_for_core(core_id)`. Cores 1, 2, 3 → bands 1, 2, 3. Core 0 → band 0.
- `mc_wait_debayer_done` ahora espera `done == BAND_COUNT - 1 = 3` (core 0 no incrementa el contador, su trabajo es inline).
- `debayer_band` refactorizado a wrapper sobre `debayer_one_row(vy, ...)`.
- Nueva `wait_fs_and_lip_with_band(band_start, band_end, raw, fb, stride)` que entrelaza el polling de FSI con `debayer_one_row(cur_row)` entre polls. Latencia de FSI ≤ ~63 µs (un row), muy por debajo del intervalo FS-to-FS de ~15 ms.
- Nuevas primitivas en `unicam.h/.cpp`: `unicam_arm_for_wait`, `unicam_consume_fsi`, `unicam_lip_strobe` para que main.cpp pueda manejar el polling state machine sin duplicar lógica.

Per-iter cycle:
```
max(wait_fs(15-20 ms), debayer_per_core(17 ms)) ≈ 20 ms = 50 fps
```
vs V159's `max(wait_fs, 23 ms) = 23-25 ms = 38-45 fps`. **+25% sobre V159**.

### 6c. ✅ V161 implementado: ISP byte-exact con libcamera + AE en CIT

Auditoría inicial reveló que **V160 ya tenía BLC=64, WB Q8=(535,256,455) y CCM Q10 correctos** — coinciden byte-exact con `binned_exif.txt:AsShotNeutral` y la ALSC ccm de `imx708.json`. Lo que faltaba:

- **Gamma:** sRGB LUT reemplazado por la `gamma_curve` afinada de `linux_extract/tuning/imx708.json:rpi.contrast` (51 puntos 16-bit, construida en `k_gamma[256]` al boot vía `build_gamma_lut()`). Sombras más profundas, midtones con más punch.
- **LSC:** 108 regs `0x7B10-0x7B45` + `0x7C00-0x7C35` portados verbatim del trace I2C de Linux (líneas 50-157 de `imx708_writes_binned_1536x864.txt`). Tabla `k_imx708_lsc[]` aplicada entre `k_imx708_common[]` y `k_imx708_binned[]` en `imx708_init_baseline()`. Vignetting visiblemente reducido. Los 2 regs Misc enable (`0xC428=0x01`, `0x3100=0x00`) quedan diferidos — la corrección on-die activa sin ellos.
- **AE:** P-controller en CIT (`0x0202/0x0203`). Sample disperso 32×32 = 1024 puntos del raw, target=100 (post-BLC), `k_p=1/8`, slew `±32 lines/update`, clamp `[16, FLL-22=1110]`, update cada 4 frames (~12 Hz), group-hold via `0x0104` para latch atómico. Converge en 1-3 s. Logged inline como `ae_cit=NNN` en TIMING.

Costo runtime: 108 escrituras I2C extra al boot (+10 ms una vez), 0 ms steady-state. AE sample 1024 reads (~10 µs) + I2C cada 4 frames (~400 µs) — invisible en TIMING.

### 6d. ✅ V162 implementado: double-buffer FB + límite firmware Pi3 documentado

Para eliminar la race "cores escriben FB mientras scanout lee":
- `framebuffer_init` allocata `virtual_height = 2 × FB_H = 2160`, `size = 0xFD2000` (verificado).
- Nueva `framebuffer_set_offset(y_offset)` con tag `0x48009 SET_VIRTUAL_OFFSET`.
- `main.cpp` mantiene `fb_buf[2]` con `fb_back_idx` toggleado. Cores siempre escriben el buffer offscreen; page-flip tras `mc_wait_debayer_done`.

**Race buffer-write resuelta**: scanout y cores nunca tocan el mismo buffer.

**Pero tearing seam persiste en movimiento.** Auditoría con polling de PV2 (`0x3F80702C`):

```
PV_STAT bit map empírico (BCM2837 firmware):
  bit 10: HDMI link active (constante)
  bit 6:  active video (~88% del frame)
  bit 5:  late vblank pulse (final del vblank window)
  bit 7:  early vblank pulse (inicio del vblank window)
  bits 0-3: HSYNC-related
  bits 5/6 mutuamente exclusivos
```

NO coinciden con las bit definitions del driver vc4 mainline (que asumen bits 19-31). Ojo si se reusa.

Causa raíz del tearing: mailbox roundtrip ~4 ms >> vblank ~0.67 ms. Aún capturando el inicio limpio de vblank (verificado con `wait_for_vblank()` esperando bit 6→0), el escribe del registro HVS por la VPU cae en active scan → seam mid-line.

**Caminos para tear-free flip (todos diferidos)**:
1. Escritura directa al registro HVS scanout-origin desde ARM (~1 µs, cabe en vblank). Bloqueo: dirección no documentada en datasheets públicos del BCM2837.
2. GIC vsync IRQ con handler que haga el flip atómico en hardware. Bloqueo: bare-metal no monta IRQ infrastructure todavía.
3. Triple buffer. No elimina seam — solo estabiliza el contenido.

V162 deja `wait_for_vblank` revertido (no ayuda y costaba 12 fps), double-buffer queda como infraestructura lista para cuando se aborde una de las opciones 1/2.

---

### 7. Próxima sesión — back-port de V162 al proyecto padre + YOLO integration

El proyecto padre `Direct2Metal/` (un nivel arriba) tiene YOLOv5n funcionando con un debayer V124-era (full-res 4608×2592 → crop 1536×864, RGGB sin ISP afinado). La idea: portar el pipeline V162 de `camara_direct/` al padre, dejando YOLO + bounding boxes intactos arriba.

**Cambios a aplicar en `Direct2Metal/src/`** (orden sugerido):

| Paso | Archivos del padre | Qué portar desde camara_direct |
|---|---|---|
| 1 | `camera_imx708.cpp`, `imx708_regs.h` | Tabla `k_imx708_binned[]` para 1536×864 + LSC `k_imx708_lsc[108]`. Cambiar de full-mode a binned. |
| 2 | `camera_unicam.cpp` | Geometría binned (IBLS, FRAME_W/H), FS-a-FS continuo (V154/V162 protocol), `unicam_stage_dma_buffer` para IBSA0 rotation. |
| 3 | `camera_debayer.cpp` | Cambiar `debayer_raw10_to_chw320` de **RGGB** a **BGGR** (V162 con flip 0x0101=0x03). Eliminar crop (binned ya entrega 1536×864 nativo). Aplicar BLC=16/WB Q8/CCM Q10/gamma curve antes del normalize a float32 — el modelo entrenado con libcamera espera ese pipeline ISP. |
| 4 | `camera_debayer.cpp` (FB path) | Reemplazar el FB debayer por la versión V162 (4-core con band-while-wait, double-buffer). Opcional; mantiene fps de detección si se prefiere preservar cycles para inferencia. |
| 5 | `kernel.cpp` | AE step inline tras el dispatch — re-uso directo de `ae_step()` de V162 main.cpp. |

Validación HW por paso (igual disciplina que esta sesión): build → flashear → escena de prueba → confirmar visual → siguiente paso. Las constantes ISP están todas en `linux_extract/` para reproducir.

YOLO en sí queda intacto: `run_yolo_complete()` consume `cam_frame` (320×320 CHW float32) y dibuja boxes sobre el FB. Si el chw320 sale BGGR + ISP-correcto del paso 3, el modelo debería detectar correctamente.

---

## Configuración para reproducir

### Hardware

- **SBC:** Raspberry Pi Zero 2 W (BCM2837, 4×Cortex-A53 @ 1 GHz nominal, 512 MB LPDDR2).
- **Cámara:** Pi Camera Module v3 (Sony IMX708) en CSI-1, 2 lanes MIPI.
- **Display:** monitor HDMI vía adaptador **mini-HDMI**.
  - ⚠️ El adaptador/cable mini-HDMI fue causa real de bloqueos en V117–V122 — comprueba con un cable conocido bueno antes de sospechar del software.
- **UART consola:** USB-TTL 3.3 V a GPIO 14 (TXD) / GPIO 15 (RXD) / GND. PL011 a 115200 8N1.
- **Power:** alimentación estable; el `over_voltage=-2` en `config.txt` reduce ruido y estabiliza la D-PHY de la cámara.

### Toolchain

```dockerfile
# Dockerfile (etiqueta: rpi-forge)
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y \
    make gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu
WORKDIR /app
```

```bash
docker build -t rpi-forge .
docker run --rm -v $(pwd):/app rpi-forge make
```

### Layout de la SD (FAT32, partición boot)

| Archivo | Origen | Necesario para |
|---------|--------|----------------|
| `bootcode.bin`, `start4.elf`, `start_x.elf`, `fixup_x.dat` | Pi firmware oficial | Boot del VPU + carga del kernel |
| `bcm2710-rpi-zero-2-w.dtb` | Pi firmware oficial | Device tree |
| `overlays/imx708.dtbo` | Pi firmware oficial | Activación D-PHY + clock + GPIOs cámara |
| `config.txt` | Este repo | Ver abajo |
| `kernel8.img` | Generado por `make` | El binario bare-metal AArch64 |

### `config.txt` mínimo

```ini
arm_64bit=1
kernel=kernel8.img
enable_uart=1
gpu_mem=128

# Reloj conservador para estabilizar D-PHY
arm_freq=600
core_freq=250
sdram_freq=400
over_voltage=-2

# HDMI 1024×768 (la geometría real la elige el FB via mailbox)
hdmi_force_hotplug=1
hdmi_group=2
hdmi_mode=4
hdmi_drive=2
disable_overscan=1

# CRÍTICO PARA CÁMARA — sin esto la D-PHY queda sin calibrar y STA=0:
start_x=1
dtoverlay=imx708
```

### Secuencia de power-up de la cámara (gotchas reales)

1. **`SET_DOMAIN_STATE(domain=14, state=1)` vía mailbox** — enciende el decoder Unicam1 CSI-2. Sin esto, `STA=0` para siempre. **`SET_POWER_STATE(0x0d)` NO funciona** (estado queda 0x02). Lección de V108.
2. **CM_CAM1 a 100 MHz** desde PLLD (DIVI=5). Sin reloj digital, el decoder no procesa packets. Hacer esto en `unicam_init` antes de tocar ningún registro Unicam.
3. **CM_CAM0 a 24 MHz** — provee EXTCLK al sensor. Lo configura el firmware del VPU al cargar `dtoverlay=imx708`.
4. **Probe IMX708 en BSC1 (I2C1) a dirección `0x1A`**. Lectura de `0x0016/0x0017` debe dar `0x0708`. Si NACK persistente: 99% es flex-cable mal asentado, no software. Bus reset y soft-reset (`0x0103=0x01`) son fallbacks.
5. **Escribir tabla `k_imx708_full[]` (266 regs)**. Sensor en standby (`0x0100=0x00`).
6. **`imx708_stream_on()` (`0x0100=0x01`)**.
7. **`unicam_capture_start()`** — re-asierta CPE + LIP para latch limpio del primer frame.

### Registros Unicam runtime imprescindibles (verificados vs Linux)

| Reg | Valor | Por qué |
|-----|------:|---------|
| `CTRL` | `0x00080F03` | MEM=1, CPE=1, PFT=0xF, OET=128, CPM=0=CSI-2 |
| `ANA` | `0x770` | D-PHY power-up (escribir `0x774` 1 ms antes para AR hold) |
| `CMP0` | `0x80000301` | Frame-boundary protocol; **NO** desactivar (V134 lo hizo y dañó la captura) |
| `CLK` | `0x06000005` | Clock-pattern HS termination |
| `DAT0` | `0xC0000005` | Data-pattern lane 0 |
| **`DAT1`** | **`0x06000005`** | **Lane 1 usa termination CLOCK-pattern, no data-pattern**. Si pones `0xC0000005` aquí, escenas reales muestran "line swap"; test patterns uniformes no lo revelan. Bug encontrado vía diff contra runtime libcamera. |
| `IDI0` | `0x0000002B` | RAW10, VC=0 |
| `ICTL` | `0x00D80007` | FSIE+FEIE+IBOB + Pi OS upper DMA enables |

### Valores AE/AGC convergidos (deben coincidir con Linux)

| Reg | Valor | Significado |
|-----|------:|-------------|
| `0x0202/03` | `0x09E7` | CIT = 2535 líneas (`0x0929` da imagen 4× oscura) |
| `0x0204/05` | `0x03C0` | AGAIN = 16× (`0x0300` da 4×) |
| `0x020E/0F` | `0x0100` | DGAIN = 1.0× |
| `0x0340/41` | `0x0A5A` | FLL = 2650 líneas (frame length lines) |
| `0x0342/43` | `0x3D20` | LLP = 15648 pck/línea (line length pixels) |
| `0x0101` | `0x03` | H+V flip → Bayer efectivo BGGR (sin esto sería RGGB y sale espejado) |

Verificación: `python3 tools/diff_imx708_full.py` debe imprimir `265/265 common`.

**Período del frame derivado de FLL × LLP:** `2650 × 15648 / pixel_clock ≈ 117 ms = 8.55 Hz teóricos`, confirmado empíricamente por V157 (medición HW). Es el techo absoluto de la cadencia de captura mientras se conserve esta configuración full-mode 4608×2592 @ 2-lane. Bajar `FLL` o `LLP` lo reduciría, pero hay que respetar `vblank ≥ 58` líneas y `hblank ≥ 11040 pck` que libcamera valida — bajar más rompe el timing D-PHY.

### Habilitación FP/SIMD (requerido para NEON sin trap)

En `src/start.S`, antes de bajar a EL1:

```asm
# Limpiar CPTR_EL2.TFP (bit 10) — sin esto FP/SIMD trap a EL2
mrs     x0, cptr_el2
bic     x0, x0, #(1 << 10)
msr     cptr_el2, x0
```

Y en EL1 antes de `kernel_main`:

```asm
# CPACR_EL1.FPEN = 0b11 (no traps FP/SIMD a EL0/EL1)
mov     x0, #(3 << 20)
msr     cpacr_el1, x0
isb
```

`Makefile`: usar `-mcpu=cortex-a53` (sin `+nosimd`) y `-MMD -MP` para tracking de headers (sin esto, cambios en `*.h` no rebuildan los `.o` correspondientes — pasó con `imx708_regs.h` y los valores AE quedaron fuera del binario).

### Mapa de memoria

```
0x00000000 .. 0x00FFFFFF  Kernel + BSS + stack (≈ 11 KB)
0x01000000 .. 0x01195000  RAW_A DMA target (1.58 MB binned ping)      ← V159
0x02000000 .. 0x02195000  RAW_B DMA target (1.58 MB binned pong)      ← V159
0x1C000000 .. 0x20000000  VPU heap (gpu_mem=128 → últimos 128 MB)
0x1E402000 ..             Framebuffer (asignado por el VPU vía mailbox)
0x3F000000 .. 0x40000000  MMIO peripherals (Device-nGnRnE en MMU)
0xC0000000 | phys         Bus alias VideoCore para DMA writes
```

(V156 = RAW + SNAP separados para memcpy CPU-stable. V158 = dos buffers RAW para ping-pong, snap eliminado, full mode (14.93 MB cada uno). V159 = binned 1.58 MB cada uno.)

MMU identity-mapea 1 GB con bloques de 2 MB. El rango VPU está mapeado **Normal Non-cacheable** + Inner Shareable para que la VPU vea los pixels sin caché ARM por encima.

### Tooling de diagnóstico

```bash
# Diff estático: tabla bare-metal vs traza I2C Linux full mode
python3 tools/diff_imx708_full.py
# (espera: "Last-value match: 265 / 265 common")

# Diff runtime: uart.log bare-metal vs dump runtime libcamera
python3 tools/diff_runtime_full.py uart.log linux_capture_linux/
# (todos los registros deben coincidir excepto IBSA0/IBEA0/IBLS marcados EXPECTED)
```

UART log esperado tras boot (V158, en validación HW):

```
camara_direct V158: IMX708 4608x2592 -> HDMI 1920x1080 (continuous DMA, IBSA0 rotation)
CORES alive=[1,1,1,1]
Mailbox: domain 14 ON
BSC1: init
Unicam: V150 full-mode init (4608x2592)
Unicam: CM_CAM1 = PLLD/5 (100MHz)
Unicam: init OK. IBSA0=0xC1000000 IBEA0=0xC1E3D000 IBLS=0x00001680
IMX708: Probe OK
IMX708: stream ON
Unicam: CPE + LIP re-armed after sensor stream_on
UNICAM_BEGIN ... UNICAM_END             ← 19 regs Linux-comparable (pre-loop)
IMX708_BEGIN ... IMX708_END             ← 19 regs Linux-comparable (pre-loop)
FRAME rows_max=0x00000A20 / 0xA20       ← primer iter: cobertura del primer frame en RAW_A
TIMING wait=117 sync=0 inval=2 disp=0 total=119 ms  fps=8.40   ← objetivo
TIMING wait=117 sync=0 inval=2 disp=0 total=120 ms  fps=8.33
...
```

Si en HW se observa `wait` creciendo a ~141 ms (memory contention del debayer concurrente), o `sync` > 0 (debayer no terminó dentro del frame period), hay que reducir el debayer a 4 cores con core 0 ayudando o pasar a binned mode.

Para referencia, baseline V156 (uart.log committed 2026-05-06):

```
TIMING wait=93 snap=18 debayer=24 total=135 ms  fps=7.40       ← V156 (revertido tras V157)
```

`wait=93` allí es el período del sensor (117 ms) menos los 24 ms de debayer que corrían entre rearm y wait — ver §"Período del sensor — finding V157" para por qué ese número era engañoso.

---

## Referencias

- **Linux baseline:** `linux_extract/` — DNG, JSON tuning, ftrace, registros I2C/Unicam runtime capturados de un Bookworm + IMX708 funcional.
- **Diff report:** `linux_extract/DIFF_REPORT.md` — regs faltantes/divergentes vs libcamera.
- **Memoria del proyecto:** `~/.claude/projects/-home-jolux-projects-Direct2Metal/memory/` — historial V108 → V157 con root causes (incluye `camara_direct_sensor_period.md` con la deducción del techo 7.4 fps).
