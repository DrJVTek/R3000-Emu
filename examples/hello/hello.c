#include <stdint.h>

// ------------------------------------------------------------
// "Host syscalls" (pour l'émulateur live)
//
// Notre émulateur intercepte l'instruction MIPS SYSCALL et, si v0 contient
// un id spécial, il imprime côté hôte.
//
// Convention:
// - v0 = 0xFF00 : print_u32(a0)
// - v0 = 0xFF02 : putc(a0 & 0xFF)
// - v0 = 0xFF03 : print_cstr(a0)
// ------------------------------------------------------------
#if defined(__mips__) && defined(__GNUC__)

static inline void host_putc(char ch)
{
    register uint32_t v0 __asm__("v0") = 0xFF02u;
    register uint32_t a0 __asm__("a0") = (uint32_t)(uint8_t)ch;
    __asm__ volatile("syscall" : : "r"(v0), "r"(a0) : "memory");
}

static inline void host_print_u32(uint32_t v)
{
    register uint32_t v0 __asm__("v0") = 0xFF00u;
    register uint32_t a0 __asm__("a0") = v;
    __asm__ volatile("syscall" : : "r"(v0), "r"(a0) : "memory");
}

static inline void host_print_cstr(const char* s)
{
    register uint32_t v0 __asm__("v0") = 0xFF03u;
    register const char* a0 __asm__("a0") = s;
    __asm__ volatile("syscall" : : "r"(v0), "r"(a0) : "memory");
}

static void host_print_nl(void)
{
    host_putc('\n');
}

#else
// NOTE:
// Ce fichier est du code "guest" PS1/R3000 et doit être compilé par la toolchain MIPS.
// Les stubs ci-dessous existent uniquement pour éviter des erreurs d'analyse/lint côté IDE
// (où ce .c peut être parsé avec un compilateur host qui ne comprend pas l'asm MIPS).
static inline void host_putc(char ch)
{
    (void)ch;
}

static inline void host_print_u32(uint32_t v)
{
    (void)v;
}

static inline void host_print_cstr(const char* s)
{
    (void)s;
}

static inline void host_print_nl(void)
{
}
#endif

int main(void)
{
    host_print_cstr("HELLO PS1/R3000 (guest) -> host printf via SYSCALL\n");

    for (uint32_t i = 1; i <= 5; ++i)
    {
        host_print_cstr("i=");
        host_print_u32(i);
        host_print_nl();
    }

    // STOP propre: BREAK (l'émulateur le traite comme HALT dans nos démos)
    __asm__ volatile("break");

    for (;;)
    {
    }
}

