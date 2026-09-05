#!/usr/bin/env python3
"""
eval_reference.py — referencia PyTorch sobre EL MISMO tensor que ve el kernel.

El banco (`make bench`) compara la numérica bit a bit contra un golden: perfecto
para cambios de kernel, pero no dice nada sobre calidad de detección. Cualquier
cambio que toque el modelo —recortar el head, cambiar de YOLOv8n a v11n, subir
la resolución— necesita un instrumento distinto, y hasta ahora el único era
"el reloj se detecta al 56 % en un frame".

Esto ejecuta YOLOv8n en PyTorch sobre `test_image.bin` (el tensor exacto que
entra al grafo bare-metal tras debayer+ISP, ya normalizado a [0,1] CHW RGB) y
saca las detecciones en el mismo formato que `GOLDEN` de hwbench.py. La
diferencia entre ambos es la fidelidad del port; la diferencia entre dos
corridas de esto es el efecto del cambio de modelo, sin el port de por medio.

Umbrales leídos de bsp/safety_config.h para que no puedan derivar del kernel.

Uso:
    docker run --rm --user $(id -u):$(id -g) -v $(pwd):/app d2m-torch \
        python tools/eval_reference.py
    ... --tensor cam_frame.bin --compare "[(74, 56)]"
"""
import argparse
import os
import re
import sys

import numpy as np
import torch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _defines(path, names):
    txt = open(os.path.join(ROOT, path)).read()
    out = {}
    for n in names:
        m = re.search(r"^\s*#define\s+%s\s+([0-9.]+)f?" % n, txt, re.M)
        if m:
            out[n] = float(m.group(1))
    return out


def wh_from_bsp():
    d = _defines("bsp/bsp.h", ["YOLO_W", "YOLO_H", "YOLO_IN"])
    if "YOLO_W" in d and "YOLO_H" in d:
        return int(d["YOLO_W"]), int(d["YOLO_H"])
    if "YOLO_IN" in d:
        return int(d["YOLO_IN"]), int(d["YOLO_IN"])
    sys.exit("YOLO_W/YOLO_H no encontrados en bsp/bsp.h; usa --wh")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--app", default="yolo_v8n_coco")
    ap.add_argument("--tensor", help="default: app/<APP>/test_image.bin")
    ap.add_argument("--weights", help="checkpoint .pt (default: yolov8n.pt del repo)")
    ap.add_argument("--wh", type=int, nargs=2, metavar=("W", "H"),
                    help="default: de bsp/bsp.h")
    ap.add_argument("--conf", type=float, help="default: CONF_THRESH de bsp/safety_config.h")
    ap.add_argument("--iou", type=float, help="default: NMS_THRESH de bsp/safety_config.h")
    ap.add_argument("--compare", help='detecciones del kernel, p.ej. "[(74, 56)]"')
    ap.add_argument("--boxes", action="store_true", help="imprimir cajas además de clases")
    args = ap.parse_args()

    w, h = tuple(args.wh) if args.wh else wh_from_bsp()
    thr = _defines("bsp/safety_config.h", ["CONF_THRESH", "NMS_THRESH"])
    conf = args.conf if args.conf is not None else thr.get("CONF_THRESH", 0.5)
    iou = args.iou if args.iou is not None else thr.get("NMS_THRESH", 0.35)

    tensor_path = args.tensor or os.path.join(ROOT, "app", args.app, "test_image.bin")
    ckpt = args.weights or os.path.join(ROOT, "yolov8n.pt")

    want = 3 * w * h * 4
    have = os.path.getsize(tensor_path)
    if have != want:
        sys.exit(f"{tensor_path}: {have} B pero {w}x{h} necesita {want} B "
                 f"(3*{w}*{h} floats). Regenera con tools/convert_image.py.")

    x = np.fromfile(tensor_path, dtype=np.float32).reshape(1, 3, h, w)
    print(f"tensor   {tensor_path}")
    print(f"         [3][{h}][{w}] float32 CHW RGB, rango [{x.min():.3f}, {x.max():.3f}], "
          f"media {x.mean():.3f}")
    print(f"umbrales conf>{conf} iou={iou}  (de bsp/safety_config.h)")

    from ultralytics import YOLO
    # En 8.4 la NMS vive en ultralytics.utils.nms; en versiones anteriores
    # estaba en ultralytics.utils.ops. Se prueban las dos para que el harness
    # no se rompa si alguien mueve el pin de la imagen.
    try:
        from ultralytics.utils.nms import non_max_suppression
    except ImportError:
        from ultralytics.utils.ops import non_max_suppression

    yolo = YOLO(ckpt)
    model = yolo.model
    model.eval()
    model.fuse()
    names = model.names

    with torch.no_grad():
        preds = model(torch.from_numpy(x))
    if isinstance(preds, (list, tuple)):
        preds = preds[0]
    det = non_max_suppression(preds, conf_thres=conf, iou_thres=iou)[0]

    got = []
    print(f"\ndetecciones ({len(det)}):")
    for *xyxy, c, cls in det.tolist():
        cls = int(cls)
        pct = int(c * 100)
        got.append((cls, pct))
        box = ("  box=[%.0f %.0f %.0f %.0f]" % tuple(xyxy)) if args.boxes else ""
        print(f"  cls={cls:3d} {names[cls]:<16s} {pct:3d}%{box}")
    got.sort()
    print("\nREFERENCE =", got)

    if args.compare:
        kern = sorted(tuple(t) for t in eval(args.compare))  # noqa: S307 — entrada propia
        print("KERNEL    =", kern)
        rc = {c for c, _ in kern}
        rr = {c for c, _ in got}
        print("\nclases solo en la referencia:", sorted(rr - rc) or "ninguna")
        print("clases solo en el kernel:    ", sorted(rc - rr) or "ninguna")
        common = sorted(rr & rc)
        if common:
            dk = dict(kern)
            dr = dict(got)
            print("confianza en las comunes (kernel - referencia):")
            for c in common:
                print(f"  cls={c:3d} {names[c]:<16s} {dk[c]:3d}% - {dr[c]:3d}% = {dk[c]-dr[c]:+d} pp")


if __name__ == "__main__":
    main()
