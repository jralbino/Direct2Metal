# camara_direct — IMX708 bare-metal end-to-end

Subproyecto dentro de Direct2Metal enfocado a **entender y dominar la Pi Camera v3 (IMX708)** sin Linux, sin libcamera, sin bcm2835-unicam. Objetivo: capturar video a la máxima resolución viable y mostrarlo por HDMI, usando lo aprendido en Phase 9 del proyecto padre (V17–V154).

Este folder existe para empezar limpio, sin el lastre del pipeline YOLO ni las ramas diagnósticas acumuladas en `src/camera_*.cpp`, y con el conocimiento ganado ya consolidado desde el día 0.

---

## 1. Target hardware

| | |
|---|---|
| SoC | BCM2837 (Cortex-A53 ×4) |
| Board | Raspberry Pi Zero 2 W |
| Sensor | IMX708 (Pi Camera v3, 12 MP) |
| Interface | MIPI CSI-2, 2-lane, 450 Mbps/lane |
| I²C | BSC1 (GPIO44 SDA / GPIO45 SCL, ALT2) |
| Sensor I²C addr | `0x1A` |

## 2. Requisitos de boot (SD card `config.txt`)

```
start_x=1            # VPU firmware con soporte de cámara
dtoverlay=imx708     # pinmux + clock tree para la cámara
kernel=kernel8.img
```

Sin estas dos líneas los GPIOs de CSI-2 no están configurados y el VPU no inicializa los PLLs que alimentan al sensor. Aunque no usemos Linux, el VPU firmware consume `dtoverlay=imx708` **al arranque**, antes de entregar el control a nuestro kernel.

## 3. Pipeline

```
  IMX708 ──CSI-2 2-lane 450 Mbps──►  Unicam1 (MMIO 0x3F801000)
    │                                   │
    │ I²C BSC1                           │ DMA
    │ (init regs)                        ▼
    │                             g_raw_frame (RAW10 packed)
    ▼                                   │ CPU
  [stream on]                            ▼
                                   Debayer RGGB → RGB
                                         │
                                         ▼
                                Framebuffer (GPU mailbox)
                                         │
                                         ▼
                                      HDMI out
```

## 4. Power-up: EL paso crítico

Unicam1 está **power-gated por default**. El VPU firmware deja todos los MMIO (CTRL/STA/CLT/DLT…) a 0 y ANA = 0x777 (D-PHY apagada). Hay que activar el **POWER DOMAIN 14** vía mailbox ANTES de tocar MMIO:

```
SET_DOMAIN_STATE  tag=0x00038030  domain=14  state=1
```

`SET_POWER_STATE` (tag 0x28001) **NO** activa Unicam — ese mailbox controla un dominio distinto (audio/USB). En el proyecto padre perdimos 21 versiones (V85–V107) hasta encontrar esto. **Ver V108 en `../camera_debug.md`.**

## 5. Secuencia de inicialización Unicam1

Orden obligatorio (cualquier desviación da `STA=0` permanente):

| # | Operación | Valor | Notas |
|---|-----------|-------|-------|
| 1 | `SET_DOMAIN_STATE(14, 1)` | mailbox | imprescindible |
| 2 | `SET_CLOCK_RATE(clock=4, rate=250MHz)` | mailbox | core clock |
| 3 | `CM_CAM1CTL` @ 0x3F101048 | `0x5A000004` | enable, source=PLLD |
| 4 | `CLKGATE` @ 0x3F802004 | `0x5A000015` (2-lane) / `0x5A000005` (1-lane) | password+gate bits |
| 5 | `CTRL.MEM` | set bit | habilita interfaz DMA |
| 6 | `ANA` | `0x774` → `0x770` | D-PHY bias (CTAT=7, PTAT=7), 2 pasos |
| 7 | `CPR` | pulse | Clear Protocol Reset (OBLIGATORIO antes de configurar lanes) |
| 8 | `CLK` | `0x0005` | use_lp_clock=true para IMX708 |
| 9 | `DAT0`, `DAT1` | `0x1D` c/u | después de CPR |
| 10 | `CLT`, `DLT` | Linux defaults | timing ths_settle |
| 11 | `IDI0` | `0x2B` | RAW10 data type |
| 12 | `IBLS` | `1920` | byte stride (NO píxeles) |
| 13 | `IBSA0`, `IBEA0` | phys_addr buffer + tamaño | bus addr con `0xC0000000` alias |
| 14 | `IPIPE`, `ICTL` | control bits + `LIP` | LIP latcha IBSA0 en registro activo |
| 15 | `CTRL.CPE` | set bit | capture enable |

