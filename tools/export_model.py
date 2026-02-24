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
    print("=== Exporting YOLOv5n Weights (FP32 Optimized) ===")
    model = torch.hub.load('ultralytics/yolov5', 'yolov5n', pretrained=True)
    model.cpu()
    model.eval()

    path = os.path.join(os.path.dirname(__file__), '..', 'src', 'weights.bin')
    
    with open(path, 'wb') as f:
        print("Exportando capas...")
        for name, m in model.named_modules():
            if isinstance(m, nn.Conv2d):
                w = m.weight.detach().cpu().numpy()
                C_out, C_in, K_h, K_w = w.shape
                
                # Regla de Oro: Solo empaquetar 3x3 y 6x6. Dejar 1x1 planos.
                if K_h > 1 and C_out % 4 == 0:
                    repacked = repack_for_neon(w)
                    write_tensor(f, repacked)
                else:
                    write_tensor(f, w)
                
                if m.bias is not None:
                    write_tensor(f, m.bias.detach().cpu().numpy())
                else:
                    write_tensor(f, np.zeros(w.shape[0], dtype=np.float32))

    print(f"\nDone. Exported to {path}")

if __name__ == "__main__":
    main()