#pragma once
#include "esp_err.h"

/* On-device species identification (FSD §3.2): quantized int8 classifier
 * (esp-dl / TFLite-Micro) on the S3; asynchronous — never delays capture.
 * Top-3 + confidence; below threshold -> "Unidentified bird"; "no bird"
 * guard class. Disabled/bird-only on ESP32-classic. */

esp_err_t classify_init(void);
