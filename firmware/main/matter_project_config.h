#pragma once

#include <sdkconfig.h>

#ifndef CONFIG_ESP_MATTER_ENABLE_MATTER_SERVER
#ifdef CONFIG_ESP_MATTER_COMMISSIONER_ENABLE
/* One-OS receives Matter candidates from other application-owned L2 evidence. */
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONER_DISCOVERY 0
#endif

#define CHIP_CONFIG_CONTROLLER_MAX_ACTIVE_DEVICES 8
#endif
