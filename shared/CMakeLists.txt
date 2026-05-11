# Shared workspace.
#
# Only ABI-safe declarations that intentionally cross kernel/userspace
# boundaries belong here.

add_library(tori_shared_headers INTERFACE)

target_include_directories(tori_shared_headers INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
