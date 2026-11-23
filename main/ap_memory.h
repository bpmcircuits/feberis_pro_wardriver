#pragma once
#include "core/wardrive.h"
#include <stdbool.h>
#include <stddef.h>

/* Returns false without internal-RAM fallback; UART diagnostics remain usable. */
bool ap_memory_init(wd_engine_t *engine);
size_t ap_memory_reserved(void);
