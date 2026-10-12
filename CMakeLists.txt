cmake_minimum_required(VERSION 3.16)
project(HarisOS NONE)

find_program(NASM_EXECUTABLE nasm)
if(NOT NASM_EXECUTABLE)
    message(FATAL_ERROR "NASM not found. Install it and make sure it is in PATH.")
endif()

find_program(QEMU_EXECUTABLE NAMES qemu-system-x86_64 qemu-system-i386)

set(IMAGE ${CMAKE_BINARY_DIR}/haris.img)

add_custom_command(
    OUTPUT  ${IMAGE}
    COMMAND ${NASM_EXECUTABLE} -f bin ${CMAKE_SOURCE_DIR}/boot/boot.asm -o ${IMAGE}
    DEPENDS ${CMAKE_SOURCE_DIR}/boot/boot.asm
    COMMENT "Assembling boot.asm -> haris.img"
    VERBATIM
)
add_custom_target(haris ALL DEPENDS ${IMAGE})

if(QEMU_EXECUTABLE)
    add_custom_target(run
        COMMAND ${QEMU_EXECUTABLE} -drive format=raw,file=${IMAGE}
        DEPENDS haris
        USES_TERMINAL
    )
endif()
