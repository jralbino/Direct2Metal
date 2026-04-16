import sys
from PIL import Image

def recover_image(log_filename, output_filename, width, height):
    print(f"Leyendo {log_filename}...")
    
    with open(log_filename, 'r', errors='ignore') as f:
        lines = f.readlines()

    start_idx = -1
    end_idx = -1
    
    for i, line in enumerate(lines):
        if '---PPM_START---' in line or 'P3' in line.strip():
            start_idx = i + 1
            break
            
    if start_idx != -1:
        for i in range(start_idx, len(lines)):
            if '---PPM_END---' in lines[i]:
                end_idx = i
                break

    if start_idx == -1 or end_idx == -1:
        print("Error: No se encontró un bloque de imagen completo.")
        return

    print(f"Imagen detectada entre las lineas {start_idx} y {end_idx}")

    pixels = []
    for line in lines[start_idx:end_idx]:
        tokens = line.replace('-', ' ').replace('+', ' ').split()
        for token in tokens:
            if token.isdigit():
                pixels.append(max(0, min(255, int(token))))

    required_vals = width * height * 3
    print(f"Extraídos {len(pixels)} valores (Se requieren {required_vals}).")
    
    if len(pixels) > required_vals:
        pixels = pixels[:required_vals]
    elif len(pixels) < required_vals:
        pixels.extend([0] * (required_vals - len(pixels)))

    img_data = [(pixels[i], pixels[i+1], pixels[i+2]) for i in range(0, required_vals, 3)]

    img = Image.new('RGB', (width, height))
    img.putdata(img_data)
    img.save(output_filename)
    print(f"¡Éxito! Imagen guardada como {output_filename}")

recover_image('uart_log.txt', 'recovered.png', 160, 160)
