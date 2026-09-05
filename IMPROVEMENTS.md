# Direct2Metal — Mejoras posibles (2026-09-04)

> Reporte de planeación, no un log de versiones (ese es `PLAN.md`) ni el roadmap
> estratégico (ese es `GOALS.md`). Este archivo responde una pregunta puntual:
> **con el estado actual (V193, 491-492 ms/frame HW, camino Winograd cerrado),
> ¿qué vale la pena intentar después, en qué orden, y por qué?**
> Cuando un ítem avance, su detalle táctico va a PLAN.md/GOALS.md como siempre;
> este archivo se actualiza para tachar/reordenar, no se duplica.

Único árbol vivo: `~/projects/Direct2Metal_old`. `~/projects/D2M` queda solo como
historia (V76, congelado) — no se vuelve a consultar salvo que se pida
explícitamente algo de esa época.

---

## Estado de partida (actualizado 2026-09-04, tras V194-V200)

- **264 ms/frame sostenidos** en HW real @ 1000 MHz, YOLOv8n 320×192, FoV
  completo. Desglose: `cap=1 l0=7 bb=88 neck=50 head=99 render=16`. Verificado
  sobre 151-301 frames con `[SOC] arm=1000MHz thr=0x0` hasta 74 °C, y en vivo
  con cámara (`[T]` mediana 293 ms incluyendo captura y render).
- **De dónde vienen los 264**: la sesión empezó en **705 ms sostenidos** (los
  491 que decía este archivo eran los primeros ~95 frames; después el firmware
  bajaba el reloj a 600 MHz). Cadena: V196 reloj fijado por mailbox
  (705 → 435), V195 P8 stride-2 + acumuladores `noinline` (504 → 435 dentro de
  la ventana), V197 padding explícito (435 → 367), V199 los 4 cores calculan
  (367 → 264). **Sin tocar el modelo, los pesos ni una sola FMA.**
- **Un único camino de inferencia** desde V200 (borrado el reparto async de
  V180), para que los A/B entre modelos no arrastren la duda de por cuál se
  midió.
- Dos verdades de este archivo se cayeron por el camino: "conv3×3 está cerca
  del piso del A53" (se le sacó ~40 % al mismo kernel) y "el throttling
  térmico" (era el governor del firmware, `thr` nunca dejó de ser 0).
- El ítem más caro que queda sigue siendo el head DFL: **P3 head 65 ms**, con
  P4 22 y P5 13 — 99 ms de 264, el 37 % del frame.
- `tools/env` sigue roto (intérpretes de 0 bytes) — **y ahora es el bloqueador
  principal**, porque lo único que queda por delante necesita re-exportar
  pesos.
- Árbol limpio y pusheado a `main`. El WIP de V180-182 quedó commiteado en
  V200-, y de paso resultó que `HEAD` no compilaba (ver el log de la sesión).

---

## Ítems, en orden recomendado

### 1. ~~Arreglar `tools/env`~~ — **HECHO (V201), y no recreándolo**

La causa era que el miniconda que proveía el intérprete había desaparecido del
host. El `site-packages` de 7.7 GB seguía ahí, intacto, pero es `cp313` y en el
host solo queda Python 3.14: los `.so` de torch/numpy no cargan. Recrear el venv
habría dejado el mismo problema esperando a la próxima limpieza del host.

Sustituido por **`tools/Dockerfile.torch`** (imagen `d2m-torch`), mismo patrón
que `rpi-forge` para el toolchain, con torch CPU y versiones pinneadas a las que
produjeron los pesos vigentes. Validado de la forma fuerte: `export_model.py`
**reproduce `weights.bin` byte a byte** (CRC 0x176A3D5A, `git status` limpio).

De paso salieron dos bugs latentes que habrían quemado la siguiente sesión: los
exportadores escribían en `../src/` (directorio retirado en V171) y cargaban
`yolov8n.pt` relativo al cwd — si ultralytics no lo encuentra **se lo descarga
solo**, y habríamos exportado pesos distintos sin enterarnos.

