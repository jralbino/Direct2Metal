import struct
import numpy as np
import torch
import torch.nn as nn
import os

def pack_int8_neon(weight_q):
    """ Empaqueta los pesos INT8 para lecturas continuas vld1q_s8 """
    C_out, C_in, K_h, K_w = weight_q.shape
    C_out_pad = ((C_out + 3) // 4) * 4
    w = weight_q.reshape(C_out, C_in * K_h * K_w)
    
    if C_out_pad > C_out:
        w = np.concatenate([w, np.zeros((C_out_pad - C_out, w.shape[1]), dtype=np.int8)], axis=0)
        
    w = w.reshape(C_out_pad // 4, 4, C_in * K_h * K_w)
    w = w.transpose(0, 2, 1) 
    return w.flatten()

def write_layer_int8(f, w_fp32, bias_fp32):
    C_out, C_in, K_h, K_w = w_fp32.shape
    
    # 1. Calcular la Escala Simétrica (Per-Tensor Quantization)
    max_val = np.max(np.abs(w_fp32))
    scale = max_val / 127.0 if max_val > 0 else 1.0
    
    # 2. Cuantizar a INT8 (W_int8 = W_fp32 / Scale)
    w_int8 = np.round(w_fp32 / scale).astype(np.int8)
    w_int8 = np.clip(w_int8, -127, 127)
    
    # 3. Empaquetar
    if K_h > 1 and C_out % 4 == 0:
        w_packed = pack_int8_neon(w_int8)
    else:
        w_packed = w_int8.flatten()
        
    # 4. Escribir en disco
    # [Total Elementos UINT32] + [Scale FLOAT32] + [Pesos INT8]
    f.write(struct.pack('<I', len(w_packed)))
    f.write(struct.pack('<f', scale))
    f.write(w_packed.tobytes())
    
    # 5. Escribir el Bias en FP32 puro (Lo sumaremos al final del acumulador INT32)
    if bias_fp32 is not None:
        f.write(struct.pack('<I', len(bias_fp32)))
        f.write(bias_fp32.astype(np.float32).tobytes())
    else:
        zeros = np.zeros(C_out, dtype=np.float32)
        f.write(struct.pack('<I', len(zeros)))
        f.write(zeros.tobytes())

def main():
    print("=== Exporting YOLOv5n Weights (INT8 Quantization) ===")
    model = torch.hub.load('ultralytics/yolov5', 'yolov5n', pretrained=True)
    model.cpu()
    model.eval()

    # Guardamos en un archivo nuevo para no romper el modelo FP32
    path = os.path.join(os.path.dirname(__file__), '..', 'src', 'weights_int8.bin')
    
    with open(path, 'wb') as f:
        for name, m in model.named_modules():
            if isinstance(m, nn.Conv2d):
                w = m.weight.detach().cpu().numpy()
                b = m.bias.detach().cpu().numpy() if m.bias is not None else None
                print(f"Quantizing [{name}] -> INT8")
                write_layer_int8(f, w, b)

    print(f"\nDone. Exported to {path}")
    
    # Mostrar el tamaño de la magia
    size_mb = os.path.getsize(path) / (1024 * 1024)
    print(f"New INT8 Model Size: {size_mb:.2f} MB")

if __name__ == "__main__":
    main()