**Reglas verificadas en hardware:**
- CLK / DAT0 / DAT1 escritos **antes** de CPR → CPR los resetea a power-down. Escribir **después**.
- ANA escrita en un solo paso → D-PHY se queda en estado intermedio. Secuencia 0x774 → 0x770.
- CLKGATE debe incluir el password `0x5A000000`. Sin él la escritura es descartada silenciosamente.

## 6. Sensor IMX708

### 6.1 Probe
I²C `0x1A`, leer 0x0016 / 0x0017 → esperar `0x07 / 0x08` (chip ID).

### 6.2 Secuencia init (modo baseline 2×2-binned 1536×864 @ 52fps)

Basado en `drivers/media/i2c/imx708.c` del kernel Raspberry Pi, modo `mode_2x2binned_720p_regs`. Registros clave:

| Reg | Valor | Meaning |
|-----|-------|---------|
| 0x0100 | 0x00 | standby (siempre primero) |
| 0x0136 / 0x0137 | 0x18 / 0x00 | EXCK_FREQ = 24 MHz |
| 0x0112 / 0x0113 | 0x0A / 0x0A | CSI-2 format = RAW10 |
| 0x0114 | 0x01 | 2-lane |
| 0x034C / 0x034D | 0x06 / 0x00 | x_output_size = 1536 |
| 0x034E / 0x034F | 0x03 / 0x60 | y_output_size = 864 |
| 0x0310 | **0x00** | non-continuous HS clock (V104 HW override — kernel pone 0x01) |
| 0x030E / 0x030F | 0x01 / 0x2C | MIPI OP-PLL = 450 MHz |
| 0x0204 / 0x0205 | **0x03 / 0xC0** | ANA_GAIN = 16× (V122 HW override) |
| 0x020E / 0x020F | **0x02 / 0x00** | DIG_GAIN = 2× |
| 0x0202 / 0x0203 | 0x04 / 0x6B | integration = 1131 líneas anti-flicker |
| 0x0100 | 0x01 | stream ON (**después** de que Unicam esté capturando) |

**Registros a mantener en `0x00` para simplificar:**
- `0x0B8E / 0x0B94` — PDAF enable (off = no packets DT=0x30 intercalados)
- `0x3400` — embedded data enable (off = no DT=0x12 bleeding al buffer)
- `0x0220` — HDR mode (off = exposición lineal sin DOL)

### 6.3 Sobre la ganancia (HW-verified)

La tabla del datasheet sugiere `ANA_GAIN=0x0070` ≈ 16×. **NO ES CIERTO en este módulo**: 0x0070 produce solo ~1.12×. Usar `0x03C0` para 16× reales. Sin este fix la imagen sale casi negra incluso en exteriores. Verificado por observación directa de bytes en V122.

### 6.4 Patrón Bayer

En la orientación por defecto (sin escribir `0x0101` de orient flip), el pixel `(0,0)` es **ROJO**. Patrón efectivo:

```
(even,even) = R      (even,odd) = Gr
(odd, even) = Gb     (odd, odd) = B
```

Esto es **RGGB**, no BGGR. libcamera reporta el modo como `SBGGR10_1X10` pero **esa etiqueta es engañosa**. Siempre verificar visualmente — el síntoma típico de R/B swapped es "piel azul" (ver V124 en el proyecto padre).

## 7. Formato RAW10 packed en DRAM

Cada grupo de 5 bytes codifica 4 píxeles:

```
byte0 = pixel0[9:2]
byte1 = pixel1[9:2]
byte2 = pixel2[9:2]
byte3 = pixel3[9:2]
byte4 = {pixel3[1:0], pixel2[1:0], pixel1[1:0], pixel0[1:0]}
```

- Stride de línea: `1536 px × 10 bit / 8 = 1920 bytes` = 384 grupos × 5 bytes.
- Para debayer MSB-8 (suficiente para display): leer solo bytes 0–3 de cada grupo, descartar byte 4.

Helper de referencia (del proyecto padre):
```cpp
static inline uint8_t raw10_msb8(const uint8_t* row, int col) {
    return row[(col >> 2) * 5 + (col & 3)];
}
```

## 8. Framebuffer HDMI

Setup vía GPU mailbox (tags típicos, en una sola llamada):
- `0x00048003` SET_PHYSICAL_SIZE
- `0x00048004` SET_VIRTUAL_SIZE (igual a physical para empezar)
- `0x00048005` SET_DEPTH = 32
- `0x00048006` SET_PIXEL_ORDER = BGR
- `0x00040001` ALLOCATE_BUFFER

