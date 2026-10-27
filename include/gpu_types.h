/* Fixed-width integer types for the AMD GPU driver (freestanding: no <stdint.h>). */
#ifndef HARIS_GPU_TYPES_H
#define HARIS_GPU_TYPES_H

typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int   uint32_t;
typedef unsigned long long uint64_t;

typedef signed char  int8_t;
typedef signed short int16_t;
typedef signed int   int32_t;
typedef signed long long int64_t;


/* The GPU is given 32-bit addresses of heap blocks (PSP ring, firmware buffers, GART tables). HarisOS is 64-bit, so a
   block above 4 GiB would be cut off and the GPU would read or write the wrong memory: callers refuse such blocks. */
#define GPU_DMA_LIMIT 0x100000000ULL

#endif
