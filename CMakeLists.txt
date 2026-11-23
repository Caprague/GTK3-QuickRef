cmake_minimum_required(VERSION 3.16)

project(gtk-quickref VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

execute_process(
    COMMAND dpkg-architecture -qDEB_HOST_MULTIARCH
    OUTPUT_VARIABLE QUICKREF_MULTIARCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

set(QUICKREF_DEPS_PREFIX "" CACHE PATH "Prefix containing local cmark-gfm development files")
if(NOT QUICKREF_DEPS_PREFIX)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.deps/prefix/usr/include/cmark-gfm-core-extensions.h")
        set(QUICKREF_DEPS_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/.deps/prefix")
    endif()
endif()

find_path(
    CMARK_GFM_INCLUDE_DIR
    NAMES cmark-gfm.h
    PATHS
        "${QUICKREF_DEPS_PREFIX}/usr/include"
        "/usr/include"
    NO_DEFAULT_PATH
)
find_file(
    CMARK_GFM_STATIC
    NAMES libcmark-gfm.a
    PATHS
        "${QUICKREF_DEPS_PREFIX}/usr/lib/${QUICKREF_MULTIARCH}"
        "/usr/lib/${QUICKREF_MULTIARCH}"
    NO_DEFAULT_PATH
)
find_file(
    CMARK_GFM_EXTENSIONS_STATIC
    NAMES libcmark-gfm-extensions.a
    PATHS
        "${QUICKREF_DEPS_PREFIX}/usr/lib/${QUICKREF_MULTIARCH}"
        "/usr/lib/${QUICKREF_MULTIARCH}"
    NO_DEFAULT_PATH
)

foreach(required
        CMARK_GFM_INCLUDE_DIR
        CMARK_GFM_STATIC
        CMARK_GFM_EXTENSIONS_STATIC)
    if(NOT ${required})
        message(FATAL_ERROR "缺少构建依赖：${required}。请先运行 ./bootstrap-deps.sh")
    endif()
endforeach()

add_executable(gtk-quickref
    src/main.cpp
    src/markdown_view.cpp
)

target_include_directories(gtk-quickref PRIVATE
    "${CMARK_GFM_INCLUDE_DIR}"
)
target_link_libraries(gtk-quickref PRIVATE
    PkgConfig::GTK3
    "${CMARK_GFM_EXTENSIONS_STATIC}"
    "${CMARK_GFM_STATIC}"
)
target_compile_options(gtk-quickref PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

install(TARGETS gtk-quickref RUNTIME DESTINATION bin)
