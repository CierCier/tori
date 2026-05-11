# Userspace workspace.
#
# Applications, services, and userspace libraries will live here once the
# kernel has a user/kernel boundary.

add_library(tori_userspace_config INTERFACE)

target_include_directories(tori_userspace_config INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/shared/include
)
