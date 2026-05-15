# Libc workspace.
#
# Builds a minimal freestanding C library as a static archive.
# The kernel must not link against this target.

set(LIBC_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LIBC_OUT_DIR ${CMAKE_BINARY_DIR}/libc)
set(LIBC_ARCHIVE ${LIBC_OUT_DIR}/libtori_libc.a)

file(MAKE_DIRECTORY ${LIBC_OUT_DIR})

set(LIBC_CC ${CMAKE_C_COMPILER})

set(LIBC_CFLAGS
    --target=x86_64-unknown-none
    -ffreestanding
    -fno-builtin
    -nostdlibinc
    -I${LIBC_SRC_DIR}/include
    -I${PROJECT_SOURCE_DIR}/shared/include
    -std=gnu17
)

set(LIBC_ASFLAGS
    --target=x86_64-unknown-none
    -ffreestanding
    -I${LIBC_SRC_DIR}/include
)

set(CRT0_OBJ ${LIBC_OUT_DIR}/crt0.o)
set(WRITE_OBJ ${LIBC_OUT_DIR}/write.o)
set(EXIT_OBJ ${LIBC_OUT_DIR}/exit.o)
set(FORK_OBJ ${LIBC_OUT_DIR}/fork.o)
set(EXECV_OBJ ${LIBC_OUT_DIR}/execv.o)
set(GETPID_OBJ ${LIBC_OUT_DIR}/getpid.o)
set(GETPPID_OBJ ${LIBC_OUT_DIR}/getppid.o)
set(WAITPID_OBJ ${LIBC_OUT_DIR}/waitpid.o)

add_custom_command(
    OUTPUT ${CRT0_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_ASFLAGS} -c ${LIBC_SRC_DIR}/src/crt0/_start.S -o ${CRT0_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/crt0/_start.S
    COMMENT "Building crt0.o"
)

add_custom_command(
    OUTPUT ${WRITE_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/write.c -o ${WRITE_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/write.c
    COMMENT "Building write.o"
)

add_custom_command(
    OUTPUT ${EXIT_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/exit.c -o ${EXIT_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/exit.c
    COMMENT "Building exit.o"
)

add_custom_command(
    OUTPUT ${FORK_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/fork.c -o ${FORK_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/fork.c
    COMMENT "Building fork.o"
)

add_custom_command(
    OUTPUT ${EXECV_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/execv.c -o ${EXECV_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/execv.c
    COMMENT "Building execv.o"
)

add_custom_command(
    OUTPUT ${GETPID_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/getpid.c -o ${GETPID_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/getpid.c
    COMMENT "Building getpid.o"
)

add_custom_command(
    OUTPUT ${GETPPID_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/getppid.c -o ${GETPPID_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/getppid.c
    COMMENT "Building getppid.o"
)

add_custom_command(
    OUTPUT ${WAITPID_OBJ}
    COMMAND ${LIBC_CC} ${LIBC_CFLAGS} -c ${LIBC_SRC_DIR}/src/syscall/waitpid.c -o ${WAITPID_OBJ}
    DEPENDS ${LIBC_SRC_DIR}/src/syscall/waitpid.c
    COMMENT "Building waitpid.o"
)

add_custom_command(
    OUTPUT ${LIBC_ARCHIVE}
    COMMAND ${CMAKE_AR} rcs ${LIBC_ARCHIVE} ${WRITE_OBJ} ${EXIT_OBJ} ${FORK_OBJ} ${EXECV_OBJ} ${GETPID_OBJ} ${GETPPID_OBJ} ${WAITPID_OBJ}
    DEPENDS ${WRITE_OBJ} ${EXIT_OBJ} ${FORK_OBJ} ${EXECV_OBJ} ${GETPID_OBJ} ${GETPPID_OBJ} ${WAITPID_OBJ}
    COMMENT "Archiving libtori_libc.a"
)

add_custom_target(tori_libc ALL DEPENDS ${LIBC_ARCHIVE} ${CRT0_OBJ})
