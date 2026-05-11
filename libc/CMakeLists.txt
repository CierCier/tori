# Libc workspace.
#
# This workspace is reserved for the future Tori userspace C library. The
# kernel must not link against this target.

add_library(tori_libc INTERFACE)

target_include_directories(tori_libc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
