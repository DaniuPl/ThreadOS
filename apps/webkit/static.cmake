# Run at the end of WebKit's project() (CMAKE_PROJECT_INCLUDE)
#
# WebKit's CMake makes its top library, WebKit itself, a shared one,
# and every port but the ones that ask otherwise inherits that
# (Source/cmake/WebKitCommon.cmake). ThreadOS has no dynamic loader:
# its programs are static. CMake turns add_library(SHARED) into a
# static library on a platform that says it has no shared ones, and
# project() is where the platform's own files have just said that it
# does - so it is said here, after them, and before WebKit's files ask.
set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS FALSE)

# ThreadOS's own programs on top of WebKit (apps/webkit/programs.cmake)
# need WebKit's targets to exist, so they are added once WebKit's top
# CMakeLists.txt has been read to its end. (Every project() runs this
# file; the programs are added once.)
get_property(_threados_deferred GLOBAL PROPERTY THREADOS_PROGRAMS_DEFERRED)
if (THREADOS_PROGRAMS AND NOT _threados_deferred)
    set_property(GLOBAL PROPERTY THREADOS_PROGRAMS_DEFERRED TRUE)
    cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR}
                   CALL include ${THREADOS_PROGRAMS})
endif ()
