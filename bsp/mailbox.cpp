/* File: src/mailbox.cpp - VIDEOCORE MAILBOX INTERFACE */
#include "mailbox.h"
#include "bsp.h"

// El buffer del Mailbox DEBE estar alineado a 16 bytes.
__attribute__((aligned(16))) volatile uint32_t mbox[36];

// Helpers para limpiar caché
static inline void data_sync_barrier() { asm volatile("dsb sy" : : : "memory"); }
static inline void data_mem_barrier()  { asm volatile("dmb sy" : : : "memory"); }

int mbox_call(unsigned char ch) { // <--- CAMBIO AQUÍ
    uint32_t r = (((uint32_t)((uint64_t)&mbox) & ~0xF) | (ch & 0xF));

    while (*MAILBOX_STATUS & MAILBOX_FULL) { asm volatile("nop"); }

    data_sync_barrier();
    *MAILBOX_WRITE = r;
    data_mem_barrier();

    while (1) {
        while (*MAILBOX_STATUS & MAILBOX_EMPTY) { asm volatile("nop"); }
        
        data_sync_barrier();
        uint32_t data = *MAILBOX_READ;
        data_mem_barrier();

        if ((data & 0xF) == ch) {
            return mbox[1] == 0x80000000;
        }
    }
    return 0;
}
/* ── V194: SoC health readout (property channel) ───────────────────────────
 * Cualquier claim de "X ms/frame" solo vale acompañado del reloj y del
 * estado de throttle con que se midió. Tras ~10 corridas seguidas de
 * `make bench` a 1 GHz sin disipador el frame time subió de 492 a 503-504 ms
 * de forma estable (se repitió idéntico) — un A/B de ±5 % es indistinguible
 * de esa deriva. Esto la vuelve observable en vez de inferida.
 *
 * Un solo round-trip con tres tags:
 *   0x00030046 get_throttled            -> bitmask
 *   0x00030047 get_clock_rate_measured  -> Hz reales del clock ARM (id 3)
 *   0x00030006 get_temperature          -> milésimas de °C
 * (0x00030006 es TEMPERATURE, no throttled — el tag de throttle es 0x46.)
 *
 * La MMU ya está activa cuando esto corre, así que `mbox` (BSS, memoria
 * normal cacheable) necesita mantenimiento explícito: clean antes de que
 * la GPU lea la petición, invalidate después para ver su respuesta.
 * flush_to_ram hace `dc civac` (clean+invalidate), sirve para ambos. */

extern "C" void flush_to_ram(volatile void* addr, unsigned long size);
extern void uart_puts(const char* s);
extern void uart_dec(int n);
extern void uart_hex(uint32_t d);

/* Máximo del clock ARM, cacheado por soc_clock_boost() (V196) para que el
 * reporter pueda notar una caída y volver a pedirlo. 0 = aún no consultado. */
static uint32_t g_soc_max_hz = 0;

#define MBOX_TAG_GET_THROTTLED     0x00030046u
#define MBOX_TAG_GET_CLOCK_MEASURED 0x00030047u
#define MBOX_TAG_GET_TEMPERATURE   0x00030006u
#define MBOX_CLOCK_ID_ARM          3u

bool soc_status_read(soc_status_t* st) {
    st->throttled = 0; st->arm_hz = 0; st->temp_mc = 0;
    st->have_throttled = false; st->have_clock = false; st->have_temp = false;

    mbox[0]  = 17 * 4;
    mbox[1]  = 0;

    mbox[2]  = MBOX_TAG_GET_THROTTLED;      /* value buf 4 B: [bits]        */
    mbox[3]  = 4;  mbox[4]  = 0;  mbox[5]  = 0;

    mbox[6]  = MBOX_TAG_GET_CLOCK_MEASURED; /* value buf 8 B: [id][rate_hz] */
    mbox[7]  = 8;  mbox[8]  = 0;  mbox[9]  = MBOX_CLOCK_ID_ARM; mbox[10] = 0;

    mbox[11] = MBOX_TAG_GET_TEMPERATURE;    /* value buf 8 B: [id][millideg]*/
    mbox[12] = 8;  mbox[13] = 0;  mbox[14] = 0; mbox[15] = 0;

    mbox[16] = 0;                            /* end tag */

    flush_to_ram((volatile void*)mbox, sizeof(mbox));
    int ok = mbox_call(MBOX_CH_PROP);
    flush_to_ram((volatile void*)mbox, sizeof(mbox));
    if (!ok) return false;

    /* Bit 31 del word de request/response = "el firmware atendió este tag".
     * Un tag desconocido lo deja en 0, así que esto distingue "sin soporte"
     * de "soportado y vale 0" — que es justo el caso sano del throttle. */
    if (mbox[4]  & 0x80000000u) { st->throttled = mbox[5];  st->have_throttled = true; }
    if (mbox[8]  & 0x80000000u) { st->arm_hz    = mbox[10]; st->have_clock     = true; }
    if (mbox[13] & 0x80000000u) { st->temp_mc   = (int)mbox[15]; st->have_temp = true; }
    return true;
}

