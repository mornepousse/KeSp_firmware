#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "display_backend.h"

/* Backend Sharp memory-LCD des moitiés Niphargus (portrait 68 × 160).
 * Spec : docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
extern const display_backend_t memlcd_display_backend;