`tools/env` (7.7 GB) queda como peso muerto en disco; se puede borrar.

### 2. ~~Barrido de perfil `C2F_PROF`~~ — **HECHO, y había mucho más aire del previsto**

El perfil por capa ya estaba en `hwbench.log`; no hacía falta el barrido. Lo
que mostró, comparando ms medidos contra MAC por capa: el `conv1x1` corría a
2.3-2.5 GMAC/s y el `conv3×3` a 1.3-1.8, con las **stride-2 a 0.86-1.29** —
mismo MAC, mismo silicio, la mitad del rendimiento. De ahí salieron V195
(las 6 conv3×3 stride-2 no tenían fast path: 87 → 56 ms) y V197 (el anillo de
padding era 47 % de las posiciones a S32: 435 → 367 ms). El veredicto "conv3×3
está cerca del piso del A53" queda retirado: se le sacó ~40 % sin tocar una FMA.

Lo que sigue **sin** medir: no volví a comparar GMAC/s por capa después de
V197/V199. Si alguien quiere reabrir el kernel, ese es el primer sitio donde
mirar, y ahora cuesta una corrida de `make bench`.

### 3. Head DFL: probar 1 conv por rama en vez de 2 (esfuerzo **alto**, impacto ~35-40 ms sobre 264)

Cada rama del head (box/cls, en P3/P4/P5) apila 2 conv3×3 antes del conv1x1
final. Quitar la segunda recortaría el ítem más caro del grafo: los tres heads
son 99 ms de los 264 (37 %), y la segunda conv de cada rama es ~35-40 de esos.

**Dos avisos sobre la versión anterior de este ítem.** (a) El esfuerzo no es
medio: no hay fusión lineal válida porque entre las dos conv3×3 hay un SiLU,
así que "fusionar sus pesos aproximando con la primera" no funciona — hace
falta reentrenar o destilar, o sea dataset + GPU, no solo `export_model.py`.
(b) Antes de eso hace falta **con qué medir la precisión**: hoy el único
instrumento es "el reloj se detecta al 56 % en un frame". Ver ítem 3b.

Camino barato para acotar el riesgo **antes** de comprometerse:

1. Con `tools/env` arreglado, cargar los pesos YOLOv8n pre-entrenados de
   `ultralytics`, **cortar quirúrgicamente** la segunda conv de cada rama
   (ej. dejarla como identidad, o fusionar sus pesos aproximando con la
   primera) y correr inferencia en PyTorch sobre unas ~20 imágenes de
   `coco128` para ver cuánto cae el mAP/confianza *sin* reentrenar.
2. Si la caída es aceptable (a validar con el usuario, no asumir un umbral),
   recién ahí vale la pena exportar y portar al kernel. Si no, esto se cierra
   rápido y barato — mismo espíritu que el descarte de Winograd: medir antes
   de comprometer una sesión completa.

### 3b. ~~Harness de evaluación de precisión~~ — **HECHO (V201)**

`tools/eval_reference.py` corre YOLOv8n en PyTorch sobre **el mismo tensor que
ve el kernel** y saca las detecciones en formato `GOLDEN`; los umbrales los lee
de `bsp/safety_config.h` para que no puedan derivar. Con esto el banco pasa a
tener dos instrumentos complementarios: `[ABS]` para la numérica bit a bit y
éste para la calidad.

**Primer resultado, y vale la pena tenerlo escrito**: sobre el golden actual,
referencia `[(74, 56)]` y kernel `[(74, 56)]` — **idénticos al punto
porcentual**. O sea el port bare-metal coincide con PyTorch sobre el mismo
tensor. A partir de ahora, si los dos discrepan es el port; si cambia solo la
referencia, es el modelo.

### 4. A/B de modelos: YOLOv5n vs YOLOv8n sobre el mismo frame capturado (esfuerzo alto, impacto en precisión + posible resolución)