void soc_status_report(const char* tag) {
    soc_status_t st;
    uart_puts(tag ? tag : "[SOC]");
    if (!soc_status_read(&st)) { uart_puts(" mailbox ERROR\n"); return; }

    if (st.have_clock) { uart_puts(" arm="); uart_dec((int)(st.arm_hz / 1000000u)); uart_puts("MHz"); }
    else                 uart_puts(" arm=?");
    if (st.have_temp)  { uart_puts(" temp="); uart_dec(st.temp_mc / 1000);
                         uart_puts("."); uart_dec((st.temp_mc / 100) % 10); uart_puts("C"); }
    else                 uart_puts(" temp=?");
    /* V196: si el reloj se cayó del máximo, volver a pedirlo. Medido: la
     * petición del boot aguanta 301 frames / 66 °C sin recaer, así que esto
     * no debería dispararse nunca — es la red por si el firmware cambia de
     * criterio en un despliegue largo, y deja rastro en el log si pasa. */
    if (st.have_clock && g_soc_max_hz && st.arm_hz + 1000000u < g_soc_max_hz) {
        uint32_t got = soc_clock_boost();
        uart_puts(" REBOOST->"); uart_dec((int)(got / 1000000u)); uart_puts("MHz");
    }
    if (st.have_throttled) {
        uart_puts(" thr="); uart_hex(st.throttled);
        /* Bits bajos = ahora mismo; bits 16-19 = "ocurrió alguna vez". */
        if (st.throttled & SOC_THR_UNDERVOLT_NOW) uart_puts(" UNDERVOLT");
        if (st.throttled & SOC_THR_CAPPED_NOW)    uart_puts(" CAPPED");
        if (st.throttled & SOC_THR_THROTTLED_NOW) uart_puts(" THROTTLED");
        if (st.throttled & SOC_THR_SOFTTEMP_NOW)  uart_puts(" SOFTTEMP");
        if (st.throttled & SOC_THR_EVER_MASK)     uart_puts(" (ever)");
    } else uart_puts(" thr=unsupported");
    uart_puts("\n");
}

/* ── V196: mantener el reloj ARM arriba ────────────────────────────────────
 * Síntoma (reportado por el usuario, reproducido con `--frames 150`): los
 * primeros ~95 frames corren a 435 ms y a partir de ahí a 705 ms, estable.
 * El `[SOC]` de V194 lo explica sin ambigüedad: `arm` pasa de 1000 a 600 MHz
 * con `thr=0x00000000` y la temperatura **bajando** (60.1 → 57.9 °C). No es
 * throttle ni calor: es la ventana de turbo inicial del firmware. En Linux el
 * driver cpufreq pide el reloj por este mismo canal; bare-metal no lo pide
 * nunca, así que al expirar la ventana el VideoCore vuelve a `arm_freq_min`
 * (600 MHz en el BCM2837). 435 × 1000/600 = 725 ≈ 705: el grafo es
 * compute-bound y escala lineal con el reloj (V189).
 *
 * Arreglo: pedirlo explícitamente. GET_MAX_CLOCK_RATE del clock ARM y luego
 * SET_TURBO(1) + SET_CLOCK_RATE a ese máximo, así no hay que hardcodear
 * 1000 MHz ni mantenerlo en sync con `arm_freq` de config.txt. */

#define MBOX_TAG_GET_MAX_CLOCK   0x00030004u
#define MBOX_TAG_SET_TURBO       0x00038005u
#define MBOX_TAG_SET_CLOCK_RATE  0x00038002u

uint32_t soc_clock_boost() {
    /* 1) ¿cuál es el máximo que admite el clock ARM? Preguntarlo evita
     *    hardcodear 1000 MHz y mantenerlo en sync con `arm_freq`. */
    mbox[0] = 8 * 4; mbox[1] = 0;
    mbox[2] = MBOX_TAG_GET_MAX_CLOCK; mbox[3] = 8; mbox[4] = 0;
    mbox[5] = MBOX_CLOCK_ID_ARM;      mbox[6] = 0;
    mbox[7] = 0;
    flush_to_ram((volatile void*)mbox, sizeof(mbox));
    int ok = mbox_call(MBOX_CH_PROP);
    flush_to_ram((volatile void*)mbox, sizeof(mbox));
    if (!ok || !(mbox[4] & 0x80000000u) || mbox[6] == 0) return 0;
    uint32_t max_hz = mbox[6];
    g_soc_max_hz = max_hz;

    /* 2) pedirlo. Un tag por llamada: SET_TURBO (0x00038005) hace que este
     *    firmware rechace el buffer entero con st=0x80000001, así que va
     *    solo SET_CLOCK_RATE — que es el que concede el reloj de todas
     *    formas (rc=0x80000008, hz=0x3B9ACA00). */
    mbox[0] = 9 * 4; mbox[1] = 0;
    mbox[2] = MBOX_TAG_SET_CLOCK_RATE; mbox[3] = 12; mbox[4] = 12;
    mbox[5] = MBOX_CLOCK_ID_ARM; mbox[6] = max_hz; mbox[7] = 0;
    mbox[8] = 0;
    flush_to_ram((volatile void*)mbox, sizeof(mbox));
    ok = mbox_call(MBOX_CH_PROP);
    flush_to_ram((volatile void*)mbox, sizeof(mbox));
    return (ok && (mbox[4] & 0x80000000u)) ? mbox[6] : 0;
}
