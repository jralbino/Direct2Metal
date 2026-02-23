import struct
import numpy as np
import torch
import torch.nn as nn
import os

def repack_for_neon(weight_np):
    C_out, C_in, K, _ = weight_np.shape
    C_out_pad = ((C_out + 3) // 4) * 4
    w = weight_np.reshape(C_out, C_in * K * K).astype('float32')
    if C_out_pad > C_out:
        w = np.concatenate([w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype='float32')], axis=0)
    w = w.reshape(C_out_pad // 4, 4, C_in * K * K)
    w = w.transpose(0, 2, 1) 
    return w.flatten()

def write_tensor(f, arr):
    flat = arr.astype(np.float32).flatten()
    f.write(struct.pack('<I', len(flat))) 
    f.write(flat.tobytes())                

def main():
    print("=== Exporting YOLOv5n Weights (Corrected & Robust) ===")
    
    # Cargar modelo (AutoShape fusiona capas automáticamente al cargar)
    model = torch.hub.load('ultralytics/yolov5', 'yolov5n', pretrained=True)
    model.cpu()
    model.eval()

    path = os.path.join(os.path.dirname(__file__), '..', 'src', 'weights.bin')
    
    with open(path, 'wb') as f:
        # 1. Exportar Pesos
        print("Exportando capas...")
        for name, m in model.named_modules():
            if isinstance(m, nn.Conv2d):
                w = m.weight.detach().cpu().numpy()
                C_out = w.shape[0]
                if C_out % 4 == 0:
                    repacked = repack_for_neon(w)
                    write_tensor(f, repacked)
                else:
                    write_tensor(f, w)
                
                if m.bias is not None:
                    write_tensor(f, m.bias.detach().cpu().numpy())
                else:
                    write_tensor(f, np.zeros(w.shape[0], dtype=np.float32))

        # 2. Buscar y Exportar Anchors Dinámicamente
        print("\n--- Buscando Capa Detect ---")
        detect_layer = None
        # Buscamos el módulo cuyo nombre de clase sea 'Detect'
        for m in model.modules():
            if type(m).__name__ == 'Detect':
                detect_layer = m
                break
        
        if detect_layer:
            anchors = detect_layer.anchors.detach().cpu().numpy()
            stride = detect_layer.stride.detach().cpu().numpy()
            print("Anchors encontrados:")
            for i, a in enumerate(anchors):
                # Dividimos por stride para tener el valor relativo al grid si es necesario, 
                # pero YOLOv5 suele guardarlos en píxeles absolutos en .anchors
                print(f"Capa {i} (Stride {stride[i]}): {a.tolist()}")
        else:
            print("ADVERTENCIA: No se encontró la capa Detect automáticamente.")
            # Fallback a los valores estándar de YOLOv5n si falla la detección
            print("Usando valores estándar YOLOv5n:")
            print("Stride 8:  [[10,13], [16,30], [33,23]]")
            print("Stride 16: [[30,61], [62,45], [59,119]]")
            print("Stride 32: [[116,90], [156,198], [373,326]]")
                
    print(f"\nDone. Exported to {path}")

if __name__ == "__main__":
    main()