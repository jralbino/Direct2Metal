#ifndef MODEL_CONFIG_H
#define MODEL_CONFIG_H

// Definicion de Capa 1 (MicroYOLO)
#define L1_IN_CH 3
#define L1_OUT_CH 16
#define L1_KER_SZ 3
#define L1_STRIDE 1
#define L1_PAD 1

// AGREGA ESTA LINEA SI FALTA:
#define TOTAL_WEIGHTS_SIZE 1728 // 16*3*3*3 floats * 4 bytes

#endif