/* COM1 log helpers used by the AMD GPU driver (thin wrappers over serial.h). */
#ifndef HARIS_GPU_LOG_H
#define HARIS_GPU_LOG_H

#include "gpu_types.h"

void serial_print(const char *s);
void serial_hex32(uint32_t value);
void serial_dec32(uint32_t value);

#endif
