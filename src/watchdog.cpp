/* src/watchdog.cpp — BCM2835 hardware watchdog (PM peripheral)
 *
 * Registers at 0x3F100000 (BCM2835 ARM Peripherals § 12.1).
 * Watchdog clock tick rate ≈ 65536 ticks/second.
 * Max timeout: 0xFFFFF ticks ≈ 16 seconds.
 */
#include "watchdog.h"

#define PM_BASE     0x3F100000UL
#define PM_RSTC     (*((volatile unsigned int*)(PM_BASE + 0x1C)))
#define PM_WDOG     (*((volatile unsigned int*)(PM_BASE + 0x24)))

#define PM_PASSWORD              0x5A000000U
#define PM_RSTC_WRCFG_CLR       0xFFFFFCFFU
#define PM_RSTC_WRCFG_FULL_RESET 0x00000020U
#define PM_WDOG_TIME_SET         0x000FFFFFU

static unsigned int g_wdog_ticks = 0;
static bool g_watchdog_armed = false;

void watchdog_init(unsigned int timeout_ms) {
    g_wdog_ticks = timeout_ms * 65536U / 1000U;
    if (g_wdog_ticks > PM_WDOG_TIME_SET)
        g_wdog_ticks = PM_WDOG_TIME_SET;
    PM_WDOG = PM_PASSWORD | g_wdog_ticks;
    PM_RSTC = PM_PASSWORD | (PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;
    g_watchdog_armed = true;
}

void watchdog_kick() {
    /* Guard: do nothing if watchdog was never initialized (e.g. on QEMU).
     * Calling with g_wdog_ticks==0 would arm a 0-tick timeout → immediate reset. */
    if (!g_watchdog_armed) return;
    PM_WDOG = PM_PASSWORD | g_wdog_ticks;
    PM_RSTC = PM_PASSWORD | (PM_RSTC & PM_RSTC_WRCFG_CLR) | PM_RSTC_WRCFG_FULL_RESET;
}
