# CMake toolchain for WebKit on ThreadOS (apps/webkit/threados.cmake)
#
# ThreadOS programs are built by the host's compilers against musl
# (libc/): musl's headers and the host libstdc++'s in front of the
# host's own, everything static, and musl's start files around the
# objects. WebKit's build is CMake's, so this says the same to it -
# and to LLVM's, whose clang runs on ThreadOS (the Makefile's clang
# rules). The Makefile passes the paths:
#
#   THREADOS_MUSL   the musl install (include/, lib/)
#   THREADOS_REPO   the repository (libc/cxx/compat.h, libc/libc64.ld)
#   THREADOS_CXXRT  the C++ runtime's last pieces (libc/cxx/cxxrt.cpp)
#   THREADOS_LIBS   (WPE only) the libraries built for ThreadOS
#                   (BUILD/libs64), found through their pkg-config files
#   THREADOS_LINK   (WPE only) what those libraries need linked after
#                   them, as `pkg-config --static --libs` says: CMake's
#                   find modules name a library, not the static ones
#                   under it
#   THREADOS_CC, THREADOS_CXX, THREADOS_FLAGS, THREADOS_LD, THREADOS_CPU
#                   the machine (the Makefile's TARGET): the compilers,
#                   their flags, the linker script - the PC's when not
#                   given
#
# The system is Linux as far as WebKit is concerned: musl answers the
# Linux system calls (libc/overlay/), so WebKit's Linux code paths are
# the right ones; what differs is in apps/webkit/patches/.

if (NOT THREADOS_CC)
    set(THREADOS_CC gcc)
    set(THREADOS_CXX g++)
    set(THREADOS_FLAGS "-m64 -march=x86-64 -mtune=generic -mno-red-zone")
    set(THREADOS_LD libc/libc64.ld)
    set(THREADOS_CPU x86_64)
endif ()

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ${THREADOS_CPU})
set(CMAKE_C_COMPILER ${THREADOS_CC})
set(CMAKE_CXX_COMPILER ${THREADOS_CXX})

# The variables reach try_compile() projects too
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    THREADOS_MUSL THREADOS_REPO THREADOS_CXXRT THREADOS_LIBS THREADOS_LINK
    THREADOS_CC THREADOS_CXX THREADOS_FLAGS THREADOS_LD THREADOS_CPU)

execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=include
                OUTPUT_VARIABLE _gcc_include OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-libgcc-file-name
                OUTPUT_VARIABLE _libgcc OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libstdc++.a
                OUTPUT_VARIABLE _libstdcxx OUTPUT_STRIP_TRAILING_WHITESPACE)
# libstdc++'s header directories, in the order the compiler searches them
execute_process(COMMAND sh -c "echo | ${CMAKE_CXX_COMPILER} -xc++ -E -v - 2>&1 | sed -n 's/^ \\(.*c++.*\\)$/-isystem \\1/p' | tr '\\n' ' '"
                OUTPUT_VARIABLE _cxx_includes OUTPUT_STRIP_TRAILING_WHITESPACE)

set(_common "${THREADOS_FLAGS} -fno-stack-protector -fno-pic -fno-plt \
-fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections \
-DTHREADOS_LIBC")
set(CMAKE_C_FLAGS_INIT "${_common} -nostdinc -isystem ${THREADOS_MUSL}/include \
-isystem ${_gcc_include}")
set(CMAKE_CXX_FLAGS_INIT "${_common} -fno-exceptions -nostdinc -nostdinc++ \
${_cxx_includes} -isystem ${THREADOS_MUSL}/include -isystem ${_gcc_include} \
-include ${THREADOS_REPO}/libc/cxx/compat.h")

set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(BUILD_SHARED_LIBS OFF)

# Programs: static, at the address libc64.ld gives them, between musl's
# start files; the C++ runtime, libstdc++ and the C library after them.
# The libraries as one group: static ones are searched in order, and
# CMake lists ICU's data before the code that refers to it
# --allow-multiple-definition: WTF's file system code reaches for
# std::filesystem, which pulls libstdc++'s exception and locale objects
# in, and they bring definitions of their own of what cxxrt.o already
# defines (std::__throw_system_error, the personality routine) - the
# first, cxxrt.o's, is the one wanted.
# -L: a library asked for by name (-lm, -ldl, -latomic) is to be found
# among ThreadOS's - musl's empty ones, libs64's - and never in the
# host's directories, which -nostdlib leaves searched: the host's libm
# is glibc's, with ifuncs a ThreadOS program cannot resolve.
set(_search "-L${THREADOS_MUSL}/lib")
if (THREADOS_LIBS)
    set(_search "${_search} -L${THREADOS_LIBS}/lib")
endif ()
set(_start "-static -no-pie -nostdlib -Wl,--build-id=none \
-Wl,-z,max-page-size=0x1000 -Wl,--gc-sections \
-Wl,--allow-multiple-definition ${_search} \
-T ${THREADOS_REPO}/${THREADOS_LD} \
${THREADOS_MUSL}/lib/crt1.o ${THREADOS_MUSL}/lib/crti.o")
set(_end "${THREADOS_CXXRT} ${_libstdcxx} ${THREADOS_MUSL}/lib/libc.a \
${_libgcc} ${THREADOS_MUSL}/lib/crtn.o")
set(_libs "-Wl,--start-group <LINK_LIBRARIES> ${THREADOS_LINK} -Wl,--end-group")
set(CMAKE_C_LINK_EXECUTABLE
    "<CMAKE_C_COMPILER> <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> ${_start} <OBJECTS> -o <TARGET> <LINK_LIBRARIES> ${THREADOS_MUSL}/lib/libc.a ${_libgcc} ${THREADOS_MUSL}/lib/crtn.o")
set(CMAKE_CXX_LINK_EXECUTABLE
    "<CMAKE_CXX_COMPILER> <FLAGS> <CMAKE_CXX_LINK_FLAGS> <LINK_FLAGS> ${_start} <OBJECTS> -o <TARGET> ${_libs} ${_end}")

set(CMAKE_FIND_ROOT_PATH ${THREADOS_MUSL} ${THREADOS_LIBS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
