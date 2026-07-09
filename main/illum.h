#pragma once
/* Onboard illuminator LED (WS2812, GPIO48 on the reference unit — traced
 * live via the Debug-tab GPIO probe, FSD v1.34/v1.35). Optional: boards
 * without PIN_IR_LED wired just no-op. */
#include <stdbool.h>

void illum_init(void);        /* call once at boot */
bool illum_available(void);   /* true if this board has the hardware wired */
void illum_set(bool on);      /* no-op if unavailable */
