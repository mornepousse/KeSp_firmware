#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "display_backend.h"

/* Sharp memory-LCD backend for the Niphargus halves (portrait 68 x 160).
 * Spec: docs/superpowers/specs/2026-09-14-ecrans-memlcd-design.md */
extern const display_backend_t memlcd_display_backend;
