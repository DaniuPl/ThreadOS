// The C++ runtime's last pieces, for programs on musl (libc/)
//
// C++ on ThreadOS is the host compiler's own libstdc++: its headers,
// and its static library for what is not in the headers (the red-black
// tree behind std::map, string hashing, operator new). That library
// was built for glibc, and assumes a few things this system does not
// have - the ones are here.
//
// Exceptions are not among the things C++ gets here: programs are
// built with -fno-exceptions, and wherever the library would throw
// (std::__throw_*, below), it says what went wrong and ends the
// program instead. So the unwinder and its personality routine are
// never needed; the stubs below only satisfy the library members that
// were compiled with exceptions on, and end the program if anything
// ever does reach them.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/random.h>

[[noreturn]] static void die(const char *what, const char *detail)
{
    std::fflush(stdout);
    std::fprintf(stderr, "C++: %s%s%s - ending the program\n", what,
                 detail ? ": " : "", detail ? detail : "");
    std::abort();
}

namespace std {
[[noreturn]] void __throw_bad_alloc() { die("out of memory", 0); }
[[noreturn]] void __throw_bad_array_new_length() { die("bad array length", 0); }
[[noreturn]] void __throw_bad_cast() { die("bad cast", 0); }
[[noreturn]] void __throw_bad_typeid() { die("bad typeid", 0); }
[[noreturn]] void __throw_bad_exception() { die("bad exception", 0); }
[[noreturn]] void __throw_bad_function_call() { die("empty std::function called", 0); }
[[noreturn]] void __throw_logic_error(const char *s) { die("logic error", s); }
[[noreturn]] void __throw_domain_error(const char *s) { die("domain error", s); }
[[noreturn]] void __throw_invalid_argument(const char *s) { die("invalid argument", s); }
[[noreturn]] void __throw_length_error(const char *s) { die("length error", s); }
[[noreturn]] void __throw_out_of_range(const char *s) { die("out of range", s); }
[[noreturn]] void __throw_out_of_range_fmt(const char *s, ...) { die("out of range", s); }
[[noreturn]] void __throw_runtime_error(const char *s) { die("runtime error", s); }
[[noreturn]] void __throw_range_error(const char *s) { die("range error", s); }
[[noreturn]] void __throw_overflow_error(const char *s) { die("overflow", s); }
[[noreturn]] void __throw_underflow_error(const char *s) { die("underflow", s); }
[[noreturn]] void __throw_system_error(int e) { die("system error", std::strerror(e)); }
[[noreturn]] void __throw_bad_variant_access(const char *s) { die("bad variant access", s); }
[[noreturn]] void __throw_bad_variant_access(bool) { die("bad variant access", 0); }
}

extern "C" {

// glibc 2.32's "is this process single-threaded" - which every ThreadOS
// program built against musl is (libc/overlay/src/thread/x86_64/clone.s)
char __libc_single_threaded = 1;

void *__dso_handle = &__dso_handle;

[[noreturn]] void __cxa_pure_virtual() { die("pure virtual function called", 0); }
[[noreturn]] void __cxa_deleted_virtual() { die("deleted virtual function called", 0); }

// Never reached: nothing throws (see above)
[[noreturn]] void _Unwind_Resume(void *) { die("unwinding", 0); }
int __gxx_personality_v0(int, int, unsigned long, void *, void *)
{
    die("unwinding", 0);
}

// glibc's checked and renamed variants, referenced by the library's
// members built with _FORTIFY_SOURCE or for C23
int __sprintf_chk(char *s, int, size_t, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsprintf(s, fmt, ap);
    va_end(ap);
    return n;
}
ssize_t __read_chk(int fd, void *buf, size_t n, size_t)
{
    return read(fd, buf, n);
}
unsigned long __isoc23_strtoul(const char *s, char **end, int base)
{
    return std::strtoul(s, end, base);
}
unsigned int arc4random(void)
{
    unsigned int v = 0;
    getrandom(&v, sizeof(v), 0);
    return v;
}

}
