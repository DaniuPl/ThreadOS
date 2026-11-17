# ThreadOS's programs on WebKit (apps/webkit/programs.cmake)
#
# Read at the end of WebKit's own top CMakeLists.txt (deferred there by
# apps/webkit/static.cmake), when its targets exist. The programs are
# built by WebKit's own macros, as its WPEWebProcess and MiniBrowser
# are, so they link what those link:
#
#   webview     the UI process: a window (apps/webkit/webview.c and
#               the ThreadOS platform, wpethreados.c)
#   wpewebproc  the web process, WebKit's own entry point
#   wpenetproc  the network process, with HTTPS registered first
#
# ThreadOS starts programs by names of at most 15 characters, which
# is why they are not WPEWebProcess and WPENetworkProcess
# (apps/webkit/patches/process-names.patch).

set(_threados_dir ${THREADOS_REPO}/apps/webkit)

foreach (_threados_program IN ITEMS wpewebproc wpenetproc)
    set(${_threados_program}_SOURCES ${_threados_dir}/${_threados_program}.cpp)
    set(${_threados_program}_LIBRARIES WebKit)
    set(${_threados_program}_PRIVATE_INCLUDE_DIRECTORIES
        $<TARGET_PROPERTY:WebKit,INCLUDE_DIRECTORIES>)
    WEBKIT_EXECUTABLE_DECLARE(${_threados_program})
    WEBKIT_EXECUTABLE(${_threados_program})
endforeach ()

set(webview_SOURCES
    ${_threados_dir}/webview.c
    ${_threados_dir}/wpethreados.c
)
set(webview_PRIVATE_INCLUDE_DIRECTORIES
    ${CMAKE_BINARY_DIR}
    ${DERIVED_SOURCES_WPE_API_DIR}
    ${FORWARDING_HEADERS_WPE_DIR}
    ${JavaScriptCoreGLib_DERIVED_SOURCES_DIR}
    ${JavaScriptCoreGLib_FRAMEWORK_HEADERS_DIR}
    ${WPEPlatform_DERIVED_SOURCES_DIR}
    ${WEBKIT_DIR}/WPEPlatform
    ${THREADOS_REPO}/user
    ${THREADOS_REPO}/user64
)
set(webview_PRIVATE_LIBRARIES
    ${JavaScriptCore_LIBRARY_NAME}
    WebKit::WebKit
)
WEBKIT_EXECUTABLE_DECLARE(webview)
WEBKIT_EXECUTABLE(webview)
