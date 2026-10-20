cmake_minimum_required(VERSION 3.16)
project(HarisOS_UEFI NONE)

# ------------------------------------------------------------
#   Tools: clang (compiler) + lld-link (makes the PE/EFI file)
#   Windows:  winget install LLVM.LLVM   (then add C:\Program Files\LLVM\bin to PATH)
#   Linux:    sudo apt install clang lld
# ------------------------------------------------------------
find_program(CLANG_CC clang)
find_program(LLD_LINK lld-link)

if(NOT CLANG_CC OR NOT LLD_LINK)
    message(FATAL_ERROR
        "clang and/or lld-link not found.\n"
        "Windows: winget install LLVM.LLVM and add C:\\Program Files\\LLVM\\bin to PATH.\n"
        "Linux:   sudo apt install clang lld")
endif()

find_program(QEMU_EXECUTABLE NAMES qemu-system-x86_64)

# ------------------------------------------------------------
#   Build two separate programs (both are PE32+ "efi_application" files):
#   boot/uefi/uefi_loader.c  ->  BOOTX64.EFI   (started by the firmware)
#   kernel/kernel.c          ->  KERNEL.EFI     (loaded + started by the loader)
# ------------------------------------------------------------
set(EFI_CFLAGS
    --target=x86_64-pc-win32-coff       
    -ffreestanding
    -fshort-wchar                       # wide strings L"..." are 16-bit (UTF-16)
    -mno-red-zone
    -I${CMAKE_SOURCE_DIR}/include
    -O2 -Wall -Wextra -std=gnu11)

# helper: haris_efi_program
function(haris_efi_program NAME SRC ENTRY OUTNAME)
    set(OBJ ${CMAKE_BINARY_DIR}/${NAME}.obj)
    set(BIN ${CMAKE_BINARY_DIR}/${OUTNAME})
    add_custom_command(
        OUTPUT  ${OBJ}
        COMMAND ${CLANG_CC} ${EFI_CFLAGS} -c ${SRC} -o ${OBJ}
        DEPENDS ${SRC} ${CMAKE_SOURCE_DIR}/include/uefi.h
        COMMENT "Compiling ${NAME}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${BIN}
        COMMAND ${LLD_LINK} -subsystem:efi_application -entry:${ENTRY} -machine:x64
                -nodefaultlib -out:${BIN} ${OBJ}
        DEPENDS ${OBJ}
        COMMENT "Linking ${OUTNAME}"
        VERBATIM
    )
endfunction()