Resultado: puntero a framebuffer en DRAM (convertir `0xC0000000` bus addr → `0x3FFFFFFF`-mapped CPU addr). `pitch` típico = `width × 4`.

Pixel en memoria (little-endian uint32): `0xFF000000 | (B<<16) | (G<<8) | R`.

## 9. MMU y cache

- Identity map, `T0SZ=34`
- **Device-nGnRnE** para el rango MMIO (`0x3F000000`–`0x3FFFFFFF`)
- Normal cacheable WB para DRAM
- **Invalidar D-cache** del buffer DMA **antes** de leerlo desde CPU (por línea de cache, no por página)
- Si los secundary cores usan el buffer, cada uno debe hacer `init_mmu()` en su entry point (ver V72 en el proyecto padre)

## 10. Problemas conocidos (heredados)

> **Estado V162 (2026-05-09):** secciones 10.1, 10.3 RESUELTAS. 10.2 desplazada — V162 binned no la sufre. Tearing residual nuevo (mailbox vs vblank) documentado en PROGRESS.md §V162. Para detalle de V161/V162 ver PROGRESS.md.

### 10.1 Shift cíclico 32 filas / 960 bytes — RESUELTO (V147+)

El buffer DMA presenta una alternancia periódica (~36 filas en 2-lane, ~6 en 1-lane) donde cada fila aparece en uno de ≥4 estados:

- **NORMAL** `[Q1,Q2,Q3,Q4]` — contenido bien
- **ROTATED** `[Q3,Q4,Q1,Q2]` — mitades intercambiadas (shift de 960 bytes)
- **DUP_L** `[Q1,Q2,Q1,Q2]` — mitad izquierda duplicada a la derecha
- **DUP_R** `[Q3,Q4,Q3,Q4]` — mitad derecha duplicada a la izquierda

EVEN y ODD transicionan independientemente. El contenido SOLID uniforme cruza byte-perfect (V147) → el shift depende del contenido. HDMI descartado (V152 synthetic pattern → salida impecable). Por tanto el bug está entre la salida del sensor y el buffer DMA.

**Descartado como causa:**
- PDAF, embedded data, HDR/DOL mode
- 2-lane vs 1-lane
- Secondary exposure registers
- Configuración kernel-exact vs overrides

**Workaround actual** (V153/V154): clasificador per-fila de 4 estados vs quartile means de la fila `y-2` (misma paridad, ya corregida). Fallback: clonar `y-2` para estados DUP. Limitado a `maxWP_lines` reales para evitar propagar basura. Ver `src/camera_unicam.cpp:deshift_halfline_pass()`.

**Hipótesis aún no probadas (candidatas para este subproyecto):**
- `DLT` (Data Lane Timing) mal calibrado: `ths_settle`, `ths_prepare`, `ths_zero`. En V154 usamos los defaults de Linux bcm2835-unicam pero no hemos barrido el espacio.
- `CLT` (Clock Lane Timing): mismo tema.
- Registros no documentados en el rango `0x3F801000`. La fuente del kernel solo toca una parte.
- Interacción con presión de bus DRAM (VPU compartiendo memoria).
- Algún registro del IMX708 que desactiva una característica de doble lectura interna.

### 10.2 `maxWP_lines = 704 / 864` — N/A en modo binned

Era un problema en full-res (V148-V158). En el modo binned 1536×864 (V159+) el sensor entrega los 864 rows completos byte-exact (verificado en V147 con test pattern sólido). El límite reportado era específico al pipeline full-res que ya no usamos.

### 10.3 Ruido con movimiento — RESUELTO (V160-V162)

V154-V158 mostraban ruido en movimiento por race write/scanout. V160 eliminó el write/scanout overlap con el 4-core debayer + band-while-wait. V162 añadió double-buffer FB para garantizar que cores y scanout nunca tocan el mismo buffer.

**Tearing residual independiente**: una costura horizontal sigue visible en movimiento rápido. Causa diferente: mailbox `SET_VIRTUAL_OFFSET` (~4 ms) > vblank HDMI (~0.67 ms), el escribe del HVS por la VPU cae en active scan. Límite del firmware Pi3, no del bare-metal. Ver PROGRESS.md §V162 para análisis y caminos de fix.

## 11. Objetivos del subproyecto

