# ThreadOS's programs on WebKit (apps/webkit/programs.cmake)
#
# Read at the end of WebKit's own top CMakeLists.txt (deferred there by
# apps/webkit/static.cmake), when its targets exist. The programs are
# built by WebKit's own macros, as its WPEWebProcess and MiniBrowser
# are, so they link what those link:
#
#   weft        the UI process: a window (apps/webkit/weft.c and
#               the ThreadOS platform, wpethreados.c)
#   wpewebproc  the web process, WebKit's own entry point
#   wpenetproc  the network process, with HTTPS registered first
#
# ThreadOS starts programs by names of at most 15 characters, which
# is why they are not WPEWebProcess and WPENetworkProcess
# (apps/webkit/patches/process-names.patch).

set(_threados_dir ${THREADOS_REPO}/apps/webkit)

# WebKit's resources - the web process's theme for form controls
# (wpe-theme.css), the media controls' icons, the picture of a missing
# image, Web Audio's spatial sound - are GResource bundles that register
# themselves from a constructor. Nothing calls into them, so a static
# link leaves them out, and pages went without: a video's controls had
# no icons. Each is asked for by name (-u), which brings its object in.
set(_threados_resources
    "LINKER:-u,WebKitResourcesGResourceBundle_get_resource"
    "LINKER:-u,ModernMediaControlsGResourceBundle_get_resource")

foreach (_threados_program IN ITEMS wpewebproc wpenetproc)
    set(${_threados_program}_SOURCES ${_threados_dir}/${_threados_program}.cpp)
    set(${_threados_program}_LIBRARIES WebKit)
    set(${_threados_program}_PRIVATE_INCLUDE_DIRECTORIES
        $<TARGET_PROPERTY:WebKit,INCLUDE_DIRECTORIES>)
    WEBKIT_EXECUTABLE_DECLARE(${_threados_program})
    WEBKIT_EXECUTABLE(${_threados_program})
    target_link_options(${_threados_program} PRIVATE ${_threados_resources})
endforeach ()

set(weft_SOURCES
    ${_threados_dir}/weft.c
    ${_threados_dir}/wpethreados.c
)
set(weft_PRIVATE_INCLUDE_DIRECTORIES
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
set(weft_PRIVATE_LIBRARIES
    ${JavaScriptCore_LIBRARY_NAME}
    WebKit::WebKit
)
WEBKIT_EXECUTABLE_DECLARE(weft)
WEBKIT_EXECUTABLE(weft)
target_link_options(weft PRIVATE ${_threados_resources})