Este es el ítem que quedó pendiente de la sesión anterior con una premisa
falsa (`app/yolo_v5n_coco/` no existe en el layout nuevo — ver PLAN.md V193).
Lo que hay es v5n en el layout plano viejo (commit `a842b62`, pre-V171).
Portarlo es trabajo real:

- Exporter v5n (repack 8ch/4ch — reusar la lógica de `export_model.py`, pero
  el grafo v5 es distinto: anchor-based, no DFL).
- Grafo completo (`yolo_v5n.cpp` nuevo en `app/yolo_v5n_coco/`, siguiendo la
  estructura de `app/yolo_v8n_coco/` pero con el decode anchor-based de D2M).
- Comparar sobre el `test_image.bin` actual (320×192, frame de cámara real)
  contra v8n: ¿el reloj (`clock`, 56 % en v8n) se detecta con más confianza al
  tener la mitad de FLOPs por el mismo compute budget? ¿o permite subir a
  384×224 a la misma latencia?

Vale la pena solo si el objetivo es precisión/resolución, no velocidad pura
(v5n no es necesariamente más rápido por parámetro-equivalente en este
kernel — hay que medirlo, no asumirlo).

**Empezar por YOLOv11n, no por v5n.** v11n usa el mismo head DFL, así que
reutiliza `decode_v8_dfl` tal cual; v5n es anchor-based y obliga a reescribir
el decode entero. Si el objetivo es precisión, v11n es a la vez el candidato
más fuerte y el más barato de portar. Y antes de cualquiera de los dos hay un
experimento **sin exportador ni venv**: YOLOv8n es fully-conv, así que un
barrido de `YOLO_W`/`YOLO_H` responde sin exportador cuánta confianza se compra
por cuántos ms. **Ya medido (V201)** — tiempos en HW y calidad sobre un frame
real capturado a 448×256 y submuestreado a las demás geometrías (escena
idéntica por construcción):

| geometría | T en HW | clock (referencia) |
|---|---|---|
| 256×160 | 207 ms | 50 % |
| 320×192 | **275 ms** | **65 %** |
| 384×224 | 383 ms | 70 % |
| 448×256 | 507 ms | 70 % |

El tiempo es lineal en píxeles (4.4-5.0 ms por cada 1000 px) y **la calidad
satura en 384×224**: de 320 a 384 son +108 ms (+39 %) por +5 pp, y de 384 a 448
+124 ms por **nada**. Así que subir resolución no es el camino barato que
parecía: si hace falta más precisión, tiene que venir del modelo.

### 5. Winograd, segunda vuelta: reescritura estilo GEMM (esfuerzo alto, resultado incierto, impacto potencial grande)

V193 midió que el Winograd "ingenuo" (tiles como arreglo escalar, canales de
salida en los carriles vectoriales) pierde por *register spill* — el
acumulador de 16 taps × hasta 8 tiles no cabe en los 32 registros NEON del
A53. La ganancia aritmética (2.25× menos FMA) es real; el problema es
puramente de *tiling* de registros. Un rediseño que vectorice **tiles** en
vez de **canales de salida** en los carriles (dejando que `U` (peso
transformado) se transmita desde un caché pequeño por-(g,ci) mientras `M` de
varios tiles vive en registro) podría cerrar la brecha. Esto es
esencialmente reimplementar el patrón que usan NNPACK/cuDNN para Winograd —
no un ajuste de una tarde. Solo abordar esto si los ítems 2-4 no alcanzan el
objetivo de velocidad, dado el esfuerzo y la incertidumbre del resultado.

### 6. Recorte de NMS con YOLOv10n (esfuerzo alto, especulativo)

Mencionado en commits viejos como candidato (+37% mAP, sin NMS). Requeriría
re-exportar un modelo distinto y reescribir el post-proceso del head
completo — más grande que el ítem 3. No priorizar hasta agotar 1-4; anotado
acá para no perderlo.

---

## Housekeeping (barato, no bloquea nada, vale la pena en paralelo)