1. **Resolución máxima viable**. Opciones del IMX708:
   - Full-res 4608×2592 @ 14fps (readout completo, 16 MPx/s)
   - 2×2-binned 2304×1296 @ 56fps (~170 MPx/s)
   - 2×2-binned 1536×864 @ 52fps (baseline heredado)

   Decidir según qué resolución HDMI podemos emitir (Pi Zero 2 W = hasta 1080p por HDMI). Probablemente 2304×1296 binned es el sweet spot.

2. **HDMI directo**. Pipeline mínimo sensor → buffer → debayer → framebuffer. Sin stack YOLO, sin ramas diagnósticas en el path principal (sí en un binario separado).

3. **Atacar el shift cíclico desde cero**. Con código limpio y sin herencia, probar:
   - Barrido de `DLT` / `CLT` timing (ths_settle, ths_prepare)
   - Tamaños alternativos de IBLS/IBEA
   - Orden alternativo de init Unicam
   - Inyectar el registro alternativo 0x0101 (orient) y ver si altera el patrón del shift

4. **Resolver `maxWP=704`**. Probar:
   - IBEA sobredimensionado (buffer > 864 líneas)
   - Desactivar CMP0 completamente en lugar de cambiar su valor
   - Medir IBWP con granularidad fina (cada 1ms) para ver dónde exactamente se detiene
   - Probar si la línea 704 coincide con algún marcador conocido (HDR line, AE-HIST, etc)

5. **Double buffering** para eliminar ruido de movimiento.

## 12. Archivos a portar / estudiar en el padre

| Archivo | Qué tomar |
|---------|-----------|
| `src/camera_bsc.cpp` | BSC1 I²C driver + probe 0x1A |
| `src/camera_imx708.cpp` | probe + stream on/off |
| `src/imx708_regs.h` | tablas de registros (baseline 2×2 binned 720p) |
| `src/camera_unicam.cpp` | init + captura + deshift (ojo: 1500+ líneas con diagnósticos acumulados — extraer solo lo esencial) |
| `src/camera_debayer.cpp` | RGGB RAW10 debayer (CHW float y RGB8 paths) |
| `src/mailbox.cpp` | GPU mailbox (para `SET_DOMAIN_STATE` y framebuffer) |
| `src/mmu.cpp` | identity map con Device-nGnRnE para MMIO |
| `src/video.cpp` | framebuffer alloc via mailbox |

Historia detallada V17–V116: `../camera_debug.md`.
Historia V117+: commits git del padre (buscar `V1xx` en mensajes).

## 13. Estructura propuesta

```
camara_direct/
├── README.md                 ← este archivo
├── Makefile                  ← independiente del padre
├── build/
│   └── config.txt            ← start_x=1 + dtoverlay=imx708
├── src/
│   ├── main.cpp              ← entry, orquestación
│   ├── mailbox.cpp/.h        ← GPU mailbox (power domain + FB alloc)
│   ├── bsc.cpp/.h            ← BSC1 I²C
│   ├── imx708.cpp/.h         ← sensor init + stream
│   ├── imx708_regs.h         ← register tables (mode selectable)
│   ├── unicam.cpp/.h         ← Unicam1 receiver + DMA
│   ├── debayer.cpp/.h        ← RGGB → RGB + downscale a framebuffer
│   ├── framebuffer.cpp/.h    ← mailbox-based FB + write helpers
│   ├── mmu.cpp/.h            ← identity map + Device-nGnRnE
│   ├── uart.cpp/.h           ← serial debug (PL011)
│   └── start.S               ← aarch64 boot code
└── tools/
    └── parse_uart.py         ← parseo de logs de diagnóstico
```

## 14. Plan de trabajo sugerido

1. **Portar mínimo viable** (probe IMX708 + framebuffer HDMI test pattern) — sin Unicam. Confirma boot + I²C + HDMI en el kernel nuevo.
2. **Portar Unicam init** con la secuencia del §5 y capturar un frame en modo baseline (1536×864). Verificar `STA > 0` y `IBWP > 0`.
3. **Debayer mínimo** (sin AWB ni stretch) a framebuffer. Ver si el shift cíclico aparece igual que en el padre — si sí, el problema no depende del código sino de la config HW.
4. **Barrido DLT/CLT**. Estado actual: defaults de bcm2835-unicam. Probar variantes.
5. Si el shift persiste: cambiar a modo 2304×1296 @ 56fps — si el shift es proporcional al período de línea, el período visual cambiará y podemos triangular.
6. Una vez estable: pasar a máxima resolución.
