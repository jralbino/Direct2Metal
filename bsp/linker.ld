ENTRY(_start)

SECTIONS
{
    /* La Raspberry Pi carga el kernel en 0x80000 (64-bit) */
    . = 0x80000;

    /* Código (Text) */
    .text : {
        KEEP(*(.text.boot))  /* Código de arranque primero */
        *(.text)             /* Resto del código */
    }

    /* Datos de solo lectura (aquí van tus pesos y la imagen) */
    .rodata : {
        *(.rodata)
    }

    /* Datos inicializados (Data) */
    .data : {
        *(.data)
    }

    /* Datos NO inicializados (BSS) */
    . = ALIGN(16);
    __bss_start = .;
    .bss : {
        bss = .;
        *(.bss)
    }
    . = ALIGN(16);
    __bss_end = .;
    
    /* Calculamos el tamaño por si lo necesitamos */
    __bss_size = __bss_end - __bss_start;
}