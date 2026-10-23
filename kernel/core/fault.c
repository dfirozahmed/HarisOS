/* HarisOS CPU exception handling: see include/fault.h. */
#include "fault.h"
#include "serial.h"
#include "task.h"

#if defined(__x86_64__)

/* ---- stubs ---------------------------------------------------
 * One tiny stub per vector, all joining fault_common. Interrupt gates clear IF, so nothing interrupts a stub.
 * Vectors with a CPU error code (FAULT_ERROR_CODE_MASK) get nothing extra; the others push a 0 so that
 * FaultFrame looks the same for all of them.
 *
 * Stack alignment (Microsoft x64 ABI): the CPU aligns RSP to 16 before pushing its frame; frame + error code +
 * vector are 7 qwords, the general registers 15 more, 22 * 8 = 176 = 11 * 16, so RSP is still 16-aligned here.
 * The C function gets its first argument in RCX and 32 bytes of shadow space above the return address. */
#define FAULT_STUB_NOERR(v) \
    ".p2align 4\n"          \
    "fault_stub_" #v ":\n"  \
    "    pushq $0\n"        \
    "    pushq $" #v "\n"   \
    "    jmp fault_common\n"
#define FAULT_STUB_ERR(v)  \
    ".p2align 4\n"         \
    "fault_stub_" #v ":\n" \
    "    pushq $" #v "\n"  \
    "    jmp fault_common\n"

__asm__(".text\n" FAULT_STUB_NOERR(0) FAULT_STUB_NOERR(1) FAULT_STUB_NOERR(2) FAULT_STUB_NOERR(3)
            FAULT_STUB_NOERR(4) FAULT_STUB_NOERR(5) FAULT_STUB_NOERR(6) FAULT_STUB_NOERR(7)
                FAULT_STUB_ERR(8) FAULT_STUB_NOERR(9) FAULT_STUB_ERR(10) FAULT_STUB_ERR(11)
                    FAULT_STUB_ERR(12) FAULT_STUB_ERR(13) FAULT_STUB_ERR(14) FAULT_STUB_NOERR(15)
                        FAULT_STUB_NOERR(16) FAULT_STUB_ERR(17) FAULT_STUB_NOERR(18) FAULT_STUB_NOERR(19)
                            FAULT_STUB_NOERR(20) FAULT_STUB_ERR(21) FAULT_STUB_NOERR(22) FAULT_STUB_NOERR(23)
                                FAULT_STUB_NOERR(24) FAULT_STUB_NOERR(25) FAULT_STUB_NOERR(26) FAULT_STUB_NOERR(27)
                                    FAULT_STUB_NOERR(28) FAULT_STUB_ERR(29) FAULT_STUB_ERR(30) FAULT_STUB_NOERR(31)
        /* push order = reverse of FaultFrame (rax first, r15 last) */
        ".p2align 4\n"
        "fault_common:\n"
        "    pushq %rax\n"
        "    pushq %rcx\n"
        "    pushq %rdx\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %rsi\n"
        "    pushq %rdi\n"
        "    pushq %r8\n"
        "    pushq %r9\n"
        "    pushq %r10\n"
        "    pushq %r11\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, %rcx\n"
        "    cld\n"
        "    subq $32, %rsp\n"
        "    call fault_dispatch\n"
        "1:  hlt\n" /* fault_dispatch never returns */
        "    jmp 1b\n"
        ".data\n"
        ".p2align 3\n"
        ".globl fault_stub_table\n"
        "fault_stub_table:\n"
        "    .quad fault_stub_0, fault_stub_1, fault_stub_2, fault_stub_3\n"
        "    .quad fault_stub_4, fault_stub_5, fault_stub_6, fault_stub_7\n"
        "    .quad fault_stub_8, fault_stub_9, fault_stub_10, fault_stub_11\n"
        "    .quad fault_stub_12, fault_stub_13, fault_stub_14, fault_stub_15\n"
        "    .quad fault_stub_16, fault_stub_17, fault_stub_18, fault_stub_19\n"
        "    .quad fault_stub_20, fault_stub_21, fault_stub_22, fault_stub_23\n"
        "    .quad fault_stub_24, fault_stub_25, fault_stub_26, fault_stub_27\n"
        "    .quad fault_stub_28, fault_stub_29, fault_stub_30, fault_stub_31\n"
        ".text\n");

_Static_assert(sizeof(FaultFrame) == 22 * 8, "FaultFrame must match the pushes in fault_common");

/* ---- TSS: a separate stack for the double fault ---------------
 * #DF often means the kernel stack itself is broken (overflow). The handler would then fault again while
 * pushing its frame and the CPU would reset ("triple fault") without showing anything. IST1 makes the CPU
 * switch to this stack for vector 8 whatever RSP was. */
typedef struct
{
    unsigned int reserved0;
    unsigned long long rsp[3];
    unsigned long long reserved1;
    unsigned long long ist[7];
    unsigned long long reserved2;
    unsigned short reserved3;
    unsigned short iomap_base;
} __attribute__((packed)) Tss;

