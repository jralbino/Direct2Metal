import struct
import numpy as np
import torch
import torch.nn as nn
import os

def repack_for_neon(weight_np):
    """
    Empaqueta pesos espaciales (kxk donde k>1) para lectura eficiente NEON vld1q.
    Formato: [C_out//4, C_in*K*K, 4]
    """
    C_out, C_in, K, _ = weight_np.shape
    
    # Padding si C_out no es múltiplo de 4
    C_out_pad = ((C_out + 3) // 4) * 4
    
    # Aplanar los pesos espaciales (C_in * K * K)
    w = weight_np.reshape(C_out, C_in * K * K).astype('float32')
    
    # Aplicar padding de ceros si es necesario
    if C_out_pad > C_out:
        w = np.concatenate([w, np.zeros((C_out_pad - C_out, C_in * K * K), dtype='float32')], axis=0)
        
    # Reshape mágico para NEON: Agrupar 4 canales de salida contiguos
    w = w.reshape(C_out_pad // 4, 4, C_in * K * K)
    
    # Transponer para que los 4 valores del canal estén contiguos en memoria [0, 2, 1]
    # Resultado: [Groups, SpatialPixels, 4_Channel_Block]
    w = w.transpose(0, 2, 1) 
    return w.flatten()

def write_tensor(f, arr):
    flat = arr.astype(np.float32).flatten()
    f.write(struct.pack('<I', len(flat))) 
    f.write(flat.tobytes())                

def main():
    print("=== Exporting YOLOv5n Weights (Hybrid Layout) ===")
    
    # Cargar modelo oficial
    model = torch.hub.load('ultralytics/yolov5', 'yolov5n', pretrained=True)
    model.cpu()
    model.eval()

    path = os.path.join(os.path.dirname(__file__), '..', 'src', 'weights.bin')
    
    total_bytes = 0
    
    with open(path, 'wb') as f:
        print(f"{'Layer Name':<40} | {'Shape':<20} | {'Layout Strategy':<15}")
        print("-" * 85)
        
        for name, m in model.named_modules():
            if isinstance(m, nn.Conv2d):
                w = m.weight.detach().cpu().numpy()
                shape = w.shape
                C_out, C_in, K_h, K_w = shape
                
                # ESTRATEGIA DE EXPORTACIÓN HÍBRIDA
                # Caso A: Convolución 1x1 -> Usamos motor ops_neon_conv1x1 (espera layout plano NCHW)
                if K_h == 1 and K_w == 1:
                    strategy = "FLAT (1x1)"
                    write_tensor(f, w)
                    
                # Caso B: Convolución Espacial (3x3, 6x6) -> Usamos motor conv2d_partial (espera REPACK NEON)
                else:
                    # Solo podemos empaquetar si los canales son divisibles por 4
                    # (Nuestro kernel C++ actual requiere esto para 3x3)
                    if C_out % 4 == 0:
                        strategy = "REPACK (NEON)"
                        repacked = repack_for_neon(w)
                        write_tensor(f, repacked)
                    else:
                        # Fallback raro (ej. primera capa si fuera C_out=3, pero YOLO es 16)
                        strategy = "FLAT (Fallback)"
                        write_tensor(f, w)
                
                print(f"{name:<40} | {str(shape):<20} | {strategy:<15}")
                
                # Bias siempre es plano
                if m.bias is not None:
                    write_tensor(f, m.bias.detach().cpu().numpy())
                else:
                    write_tensor(f, np.zeros(C_out, dtype=np.float32))

        # Exportar Anchors (informativo/backup)
        # ... (Tu código de anchors estaba bien, lo omito por brevedad pero déjalo igual) ...
                
    print(f"\nDone. Exported to {path}")

if __name__ == "__main__":
    main()