haris_efi_program(uefi_loader ${CMAKE_SOURCE_DIR}/boot/uefi/uefi_loader.c efi_main    BOOTX64.EFI)
# Kernel image = UEFI entry wrapper + the ordinary (UEFI-independent) C kernel.
# To add a source file, just add it to this list - nothing else to edit.
set(KERNEL_SOURCES
    boot/efi_entry.c            # UEFI entry: collects BootInfo, calls kernel_main
    kernel/kernel.c
    kernel/memory/pmm.c
    kernel/core/power.c              # shut down / restart (firmware reset service + emulator fallbacks)
    kernel/core/acpi.c               # ACPI tables (DSDT / SSDT byte code)
    kernel/core/pci.c                # PCI device enumeration (Device Manager)
    kernel/core/devmgr.c             # Device Manager list (PCI + input devices)
    kernel/storage/blk.c             # block devices: sector I/O and the write lock
    kernel/storage/part.c            # partition tables (GPT, MBR)
    kernel/storage/ahci.c            # SATA SSD / hard disk driver (AHCI)
    kernel/storage/nvme.c            # NVMe SSD driver
    kernel/storage/storage.c         # starts the drivers
    kernel/fs/fat32.c                # FAT32 file system
    kernel/fs/volume.c               # puts FAT32 volumes into the virtual file system
    kernel/core/devmgr_sys.c         # Device Manager glue: asks PCI and the drivers
    kernel/fs/vfs.c                  # in-memory file system (File Explorer)
    kernel/time/rtc.c
    kernel/time/timer.c              # milliseconds since boot (PIT + rdtsc), for double-click
    kernel/graphics/gfx_clip.c  # clipping state and safe clip-box intersection
    kernel/graphics/gfx_buffer.c # backbuffer, presentation and background cache
    kernel/graphics/gfx_dirty.c  # dirty-region tracking
    kernel/graphics/gfx_backend.c # framebuffer initialization and backend properties
    kernel/graphics/gfx_shapes.c  # anti-aliased rounded rectangles and circles
    kernel/graphics/gfx_effects.c # gradients, shadows and glow effects
    kernel/graphics/gfx_cursor.c # software cursor save/restore and rendering
    kernel/graphics/gfx_lines.c  # anti-aliased lines and convex polygons
    kernel/graphics/gfx.c       # basic drawing primitives and alpha blending
    kernel/graphics/font.c      # smooth text rendering
    kernel/graphics/font_data.c # Poppins
    kernel/graphics/cursor_data.c # mouse arrow
    kernel/input/ps2.c          # PS/2 controller: sorts keyboard / mouse bytes
    kernel/input/usb_xhci.c     # USB 3 (xHCI) controller + HID boot mouse (polled)
    kernel/input/aml_scan.c     # finds I2C-HID devices in ACPI byte code
    kernel/input/hid_desc.c     # HID report descriptor: where the mouse fields are
    kernel/input/i2c_dw.c       # DesignWare I2C controller (AMD / Intel)
    kernel/input/i2c_hid.c      # I2C-HID touchpad (polled)
    kernel/input/mouse.c        # mouse driver: PS/2 and USB (polled)
    kernel/input/keyboard.c     # PS/2 keyboard driver (polled)
    kernel/ui/ui_theme.c       # theme colors and active theme
    kernel/ui/ui_layout.c      # rectangle layout and clipping helpers
    kernel/ui/ui_text.c        # text styles and alignment
    kernel/ui/ui_surfaces.c    # wallpaper, cards and window surfaces
    kernel/ui/ui_controls.c    # buttons and common controls
    kernel/ui/ui_edit.c        # text input and blinking caret
    kernel/ui/icons.c          # HarisUI: vector icons
    kernel/window/wm.c             # window manager: lifecycle and state
    kernel/window/wm_input.c       # window-manager mouse and keyboard input
    kernel/window/wm_damage.c      # window-manager damage tracking
    kernel/window/wm_render.c      # window rendering and clipping
    kernel/desktop/desktop.c
    kernel/desktop/desktop_input.c
    kernel/desktop/desktop_render.c
    kernel/desktop/desktop_clock.c
    kernel/desktop/desktop_windows.c
    kernel/desktop/desktop_power.c  # shut down / restart screen
    kernel/desktop/appmenu.c        # taskbar app right-click menu
    kernel/desktop/startmenu.c      # start menu (light translucent panel)
    kernel/desktop/taskbar.c
    apps/app_registry.c             # list of installed apps (start menu reads it)
    apps/explorer/explorer_app.c      # File Explorer: AppInfo, open / close window
    apps/explorer/explorer_state.c    # File Explorer: shared state
    apps/explorer/explorer_util.c     # File Explorer: file kinds, size text
    apps/explorer/explorer_nav.c      # File Explorer: folder, history, sort, layout, selection
    apps/explorer/explorer_ops.c      # File Explorer: rename, new, clipboard, delete
    apps/explorer/explorer_drive.c    # File Explorer: drives, lock, error messages
    apps/explorer/explorer_menu.c     # File Explorer: right-click and sort menus
    apps/explorer/explorer_toolbar.c  # File Explorer: toolbar buttons and tooltips
    apps/explorer/explorer_draw.c     # File Explorer: sidebar, list, status bar
    apps/explorer/explorer_input.c    # File Explorer: mouse and keyboard
    apps/devmgr/devmgr_app.c)       # Device Manager app