- ~~**Comitear el WIP de V180-182**~~ — **HECHO.** De los 104 archivos, 50
  eran un `chmod +x` accidental, 14 artefactos de build trackeados *y*
  gitignoreados, y solo 2 eran fuente real — que resultaron ser la mitad
  faltante de código ya commiteado: **`HEAD` no compilaba** (`yolo_v8n.cpp:586:
  'parallel_async_done' was not declared`), y `main` en GitHub tampoco.
  Arreglado y verificado con un build desde un worktree limpio.
- **CI de compilación** (`GOALS.md` §G3, ya anotado, nunca hecho): un
  workflow que corra `docker run rpi-forge make kernel8.img` en cada push
  atraparía roturas de build sin depender de que alguien lo note a mano.
  Esfuerzo bajo, la imagen Docker ya existe.
- ~~**Confirmar si el throttling térmico observado esta sesión es real**~~ —
  **HECHO (V194): no era throttling.** `soc_status_report()` lee el canal de
  propiedades (get_throttled `0x00030046`, get_clock_rate_measured `0x00030047`,
  get_temperature `0x00030006` — ojo, `0x00030006` es *temperature*, no
  *throttled* como decía la versión anterior de este archivo) e imprime
  `[SOC] arm= temp= thr=` en el boot y por frame en el bench. Medido: `arm=1000MHz`
  y `thr=0x00000000` (ni los bits 16-19 de "ocurrió alguna vez") en corridas a 43,
  46, 50 y 51.5 °C, todas a **504 ms exactos**. Segunda hipótesis, la
  instrumentación `C2F_PROF` por defecto, también falsada: `C2F_PROF=0` da 504 ms.
  Lo que queda establecido es mejor de lo esperado: **el banco repite a ±0 ms**, así
  que un A/B de 2-3 ms es señal, no ruido — no hace falta promediar corridas. El
  492 ms de V192 se midió sin registrar reloj ni throttle; a partir de ahora toda
  cifra va acompañada de su línea `[SOC]`.

  **Corrección (V196): la conclusión anterior estaba medida sobre una ventana
  demasiado corta.** El usuario reportó que en HW los primeros ~100 frames van a
  2 FPS y después baja a 1.2-1.4. Reproducido con `FRAMES=150`: 435 → 705 ms en
  el frame 100. `thr` sigue en 0 (así que "no es throttling" era literalmente
  cierto), pero **el reloj ARM cae de 1000 a 600 MHz** — la ventana de turbo
  inicial del firmware, que bare-metal nunca renueva porque no existe un cpufreq
  que pida el reloj. Un bench de 3 frames mide 2 s después del reset y es ciego a
  esto. Arreglado en V196 (`soc_clock_boost()`, SET_CLOCK_RATE por mailbox):
  435 ms sostenidos en 301 frames. **Regla nueva: toda cifra de rendimiento se
  valida con una corrida larga, no con los 3 frames del bench.**

- **Limpieza de docs menor**: `GOALS.md` §"Known issues" tiene una entrada
  sobre `test_image.bin` a 320² vs 256² que ya no aplica (V184 la resolvió
  con un fix distinto al que describe la entrada) — marcarla resuelta o
  borrarla evita que alguien la lea como vigente.

---

## Lo que NO vale la pena reintentar (ya medido, ver GOALS.md/CLAUDE.md)

INT8 (Tier 1 y 2, A53 sin SDOT), interleave de FMLA, store contiguo vs
`vgetq_lane`, Winograd F(2×2,3×3) en su forma directa (V193), y el reparto
async de V180 (cores 1-3 con el core 0 bombeando — medido en HW con cámara:
383 vs 293 ms por 3 ms de diferencia en el intervalo de repintado; borrado en
V200). No reabrir sin evidencia nueva.

**Retirado de esta lista**: "conv3×3 está cerca del piso del A53". Lo estaba
respecto al kernel de V191, no respecto al A53 — V195/V197/V199 le sacaron
~40 % al mismo kernel sin tocar una FMA. La evidencia que lo delató estaba a
la vista: el `conv1x1` de este mismo repo corre a 2.5 GMAC/s y el `conv3×3` a
1.3-1.8.
