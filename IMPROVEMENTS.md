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

## Estado de partida

- **491-492 ms/frame** en HW real @ 1 GHz stock, YOLOv8n 320×192, campo de
  visión completo (V192). Desglose: `bb=183 neck=99 head=178` (+ cap/l0/render
  menores).
- **V193 (Winograd F(2×2,3×3)): implementado, correcto, pero más lento** (+6%).
  Cerrado y documentado; código vivo detrás de `WINOGRAD=0` (default) por si
  alguien retoma la reescritura GEMM que sí podría ganar (ver ítem 5).
- El ítem más caro que queda es el head DFL: **4 conv3×3 (~97 ms)**, ~3.5× el
  piso de cómputo NEON del A53 — ya cerca del límite para conv directa.
- `tools/env` está roto (intérpretes de 0 bytes) — **bloquea cualquier cosa
  que necesite re-exportar pesos** (torch/ultralytics). Esto es un bloqueador
  transversal para varios ítems de abajo.
- Hay ~104 archivos sin commitear (V180 ping-pong no bloqueante, V181 thumbnail
  debug, V182 tracker con predicción de velocidad) — trabajo del usuario en
  curso, no tocado en esta sesión.

---

## Ítems, en orden recomendado

### 1. Arreglar `tools/env` (bloqueador, esfuerzo bajo)

```
python3 -m venv tools/env && tools/env/bin/pip install -r tools/requirements.txt
```

Sin esto, los ítems 3, 4 y 6 no se pueden ni empezar (todos necesitan
`export_model.py`/torch). Verificar primero si este entorno tiene salida a
internet para el `pip install` (torch + ultralytics son descargas grandes);
si no la tiene, hay que resolverlo desde una máquina con acceso y traer la
venv ya armada.

### 2. Barrido de perfil `C2F_PROF` antes de asumir que ya no hay más aire (esfuerzo bajo, impacto incierto pero barato de descartar)

V190 encontró un bug real de *thrash* de L1 en el `conv1x1` de L2 (55 ms
sobre un piso de ~3 ms) que nadie había visto hasta perfilar por capa. No hay
garantía de que sea el único. `make bench C2F_PROF=N` para cada N de C2f
(2,4,6,8,12,15,18,21) más `make bench HEAD_PROF=1` para P4/P5 (ya lo tenemos
para P3) da un mapa completo por-conv. Buscar cualquier conv1x1 o conv3×3 que
esté a >5-6× su piso teórico (proporcional a `C_in·C_out·K²·HW`) — esa es la
señal que delató a L2. Si no aparece nada, confirma que V191 realmente dejó
el codebase en el piso y cierra la pregunta con evidencia en vez de
suposición.

### 3. Head DFL: probar 1 conv por rama en vez de 2 (esfuerzo medio, impacto ~45-50 ms)

Cada rama del head (box/cls, en P3/P4/P5) apila 2 conv3×3 antes del conv1x1
final. Quitar la segunda reduciría el ítem más caro del grafo (~97 ms → ~50 ms)
sin tocar el kernel NEON. Riesgo: es un cambio de arquitectura, no un
re-empaquetado de pesos — típicamente pierde exactitud sin reentrenar. Camino
barato para acotar el riesgo **antes** de comprometerse:

1. Con `tools/env` arreglado, cargar los pesos YOLOv8n pre-entrenados de
   `ultralytics`, **cortar quirúrgicamente** la segunda conv de cada rama
   (ej. dejarla como identidad, o fusionar sus pesos aproximando con la
   primera) y correr inferencia en PyTorch sobre unas ~20 imágenes de
   `coco128` para ver cuánto cae el mAP/confianza *sin* reentrenar.
2. Si la caída es aceptable (a validar con el usuario, no asumir un umbral),
   recién ahí vale la pena exportar y portar al kernel. Si no, esto se cierra
   rápido y barato — mismo espíritu que el descarte de Winograd: medir antes
   de comprometer una sesión completa.

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

- **Comitear el WIP de V180-182** en incrementos lógicos. 104 archivos sin
  commitear durante semanas es frágil — un accidente de `git` (como el que
  yo mismo cometí esta sesión con `git stash`, revertido sin pérdida) podría
  no tener la misma suerte la próxima vez.
- **CI de compilación** (`GOALS.md` §G3, ya anotado, nunca hecho): un
  workflow que corra `docker run rpi-forge make kernel8.img` en cada push
  atraparía roturas de build sin depender de que alguien lo note a mano.
  Esfuerzo bajo, la imagen Docker ya existe.
- **Confirmar si el throttling térmico observado esta sesión es real**: tras
  ~10 corridas de `make bench` seguidas a 1 GHz sin disipador, el frame time
  subió de 492→503-504 ms de forma estable (no ruido — se repitió idéntico en
  2 corridas). No se investigó a fondo por no bloquear el trabajo de
  Winograd. El BCM2837 expone `get_throttled` por el mismo mailbox de
  propiedades que ya usa el kernel para el framebuffer (tag `0x00030006`) —
  agregar una lectura de ese tag a `kernel_main`/`prof_dump` daría una señal
  directa (bits de throttle-actual/ever) en vez de inferirlo por timing.
  Importa para cualquier claim de "X ms/frame" en un despliegue real
  (duty cycle, caja cerrada, etc.).
- **Limpieza de docs menor**: `GOALS.md` §"Known issues" tiene una entrada
  sobre `test_image.bin` a 320² vs 256² que ya no aplica (V184 la resolvió
  con un fix distinto al que describe la entrada) — marcarla resuelta o
  borrarla evita que alguien la lea como vigente.

---

## Lo que NO vale la pena reintentar (ya medido, ver GOALS.md/CLAUDE.md)

INT8 (Tier 1 y 2, A53 sin SDOT), interleave de FMLA, store contiguo vs
`vgetq_lane`, y ahora Winograd F(2×2,3×3) en su forma directa (V193). No
reabrir sin evidencia nueva.
