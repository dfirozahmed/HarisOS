# HarisOS

HarisOS is a **learning project** where I build a small operating system from scratch, step by step, to understand how computers work at the lowest level.

The goal is to learn: BIOS booting, real mode, protected mode, interrupts, drivers, memory management, and more.

## Requirements

- [NASM](https://www.nasm.us/) (assembler)
- [Clang](https://clang.llvm.org/) (Compiler)
or
MSYS2 UCRT64
```bash
pacman -S mingw-w64-ucrt-x86_64-clang
```
- [QEMU](https://www.qemu.org/) (emulator)
- [CMake](https://cmake.org/) 3.16 or newer (or `make`)

## Build and Run (CMake)

```bash
cmake -S . -B build                    # configure (once)
cmake --build build                    # build build/haris.img
cmake --build build --target run       # boot in QEMU
```

## Run

```bash
qemu-system-x86_64 -drive format=raw,file=build/haris.img
```

## Resources

- OSDev Wiki: https://wiki.osdev.org