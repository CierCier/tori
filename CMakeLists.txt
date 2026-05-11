cmake_minimum_required(VERSION 3.25)

project(Tori LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(shared)
add_subdirectory(kernel)
add_subdirectory(libc)
add_subdirectory(userspace)

add_custom_target(run
    DEPENDS tori_run_qemu
)

add_custom_target(iso
    DEPENDS tori_iso
)

add_custom_target(kernel-image
    DEPENDS tori_kernel
)
