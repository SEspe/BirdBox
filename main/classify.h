#pragma once
#include <stdint.h>
#include "esp_err.h"

/* On-device species identification (FSD §3.2): quantized int8 classifier
 * (esp-dl / TFLite-Micro) on the S3; asynchronous — never delays capture.
 * Top-3 + confidence; below threshold -> "Unidentified bird"; "no bird"
 * guard class. Disabled/bird-only on ESP32-classic. */

esp_err_t classify_init(void);

/* Last inference duration in ms, for the Debug card (FSD §5); -1 = no
 * inference has run yet (always, until §3.2 lands). */
int32_t classify_last_duration_ms(void);
