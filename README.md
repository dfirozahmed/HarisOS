# HarisOS

HarisOS is a **learning project** where I build a small operating system from scratch, step by step, to understand how computers work at the lowest level.

The goal is to learn: BIOS booting, real mode, protected mode, interrupts, drivers, memory management, and more.

## Requirements

- [NASM](https://www.nasm.us/) (assembler)
- [QEMU](https://www.qemu.org/) (emulator)

## Build

```bash
nasm -f bin boot/boot.asm -o build/boot.bin
```
## Run

```bash
qemu-system-x86_64 -drive format=raw,file=build/boot.bin
```

You should see the welcome message in the QEMU window.

## Resources

- OSDev Wiki: https://wiki.osdev.org