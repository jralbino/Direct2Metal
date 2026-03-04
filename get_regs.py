import urllib.request, re

print("Descargando driver de Linux...")
url = "https://raw.githubusercontent.com/raspberrypi/linux/rpi-6.6.y/drivers/media/i2c/imx708.c"
text = urllib.request.urlopen(url).read().decode('utf-8')

# Cortar justo el bloque de 1536x864
block = text.split("static const struct imx708_reg mode_2x2binned_720p_regs[]")[1].split("};")[0]
regs = re.findall(r'\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}', block)

with open('src/imx708_regs.h', 'w') as f:
    f.write("static const RegVal k_imx708_init[] = {\n")
    for r, v in regs:
        f.write(f"    {{ {r}, {v} }},\n")
    f.write("};\n")
    f.write("static const int k_imx708_init_len = sizeof(k_imx708_init)/sizeof(RegVal);\n")

print(f"¡EXITO! {len(regs)} registros extraídos en src/imx708_regs.h")