file(GLOB HARIS_HEADERS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/include/*.h ${CMAKE_SOURCE_DIR}/apps/explorer/*.h)
set(KERNEL_BIN ${CMAKE_BINARY_DIR}/KERNEL.EFI)
set(KERNEL_OBJS "")
foreach(SRC ${KERNEL_SOURCES})
    get_filename_component(OBJ_NAME ${SRC} NAME_WE)
    set(OBJ ${CMAKE_BINARY_DIR}/${OBJ_NAME}.obj)
    add_custom_command(
        OUTPUT  ${OBJ}
        COMMAND ${CLANG_CC} ${EFI_CFLAGS} -c ${CMAKE_SOURCE_DIR}/${SRC} -o ${OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/${SRC} ${HARIS_HEADERS}
        COMMENT "Compiling ${SRC}"
        VERBATIM
    )
    list(APPEND KERNEL_OBJS ${OBJ})
endforeach()

add_custom_command(
    OUTPUT ${KERNEL_BIN}
    COMMAND ${LLD_LINK} -subsystem:efi_application -entry:efi_kernel_entry -machine:x64
            -nodefaultlib -out:${KERNEL_BIN} ${KERNEL_OBJS}
    DEPENDS ${KERNEL_OBJS}
    COMMENT "Linking KERNEL.EFI"
    VERBATIM
)

# ------------------------------------------------------------
# "EFI System Partition" folder: firmware boots EFI/BOOT/BOOTX64.EFI by default.
# KERNEL.EFI goes in the root of the same disk (the loader opens \KERNEL.EFI).
# QEMU can present a plain folder as a FAT disk (fat:rw:esp), so no disk image is needed.
# ------------------------------------------------------------
set(ESP_LOADER ${CMAKE_BINARY_DIR}/esp/EFI/BOOT/BOOTX64.EFI)
set(ESP_KERNEL ${CMAKE_BINARY_DIR}/esp/KERNEL.EFI)

add_custom_command(
    OUTPUT  ${ESP_LOADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/esp/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/BOOTX64.EFI ${ESP_LOADER}
    DEPENDS ${CMAKE_BINARY_DIR}/BOOTX64.EFI
    COMMENT "Copying to esp/EFI/BOOT/BOOTX64.EFI"
    VERBATIM
)

add_custom_command(
    OUTPUT  ${ESP_KERNEL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/esp
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/KERNEL.EFI ${ESP_KERNEL}
    DEPENDS ${CMAKE_BINARY_DIR}/KERNEL.EFI
    COMMENT "Copying to esp/KERNEL.EFI"
    VERBATIM
)

# ------------------------------------------------------------
# Bootable disk image for a USB stick / virtual machine: build/haris.img
# (GPT + FAT32 EFI System Partition). tools/mkimg.c is a small host program,
# ------------------------------------------------------------
set(MKIMG_EXE ${CMAKE_BINARY_DIR}/mkimg${CMAKE_HOST_EXECUTABLE_SUFFIX})
set(HARIS_IMG ${CMAKE_BINARY_DIR}/haris.img)

add_custom_command(
    OUTPUT  ${MKIMG_EXE}
    COMMAND ${CLANG_CC} -O2 -o ${MKIMG_EXE} ${CMAKE_SOURCE_DIR}/tools/mkimg.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/mkimg.c
    COMMENT "Building mkimg"
    VERBATIM
)

add_custom_command(
    OUTPUT  ${HARIS_IMG}
    COMMAND ${MKIMG_EXE} ${HARIS_IMG} ${CMAKE_BINARY_DIR}/BOOTX64.EFI ${CMAKE_BINARY_DIR}/KERNEL.EFI
    DEPENDS ${MKIMG_EXE} ${CMAKE_BINARY_DIR}/BOOTX64.EFI ${KERNEL_BIN}
    COMMENT "Creating haris.img"
    VERBATIM
)

add_custom_target(haris ALL DEPENDS ${ESP_LOADER} ${ESP_KERNEL} ${HARIS_IMG})

# ------------------------------------------------------------
# Running needs UEFI firmware for the virtual machine (OVMF / edk2).
# The QEMU installer ships it in its "share" folder; Linux distros have an ovmf package.
# If it is not found automatically, pass it yourself:
#   cmake -S . -B build -DOVMF_CODE="C:/Program Files/qemu/share/edk2-x86_64-code.fd"
# ------------------------------------------------------------
set(OVMF_CODE "" CACHE FILEPATH "UEFI firmware code image (edk2-x86_64-code.fd, OVMF_CODE.fd or OVMF.fd)")

set(FW_CODE "${OVMF_CODE}")
if(NOT FW_CODE)
    if(QEMU_EXECUTABLE)
        get_filename_component(QEMU_DIR "${QEMU_EXECUTABLE}" DIRECTORY)
    endif()
    find_file(FW_CODE_AUTO
        NAMES edk2-x86_64-code.fd OVMF_CODE_4M.fd OVMF_CODE.fd OVMF.fd
        PATHS "${QEMU_DIR}/share" "${QEMU_DIR}/../share/qemu" "C:/Program Files/qemu/share"
              /usr/share/qemu /usr/share/OVMF /usr/share/ovmf /usr/share/edk2/x64
              /usr/share/edk2/ovmf /usr/local/share/qemu /opt/homebrew/share/qemu
        NO_DEFAULT_PATH)
    if(FW_CODE_AUTO)
        set(FW_CODE "${FW_CODE_AUTO}")
    endif()
endif()

# Firmware "code" images need a matching writable "vars" file (variable storage);
# a combined OVMF.fd does not.
set(FW_MODE "none")
if(FW_CODE)
    get_filename_component(FW_CODE_NAME "${FW_CODE}" NAME)
    get_filename_component(FW_DIR "${FW_CODE}" DIRECTORY)
    set(FW_VARS_NAME "")
    if(FW_CODE_NAME STREQUAL "edk2-x86_64-code.fd")
        set(FW_VARS_NAME "edk2-i386-vars.fd")
    elseif(FW_CODE_NAME STREQUAL "OVMF_CODE_4M.fd")
        set(FW_VARS_NAME "OVMF_VARS_4M.fd")
    elseif(FW_CODE_NAME STREQUAL "OVMF_CODE.fd")
        set(FW_VARS_NAME "OVMF_VARS.fd")
    endif()

    if(FW_VARS_NAME AND EXISTS "${FW_DIR}/${FW_VARS_NAME}")
        set(FW_MODE "pflash")
        configure_file("${FW_DIR}/${FW_VARS_NAME}" "${CMAKE_BINARY_DIR}/ovmf_vars.fd" COPYONLY)
    else()
        set(FW_MODE "bios")
    endif()
    message(STATUS "UEFI firmware: ${FW_CODE} (mode: ${FW_MODE})")
endif()

if(QEMU_EXECUTABLE AND FW_MODE STREQUAL "pflash")
    add_custom_target(run
        COMMAND ${QEMU_EXECUTABLE} -machine q35 -m 256M
                -drive if=pflash,format=raw,unit=0,readonly=on,file=${FW_CODE}
                -drive if=pflash,format=raw,unit=1,file=${CMAKE_BINARY_DIR}/ovmf_vars.fd
                -drive format=raw,file=fat:rw:esp
                -nic none
                -device qemu-xhci -device usb-mouse -device usb-kbd
        DEPENDS haris
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL VERBATIM)
elseif(QEMU_EXECUTABLE AND FW_MODE STREQUAL "bios")
    add_custom_target(run
        COMMAND ${QEMU_EXECUTABLE} -machine q35 -m 256M
                -bios ${FW_CODE}
                -drive format=raw,file=fat:rw:esp
                -nic none
                -device qemu-xhci -device usb-mouse -device usb-kbd
        DEPENDS haris
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL VERBATIM)
else()
    message(WARNING
        "Build works, but the 'run' target is unavailable: QEMU or the UEFI firmware was not found.\n"
        "Pass the firmware with -DOVMF_CODE=<path to edk2-x86_64-code.fd or OVMF.fd>.")
endif()
