#pragma once

#include <sdkconfig.h>

/* Matter candidates are application-owned; controller L2 never scans for them. */
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY 0

/* Bound controller CASE/session bookkeeping to the One-OS embedded use case. */
#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES 8
