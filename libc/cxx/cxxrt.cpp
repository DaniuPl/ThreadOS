// The C++ runtime's last pieces, for programs on musl (libc/)
//
// C++ on ThreadOS is the host compiler's own libstdc++: its headers,
// and its static library for the few things that are not templates
// (the red-black tree behind std::map, the list node, RTTI for
// dynamic_cast). That library was built for glibc and with exceptions,
// and assumes a few things this system does not have - those are here,
// together with what is better taken from here than from it (operator
// new and std::string, below, whose library copies drag the exception
// runtime in).
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
#include <cwchar>
#include <new>
#include <string>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/random.h>
#include <sys/stat.h>

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

// operator new and delete, on malloc. The library's own versions would
// do as well, but the nothrow ones are built with exceptions on and
// drag the unwinder in behind them; these are the whole family, so the
// linker never reaches for the library's.
void *operator new(std::size_t n)
{
    void *p = std::malloc(n ? n : 1);
    if (!p)
        std::__throw_bad_alloc();
    return p;
}
void *operator new[](std::size_t n) { return operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    return std::malloc(n ? n : 1);
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
    return std::malloc(n ? n : 1);
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { std::free(p); }

namespace std {
const nothrow_t nothrow{};

// std::string, instantiated here rather than taken from the library.
// The library's copy (string-inst.o) was compiled with exceptions, so
// its members catch and rethrow - and linking it pulls in the whole
// exception runtime and the unwinder after it. Instantiated in this
// file, which is built with -fno-exceptions, the same members end the
// program through __throw_* instead, and the library's is never
// needed.
template class __cxx11::basic_string<char>;
}

extern "C" {

// glibc 2.32's "is this process single-threaded", which libstdc++ reads
// to decide whether its reference counts need atomic instructions. musl
// has no such flag, and programs on it do start threads (pthreads,
// libc/overlay/src/thread/x86_64/clone.s), so the answer is always no:
// counts shared between threads must not be torn
char __libc_single_threaded = 0;

void *__dso_handle = &__dso_handle;

[[noreturn]] void __cxa_pure_virtual() { die("pure virtual function called", 0); }
[[noreturn]] void __cxa_deleted_virtual() { die("deleted virtual function called", 0); }

// Never reached: nothing throws (see above). The library's exception
// code names the rest of the unwinder too.
[[noreturn]] void _Unwind_Resume(void *) { die("unwinding", 0); }
[[noreturn]] int _Unwind_RaiseException(void *) { die("unwinding", 0); }
[[noreturn]] int _Unwind_Resume_or_Rethrow(void *) { die("unwinding", 0); }
void _Unwind_DeleteException(void *) {}
unsigned long _Unwind_GetRegionStart(void *) { return 0; }
unsigned long _Unwind_GetTextRelBase(void *) { return 0; }
unsigned long _Unwind_GetDataRelBase(void *) { return 0; }
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
int __fprintf_chk(FILE *f, int, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = std::vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
void *__memcpy_chk(void *d, const void *s, size_t n, size_t)
{
    return std::memcpy(d, s, n);
}
wchar_t *__wmemset_chk(wchar_t *d, wchar_t c, size_t n, size_t)
{
    return std::wmemset(d, c, n);
}
size_t __mbsrtowcs_chk(wchar_t *d, const char **s, size_t n, mbstate_t *ps,
                       size_t)
{
    return std::mbsrtowcs(d, s, n, ps);
}
size_t __mbsnrtowcs_chk(wchar_t *d, const char **s, size_t nms, size_t n,
                        mbstate_t *ps, size_t)
{
    return mbsnrtowcs(d, s, nms, n, ps);
}
// The large-file names: off_t is 64 bits here already (std::fstream's
// members call the descriptor ones)
off_t ftello64(FILE *f) { return ftello(f); }
int fseeko64(FILE *f, off_t off, int whence) { return fseeko(f, off, whence); }
off_t lseek64(int fd, off_t off, int whence) { return lseek(fd, off, whence); }
int fstat64(int fd, struct stat *st) { return fstat(fd, st); }
// openat checked for a missing mode: std::filesystem's directory
// iterators open with it, never with O_CREAT
int __openat_2(int dir, const char *path, int flags)
{
    return openat(dir, path, flags);
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

// Destructors of thread_local objects (WebKit has many). libstdc++ hands
// them to glibc's __cxa_thread_atexit_impl, which runs them when the
// thread ends; musl has none. Here each thread keeps a list, newest
// first so that they run in the reverse of the order they were made, in
// a pthread key whose destructor runs it when the thread ends.
namespace {
struct thread_dtor {
    void (*fn)(void *);
    void *obj;
    thread_dtor *next;
};
pthread_key_t dtor_key;
pthread_once_t dtor_once = PTHREAD_ONCE_INIT;

void run_thread_dtors(void *p)
{
    for (auto *d = static_cast<thread_dtor *>(p); d;) {
        auto *next = d->next;
        d->fn(d->obj);
        std::free(d);
        d = next;
    }
}

void make_dtor_key() { pthread_key_create(&dtor_key, run_thread_dtors); }
}

extern "C" int __cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *)
{
    pthread_once(&dtor_once, make_dtor_key);
    auto *d = static_cast<thread_dtor *>(std::malloc(sizeof(thread_dtor)));
    if (!d)
        return -1;
    d->fn = fn;
    d->obj = obj;
    d->next = static_cast<thread_dtor *>(pthread_getspecific(dtor_key));
    pthread_setspecific(dtor_key, d);
    return 0;
}
