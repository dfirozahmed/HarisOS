/* HarisOS CPU exception report text: see include/fault.h. Pure string building, no hardware access. */
#include "fault.h"

typedef struct
{
    char *p;
    int cap, n;
} Out;

static void put(Out *o, char c)
{
    if (o->n + 1 < o->cap)
        o->p[o->n++] = c;
}

static void puts_(Out *o, const char *s)
{
    while (*s)
        put(o, *s++);
}

static void hex(Out *o, unsigned long long v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        put(o, "0123456789ABCDEF"[(v >> (i * 4)) & 0xF]);
}

/* "NAME=0123456789ABCDEF" */
static void reg(Out *o, const char *name, unsigned long long v)
{
    puts_(o, name);
    put(o, '=');
    hex(o, v, 16);
}

static const char *const NAMES[32] = {
    "#DE DIVIDE ERROR", "#DB DEBUG", "NMI", "#BP BREAKPOINT", "#OF OVERFLOW", "#BR BOUND RANGE EXCEEDED",
    "#UD INVALID OPCODE", "#NM DEVICE NOT AVAILABLE", "#DF DOUBLE FAULT", "COPROCESSOR SEGMENT OVERRUN",
    "#TS INVALID TSS", "#NP SEGMENT NOT PRESENT", "#SS STACK FAULT", "#GP GENERAL PROTECTION", "#PF PAGE FAULT",
    "RESERVED", "#MF X87 FLOATING POINT", "#AC ALIGNMENT CHECK", "#MC MACHINE CHECK", "#XM SIMD FLOATING POINT",
    "#VE VIRTUALIZATION", "#CP CONTROL PROTECTION", "RESERVED", "RESERVED", "RESERVED", "RESERVED", "RESERVED",
    "RESERVED", "#HV HYPERVISOR INJECTION", "#VC VMM COMMUNICATION", "#SX SECURITY", "RESERVED"};

const char *fault_name(unsigned vector) { return vector < 32 ? NAMES[vector] : "INTERRUPT"; }

int fault_has_error_code(unsigned vector) { return vector < 32 && ((FAULT_ERROR_CODE_MASK >> vector) & 1u); }

int fault_format(char *buf, int cap, const FaultFrame *f, unsigned long long cr2)
{
    Out o = {buf, cap, 0};
    if (cap <= 0)
        return 0;

    puts_(&o, "HARISOS: CPU EXCEPTION\n");
    puts_(&o, fault_name((unsigned)f->vector));
    puts_(&o, "  VEC=");
    hex(&o, f->vector, 2);
    if (fault_has_error_code((unsigned)f->vector))
    {
        puts_(&o, "  ERR=");
        hex(&o, f->error, 16);
    }
    put(&o, '\n');

    if (f->vector == 14) /* page fault: the error code says what the access was */
    {
        puts_(&o, f->error & 8 ? "RESERVED BIT SET, " : (f->error & 1 ? "PROTECTION VIOLATION, " : "NOT PRESENT, "));
        puts_(&o, f->error & 16 ? "INSTRUCTION FETCH, " : (f->error & 2 ? "WRITE, " : "READ, "));
        puts_(&o, f->error & 4 ? "USER MODE\n" : "KERNEL MODE\n");
    }

    reg(&o, "RIP", f->rip);
    puts_(&o, "  CS=");
    hex(&o, f->cs, 4);
    puts_(&o, "  ");
    reg(&o, "RFLAGS", f->rflags);
    put(&o, '\n');
    reg(&o, "RSP", f->rsp);
    puts_(&o, "  SS=");
    hex(&o, f->ss, 4);
    put(&o, '\n');
    reg(&o, "CR2", cr2);
    puts_(&o, "\n\n");

    static const char *const NAMES_GPR[15] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "R8", "R9", "R10", "R11", "R12", "R13", "R14", "R15"};
    const unsigned long long vals[15] = {f->rax, f->rbx, f->rcx, f->rdx, f->rsi, f->rdi, f->rbp, f->r8,
                                         f->r9, f->r10, f->r11, f->r12, f->r13, f->r14, f->r15};
    for (int i = 0; i < 15; i++)
    {
        reg(&o, NAMES_GPR[i], vals[i]);
        put(&o, i % 2 == 1 || i == 14 ? '\n' : ' ');
    }

    o.p[o.n] = 0;
    return o.n;
}
