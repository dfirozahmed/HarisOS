/* x86 port I/O (inb/outb). Used by the PS/2 drivers. */
#ifndef HARIS_IO_H
#define HARIS_IO_H

#if defined(__x86_64__) || defined(__i386__)
static inline void io_out8(unsigned short port, unsigned char v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned char io_in8(unsigned short port)
{
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void io_out32(unsigned short port, unsigned int v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned int io_in32(unsigned short port)
{
    unsigned int v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
#endif

#endif
