import torch
import torch.nn as nn
import struct
import os

# 1. Definimos una Red Neuronal "Juguete" (Mock YOLO)
#    Solo para probar que nuestro C++ Bare Metal puede cargar pesos y ejecutar convoluciones.
class MicroYOLO(nn.Module):
    def __init__(self):
        super(MicroYOLO, self).__init__()
        # Capa 1: Conv2d (3 canales in -> 16 canales out, kernel 3x3, padding 1)
        # Esto mantiene el tamaño de la imagen (640x480)
        self.conv1 = nn.Conv2d(in_channels=3, out_channels=16, kernel_size=3, stride=1, padding=1, bias=False)
        
        # Capa 2: BatchNorm (Simplificado: fusionaremos esto en la conv en el futuro)
        # Por ahora, solo Conv.

    def forward(self, x):
        return self.conv1(x)

def export_weights(model, filename="weights.bin"):
    print(f"Exportando pesos a {filename}...")
    
    with open(filename, "wb") as f:
        # Recorremos todas las capas
        for name, param in model.named_parameters():
            print(f"Procesando capa: {name} | Forma: {param.data.shape}")
            
            # Aplanamos los pesos (Flatten)
            # PyTorch guarda en formato (Out_Channels, In_Channels, H, W)
            # Nosotros queremos escribirlo linealmente en memoria.
            weights_flat = param.data.numpy().flatten()
            
            # Escribimos como floats de 32 bits (4 bytes)
            # 'f' formato es float32 standard
            bytes_written = f.write(struct.pack(f'{len(weights_flat)}f', *weights_flat))
            print(f"  -> Escritos {bytes_written} bytes.")

if __name__ == "__main__":
    # Inicializar modelo con pesos aleatorios (pero deterministas para debug)
    torch.manual_seed(42)
    model = MicroYOLO()
    model.eval()

    # Exportar binario
    export_weights(model, "../src/weights.bin")
    
    # Exportar Header C++ con las constantes
    with open("../src/model_config.h", "w") as f:
        f.write("#ifndef MODEL_CONFIG_H\n")
        f.write("#define MODEL_CONFIG_H\n\n")
        f.write("// Definicion de la Capa 1\n")
        f.write("#define L1_IN_CH 3\n")
        f.write("#define L1_OUT_CH 16\n")
        f.write("#define L1_KER_SZ 3\n")
        f.write(f"// Total weights: {16*3*3*3} floats\n")
        f.write("#endif\n")
        
    print("\nListo! Archivos generados en carpeta src/")