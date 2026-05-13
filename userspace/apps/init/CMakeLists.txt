# Init binary — the first userspace process spawned by the kernel.

set(INIT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LIBC_OUT_DIR ${CMAKE_BINARY_DIR}/libc)
set(INIT_OUT_DIR ${CMAKE_BINARY_DIR}/init)
set(INIT_BINARY ${INIT_OUT_DIR}/init.elf)

file(MAKE_DIRECTORY ${INIT_OUT_DIR})

set(INIT_CC ${CMAKE_C_COMPILER})
set(INIT_TARGET --target=x86_64-unknown-none)
set(INIT_CFLAGS ${INIT_TARGET} -ffreestanding -fno-builtin -nostdlibinc -I${PROJECT_SOURCE_DIR}/libc/include -I${PROJECT_SOURCE_DIR}/shared/include -std=c17)
set(INIT_LDFLAGS ${INIT_TARGET} -ffreestanding -nostdlib -static -Wl,-z,max-page-size=0x1000 -Wl,-e_start)

set(CRT0_OBJ ${LIBC_OUT_DIR}/crt0.o)
set(INIT_MAIN_OBJ ${INIT_OUT_DIR}/main.o)

add_custom_command(
    OUTPUT ${INIT_MAIN_OBJ}
    COMMAND ${INIT_CC} ${INIT_CFLAGS} -c ${INIT_SRC_DIR}/main.c -o ${INIT_MAIN_OBJ}
    DEPENDS ${INIT_SRC_DIR}/main.c
    COMMENT "Building init main.o"
)

add_custom_command(
    OUTPUT ${INIT_BINARY}
    COMMAND ${INIT_CC} ${INIT_LDFLAGS} -o ${INIT_BINARY} ${CRT0_OBJ} ${INIT_MAIN_OBJ} -L${LIBC_OUT_DIR} -ltori_libc
    DEPENDS ${INIT_MAIN_OBJ} ${CRT0_OBJ}
    COMMENT "Linking init.elf"
)

add_custom_target(tori_init ALL DEPENDS ${INIT_BINARY})
