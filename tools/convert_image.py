import cv2
import numpy as np
import struct
import sys

def main():
    # Cambia 'zidane.jpg' por el nombre de tu imagen
    img_path = 'src/bus.jpg' 
    if len(sys.argv) > 1:
        img_path = sys.argv[1]

    print(f"Procesando {img_path}...")
    img = cv2.imread(img_path)
    if img is None:
        print("Error: No se encontró la imagen.")
        return

    # 1. Resize a 640x640 (YOLO input)
    img = cv2.resize(img, (320, 320))

    # 2. BGR a RGB
    img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

    # 3. Normalizar (0-255 -> 0.0-1.0) y Planar (HWC -> CHW)
    # YOLO espera: Canal R completo, luego G completo, luego B completo
    img = img.astype(np.float32) / 255.0
    img = img.transpose(2, 0, 1) # [3, 640, 640]

    # 4. Aplanar y guardar binario
    flat_data = img.flatten()
    
    output_path = 'src/test_image.bin'
    with open(output_path, 'wb') as f:
        f.write(struct.pack(f'{len(flat_data)}f', *flat_data))
    
    print(f"Generado {output_path} ({len(flat_data)*4} bytes).")

if __name__ == "__main__":
    main()