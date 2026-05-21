/* src/watchdog.h — BCM2835 hardware watchdog interface */
#ifndef WATCHDOG_H
#define WATCHDOG_H

/* Initialize the BCM2835 watchdog with a timeout in milliseconds (max ~16000ms).
 * A full system reset is triggered if watchdog_kick() is not called within timeout_ms. */
void watchdog_init(unsigned int timeout_ms);

/* Reset the watchdog countdown. Call at the start of each inference frame. */
void watchdog_kick();

#endif /* WATCHDOG_H */