static unsigned char DF_STACK[4096] __attribute__((aligned(16)));
static unsigned char PF_STACK[4096] __attribute__((aligned(16))); /* IST2: a page fault from a stack overflow */
static Tss TSS;

void fault_tss_descriptor(unsigned long long out[2])
{
    TSS.ist[0] = (unsigned long long)(DF_STACK + sizeof(DF_STACK));
    TSS.ist[1] = (unsigned long long)(PF_STACK + sizeof(PF_STACK));
    TSS.iomap_base = sizeof(Tss); /* no I/O permission bitmap */

    unsigned long long base = (unsigned long long)&TSS;
    unsigned long long limit = sizeof(Tss) - 1;
    out[0] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) /* present, available 64-bit TSS */ |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    out[1] = base >> 32;
}

/* ---- output --------------------------------------------------- */
/* 5x7 glyphs, upper case only (the report is upper case). One row per byte, bit 4 = leftmost pixel. */
static const char GLYPH_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#=:-./()_";
static const unsigned char GLYPHS[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E},
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E},
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C},
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11},
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04},
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x0A, 0x0A, 0x1F, 0x0A, 0x1F, 0x0A, 0x0A}, {0x00, 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00},
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C}, {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10},
    {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}, {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1F}};

enum { SCALE = 2, CELL_W = 6 * SCALE, CELL_H = 9 * SCALE, MARGIN = 16 };

static volatile unsigned *FB;
static unsigned FB_W, FB_H, FB_PITCH, RED_MODE;

static unsigned rgb(unsigned r, unsigned g, unsigned b) { return RED_MODE ? (r << 16) | (g << 8) | b : (b << 16) | (g << 8) | r; }

void fault_init(void *framebuffer, unsigned width, unsigned height, unsigned pitch, unsigned red_mask_mode)
{
    FB = (volatile unsigned *)framebuffer;
    FB_W = width;
    FB_H = height;
    FB_PITCH = pitch;
    RED_MODE = red_mask_mode;
}

/* Straight into video memory: the normal graphics path (back buffer, clipping, dirty regions) may be what broke. */
static void screen_char(int x, int y, char c, unsigned color)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    const unsigned char *g = 0;
    for (int i = 0; GLYPH_CHARS[i]; i++)
        if (GLYPH_CHARS[i] == c)
            g = GLYPHS[i];
    if (!g)
        return; /* space and anything unknown stay blank */
    for (int row = 0; row < 7; row++)
        for (int col = 0; col < 5; col++)
            if (g[row] & (0x10 >> col))
                for (int dy = 0; dy < SCALE; dy++)
                    for (int dx = 0; dx < SCALE; dx++)
                    {
                        int px = x + col * SCALE + dx, py = y + row * SCALE + dy;
                        if (px < (int)FB_W && py < (int)FB_H)
                            FB[(unsigned long long)py * FB_PITCH + px] = color;
                    }
}

static void screen_text(const char *s)
{
    for (unsigned long long i = 0; i < (unsigned long long)FB_H * FB_PITCH; i++)
        FB[i] = rgb(0x00, 0x2B, 0x7F);

    int x = MARGIN, y = MARGIN;
    for (; *s; s++)
    {
        if (*s == '\n')
        {
            x = MARGIN;
            y += CELL_H;
        }
        else
        {
            screen_char(x, y, *s, rgb(0xFF, 0xFF, 0xFF));
            x += CELL_W;
        }
    }
}

static volatile int IN_FAULT;

__attribute__((noreturn)) void fault_dispatch(FaultFrame *f)
{
    if (IN_FAULT) /* the report itself faulted (bad framebuffer?): stop, do not loop */
        for (;;)
            __asm__ volatile("cli; hlt");
    IN_FAULT = 1;

    unsigned long long cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    static char text[1024];
    int len = fault_format(text, sizeof(text), f, cr2);

    /* A page fault inside a thread's guard page is a stack overflow: say so and whose it is. */
    const char *owner = f->vector == 14 ? task_guard_hit(cr2) : 0;
    if (owner)
    {
        static const char NOTE[] = "\nSTACK OVERFLOW IN THREAD: ";
        int n = len;
        for (int i = 0; NOTE[i] && n < (int)sizeof(text) - 2; i++)
            text[n++] = NOTE[i];
        for (int i = 0; owner[i] && n < (int)sizeof(text) - 2; i++)
            text[n++] = owner[i];
        text[n++] = '\n';
        text[n] = 0;
    }

    serial_init();
    serial_puts(text); /* first: the serial port is the most robust output */
    if (FB)
        screen_text(text);

    for (;;)
        __asm__ volatile("cli; hlt");
}

#else

void fault_init(void *framebuffer, unsigned width, unsigned height, unsigned pitch, unsigned red_mask_mode)
{
    (void)framebuffer;
    (void)width;
    (void)height;
    (void)pitch;
    (void)red_mask_mode;
}

#endif
