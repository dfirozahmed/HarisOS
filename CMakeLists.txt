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

# Slow boot self tests (the ELF program tests, ~6 s, and the network self test, ~1 s on real hardware). Their result only
# goes to the serial port and a failure is not fatal, so a normal build skips them.
#   cmake -S . -B build -DHARIS_BOOT_SELFTEST=ON
option(HARIS_BOOT_SELFTEST "Run the slow self tests at boot (results go to the serial port)" OFF)
if(HARIS_BOOT_SELFTEST)
    list(APPEND EFI_CFLAGS -DHARIS_BOOT_SELFTEST=1)
endif()

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
    kernel/memory/paging_walk.c     # page table walk: split big pages, map / unmap one page (host-tested)
    kernel/memory/paging.c          # page tables on the real CPU (CR3, invlpg): guard pages
    kernel/memory/aspace.c          # user address spaces: one set of page tables per process, shared kernel half
    kernel/memory/cow.c             # copy-on-write: who holds a shared page (host-tested)
    kernel/memory/mem.c             # memcpy / memmove / memset / memcmp (the compiler calls them; no libc)
    kernel/memory/heap.c          # kernel heap: kmalloc / kfree on top of the page allocator
    kernel/core/fbwc.c               # frame buffer write-combining (PAT) for fast presenting
    kernel/core/fault.c             # CPU exceptions: register-saving stubs, crash screen + serial output, #DF stack
    kernel/core/serial.c            # COM1 output for debugging (boot self test, crash report)
    kernel/core/fault_fmt.c        # CPU exceptions: report text (host-tested)
    kernel/core/task.c             # kernel threads (cooperative): task table, round-robin scheduler, boot self test
    kernel/core/task_ctx.c          # kernel threads: the register-saving context switch
    kernel/core/heapmon.c           # background thread: checks the kernel heap once a second
    kernel/core/syscall.c           # system calls: SYSCALL entry, write / read / open / close / seek / exit / yield
    kernel/core/console.c           # console: character grid, cursor and line editor for a program's window (host-tested)
    kernel/core/pipe.c              # pipes: the byte queue between processes (host-tested)
    kernel/core/shm.c               # shared memory segments (host-tested)
    kernel/core/mq.c                # message queues (host-tested)
    kernel/core/taskset.c           # sets of threads that sleep on an object (host-tested)
    kernel/core/gui.c               # GUI surfaces for user programs: pixels in shared memory, event queues (host-tested)
    kernel/core/sem.c               # semaphores (host-tested)
    kernel/core/mutex.c             # kernel mutex + condition variable (host-tested)
    kernel/core/usync.c             # named mutexes / condition variables for processes (host-tested through test_syscall)
    kernel/core/rwlock.c            # reader-writer lock (host-tested)
    kernel/core/signal.c            # signal handlers: frames on the user stack, sigreturn
    kernel/core/elf.c               # ELF64 executables: header and segment checks (host-tested)
    kernel/core/exec.c              # run a program that is a file on a drive: find, read, load, start
    kernel/core/process.c          # user processes: address space + thread, ring 3 entry, boot self test
    kernel/core/user_demo.c         # ring 3 demo programs for the self test
    kernel/core/protect.c           # SMEP / SMAP, boot self test of the protections and the NMI stack
    kernel/core/gdt.c             # GDT: kernel + user segments, TSS (host-tested layout)
    kernel/core/fpu.c               # x87 / SSE state: per-process FXSAVE images, clean default state
    kernel/core/idle.c              # idle: own GDT/IDT, 1 ms LAPIC tick, hlt between polling passes
    kernel/core/power.c             # shut down / restart (firmware reset service + emulator fallbacks)
    kernel/core/acpi.c               # ACPI tables (DSDT / SSDT byte code, MADT processor list)
    kernel/core/cpuinfo.c            # CPU name, base speed, sockets / cores / logical processors (CPUID)
    kernel/core/smp.c               # SMP stage 1: INIT-SIPI start of the other processors, real mode -> 64-bit trampoline, parked in hlt
    kernel/core/ioapic.c            # I/O APIC: device interrupt lines to processor interrupts (used by the network card)
    kernel/core/pci.c                # PCI device enumeration (Device Manager)
    kernel/core/devmgr.c             # Device Manager list (PCI + input devices)
    kernel/net/nic.c                 # network: picks the card driver, forwards net_send / net_recv ...
    kernel/net/e1000.c               # network: Intel e1000 NIC driver (interrupt or polled), raw Ethernet frames
    kernel/net/rtl8821ce.c           # network: Realtek RTL8821CE Wi-Fi, identification only (stage 1; not a working adapter)
    kernel/net/wpa.c                 # network: WPA2 crypto (SHA-1, HMAC, PBKDF2, AES, the 4-way handshake; host-tested)
    kernel/net/rtl8168.c             # network: Realtek RTL8111/8168 NIC driver (polled; written from the Linux driver, untested on hardware)
    kernel/net/proto.c               # network: checksum, ARP and ICMP echo frames (host-tested)
    kernel/net/sock.c                # network: socket table for the system calls (SYS_SOCKET ...)
    kernel/net/tcp.c                 # network: TCP client (connect / send / receive / close, retransmission)
    kernel/net/net.c                 # network: ARP cache, answering ARP / ping, ping, boot self test
    kernel/storage/blk.c             # block devices: sector I/O and the write lock
    kernel/storage/part.c            # partition tables (GPT, MBR)
    kernel/storage/ahci.c            # SATA SSD / hard disk driver (AHCI)
    kernel/storage/nvme.c            # NVMe SSD driver
    kernel/storage/usb_msc.c         # USB mass storage (pen drives): bulk-only transport + SCSI commands
    kernel/storage/storage.c         # starts the drivers
    kernel/fs/fat32.c                # FAT32 file system
    kernel/fs/exfat.c                # exFAT file system (read only)
    kernel/fs/ntfs.c                 # NTFS file system (read and write)
    kernel/fs/volume.c               # puts FAT32 / exFAT / NTFS volumes into the virtual file system
    kernel/core/devmgr_sys.c         # Device Manager glue: asks PCI and the drivers
    kernel/fs/vfs.c                  # in-memory file system (File Explorer)
    kernel/time/rtc.c
    kernel/time/cpuload.c            # main-loop busy share, for the Task Manager
    kernel/time/timer.c            # milliseconds since boot (PIT + rdtsc), for double-click
    kernel/graphics/gfx_clip.c  # clipping state and safe clip-box intersection
    kernel/graphics/gfx_buffer.c # backbuffer, presentation and background cache
    kernel/graphics/gfx_layer.c  # draw into a separate buffer, paste with transparency (window drag)
    kernel/graphics/gfx_dirty.c # dirty-region tracking
    kernel/graphics/gfx_backend.c # framebuffer initialization and backend properties
    kernel/core/display.c         # display adapters on PCI: Bochs VBE mode setting, framebuffer owner
    kernel/graphics/gfx_shapes.c  # anti-aliased rounded rectangles and circles
    kernel/graphics/gfx_effects.c # gradients, shadows and glow effects
    kernel/graphics/gfx_cursor.c # software cursor save/restore and rendering
    kernel/graphics/gfx_lines.c  # anti-aliased lines and convex polygons
    kernel/graphics/gfx.c       # basic drawing primitives and alpha blending
    kernel/graphics/font.c      # smooth text rendering
    kernel/graphics/font_data.c # Poppins
    kernel/graphics/cursor_data.c # mouse arrow
    kernel/input/ps2.c          # PS/2 controller: sorts keyboard / mouse bytes
    kernel/input/usb_xhci.c     # USB 3 (xHCI) controller + HID mouse / keyboard + mass storage (polled)
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
    kernel/desktop/bootsplash.c     # "Welcome to HarisOS" screen with a progress bar while the kernel starts
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
    apps/devmgr/devmgr_app.c        # Device Manager app
    apps/display/display_app.c      # Display Settings app: brightness slider
    apps/taskmgr/taskmgr_app.c      # Task Manager app: processes and performance
    apps/notepad/notepad_app.c      # Notepad app: plain-text editor
    apps/wifi/wifi_app.c            # Wi-Fi app: pick a wireless network and join it
    apps/console/console_app.c      # Console window: where a program started from the File Explorer prints and reads
    apps/terminal/terminal_app.c    # Terminal app: a console window running the shell; also starts INIT.ELF at boot
    apps/gui/gui_app.c)             # Compositor: windows for the surfaces of user programs (SYS_GUI_*)

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
# User programs (ring 3): user/NAME.c becomes an ELF executable, stored on the boot disk as NAME.ELF.
#
# The toolchain on Windows (MSYS2 clang + lld) has no ELF linker, only PE ones, so the steps are:
#   1. clang compiles user/NAME.c for the bare machine (freestanding, no C library; SSE and x87 are fine: the kernel keeps
#      the floating point registers of every process apart, see include/fpu.h; AVX is not, it is switched off)
#   2. lld-link links it as a PE file at the start of the user address space, sections on 4 KiB boundaries, no relocations
#   3. tools/mkelf.c repackages those sections as an ELF64 file, one PT_LOAD segment per section
# On a machine with an ELF linker the kernel does not care how the file was made; this just works everywhere the
# kernel itself can be built. To add a program: put user/NAME.c here and add NAME to USER_PROGRAMS.
# ------------------------------------------------------------
set(MKELF_EXE ${CMAKE_BINARY_DIR}/mkelf${CMAKE_HOST_EXECUTABLE_SUFFIX})
add_custom_command(
    OUTPUT  ${MKELF_EXE}
    COMMAND ${CLANG_CC} -O2 -o ${MKELF_EXE} ${CMAKE_SOURCE_DIR}/tools/mkelf.c
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/mkelf.c
    COMMENT "Building mkelf"
    VERBATIM
)

set(USER_PROGRAMS hello luatest files echo fpua fpub libctest heaptest argtest crash proctest dirtest ls cat sh init mkdir touch rm cp mv lock upper pipetest kill ps spin sigtest forktest ipctest ipcchild semtest sighand muxtest guidemo stktest wget nettest httpd wifiscan gstest)
set(USER_CFLAGS
    --target=x86_64-pc-win32-coff
    -ffreestanding -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -mno-red-zone
    -O2 -Wall -Wextra -std=gnu11 -I${CMAKE_SOURCE_DIR}/user/include)
# The user C library (user/libc.c, posix.c, libm.c) is compiled once and linked into every program.
set(LIBC_OBJ "")
foreach(LIBSRC libc posix libm numconv)
    set(LIBSRC_OBJ ${CMAKE_BINARY_DIR}/user/${LIBSRC}.obj)
    add_custom_command(
        OUTPUT  ${LIBSRC_OBJ}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
        COMMAND ${CLANG_CC} ${USER_CFLAGS} -c ${CMAKE_SOURCE_DIR}/user/${LIBSRC}.c -o ${LIBSRC_OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/user/${LIBSRC}.c ${CMAKE_SOURCE_DIR}/user/libc.h ${CMAKE_SOURCE_DIR}/user/sys.h
                ${CMAKE_SOURCE_DIR}/user/include/math.h ${CMAKE_SOURCE_DIR}/user/include/setjmp.h
        COMMENT "Compiling user/${LIBSRC}.c"
        VERBATIM
    )
    list(APPEND LIBC_OBJ ${LIBSRC_OBJ})
endforeach()
set(USER_ELFS "")
set(USER_ESP_FILES "")
foreach(PROG ${USER_PROGRAMS})
    string(TOUPPER ${PROG} PROG_UPPER)
    set(PROG_OBJ ${CMAKE_BINARY_DIR}/user/${PROG}.obj)
    set(PROG_PE  ${CMAKE_BINARY_DIR}/user/${PROG}.exe)
    set(PROG_ELF ${CMAKE_BINARY_DIR}/user/${PROG}.elf)
    set(PROG_ESP ${CMAKE_BINARY_DIR}/esp/${PROG_UPPER}.ELF)
    add_custom_command(
        OUTPUT  ${PROG_OBJ}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
        COMMAND ${CLANG_CC} ${USER_CFLAGS} -c ${CMAKE_SOURCE_DIR}/user/${PROG}.c -o ${PROG_OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/user/${PROG}.c ${CMAKE_SOURCE_DIR}/user/sys.h ${CMAKE_SOURCE_DIR}/user/libc.h ${CMAKE_SOURCE_DIR}/user/fsutil.h ${CMAKE_SOURCE_DIR}/user/fputest.h
        COMMENT "Compiling user/${PROG}.c"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${PROG_PE}
        COMMAND ${LLD_LINK} -subsystem:native -entry:_start -nodefaultlib -base:0x400000000000 -fixed -dynamicbase:no
                -driver -align:4096 -filealign:4096 -out:${PROG_PE} ${PROG_OBJ} ${LIBC_OBJ}
        DEPENDS ${PROG_OBJ} ${LIBC_OBJ}
        COMMENT "Linking ${PROG}.exe"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${PROG_ELF}
        COMMAND ${MKELF_EXE} ${PROG_PE} ${PROG_ELF}
        DEPENDS ${MKELF_EXE} ${PROG_PE}
        COMMENT "Making ${PROG}.elf"
        VERBATIM
    )
    add_custom_command(
        OUTPUT  ${PROG_ESP}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/esp
        COMMAND ${CMAKE_COMMAND} -E copy ${PROG_ELF} ${PROG_ESP}
        DEPENDS ${PROG_ELF}
        COMMENT "Copying to esp/${PROG_UPPER}.ELF"
        VERBATIM
    )
    list(APPEND USER_ELFS ${PROG_ELF})
    list(APPEND USER_ESP_FILES ${PROG_ESP})
endforeach()

# ------------------------------------------------------------
# Ported programs: ISO C / POSIX software built from its own sources (third_party/NAME/*.c) against the headers in user/include and the
# C library above, started by user/start.c (they define main, not _start). Result: NAME.ELF on the boot disk like the programs above.
#   lua  - the Lua 5.4 interpreter (third_party/lua): lua.ELF script.lua, or no arguments for the interactive prompt
# The sources are compiled as they are, as an ordinary C program on a POSIX-like system: -U_WIN32 because this target's compiler
# defines it (Lua would then reach for <windows.h>), and -w because the warnings of somebody else's code are not ours to fix.
# ------------------------------------------------------------
set(START_OBJ ${CMAKE_BINARY_DIR}/user/start.obj)
add_custom_command(
    OUTPUT  ${START_OBJ}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/user
    COMMAND ${CLANG_CC} ${USER_CFLAGS} -c ${CMAKE_SOURCE_DIR}/user/start.c -o ${START_OBJ}
    DEPENDS ${CMAKE_SOURCE_DIR}/user/start.c ${CMAKE_SOURCE_DIR}/user/libc.h
    COMMENT "Compiling user/start.c"
    VERBATIM
)
set(PORT_CFLAGS
    --target=x86_64-pc-win32-coff
    -ffreestanding -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -mno-red-zone
    -O2 -w -std=gnu11 -U_WIN32 -U_WIN64 -I${CMAKE_SOURCE_DIR}/user/include)
set(LUA_SOURCES
    lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio
    lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib lstrlib ltablib lutf8lib linit lua)
set(LUA_OBJS "")
foreach(SRC ${LUA_SOURCES})
    set(SRC_OBJ ${CMAKE_BINARY_DIR}/third_party/lua/${SRC}.obj)
    add_custom_command(
        OUTPUT  ${SRC_OBJ}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/third_party/lua
        COMMAND ${CLANG_CC} ${PORT_CFLAGS} -I${CMAKE_SOURCE_DIR}/third_party/lua -c ${CMAKE_SOURCE_DIR}/third_party/lua/${SRC}.c -o ${SRC_OBJ}
        DEPENDS ${CMAKE_SOURCE_DIR}/third_party/lua/${SRC}.c ${CMAKE_SOURCE_DIR}/user/libc.h
        COMMENT "Compiling third_party/lua/${SRC}.c"
        VERBATIM
    )
    list(APPEND LUA_OBJS ${SRC_OBJ})
endforeach()
set(LUA_PE  ${CMAKE_BINARY_DIR}/user/lua.exe)
set(LUA_ELF ${CMAKE_BINARY_DIR}/user/lua.elf)
set(LUA_ESP ${CMAKE_BINARY_DIR}/esp/LUA.ELF)
add_custom_command(
    OUTPUT  ${LUA_PE}
    COMMAND ${LLD_LINK} -subsystem:native -entry:_start -nodefaultlib -base:0x400000000000 -fixed -dynamicbase:no
            -driver -align:4096 -filealign:4096 -out:${LUA_PE} ${LUA_OBJS} ${START_OBJ} ${LIBC_OBJ}
    DEPENDS ${LUA_OBJS} ${START_OBJ} ${LIBC_OBJ}
    COMMENT "Linking lua.exe"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${LUA_ELF}
    COMMAND ${MKELF_EXE} ${LUA_PE} ${LUA_ELF}
    DEPENDS ${MKELF_EXE} ${LUA_PE}
    COMMENT "Making lua.elf"
    VERBATIM
)
add_custom_command(
    OUTPUT  ${LUA_ESP}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/esp
    COMMAND ${CMAKE_COMMAND} -E copy ${LUA_ELF} ${LUA_ESP}
    DEPENDS ${LUA_ELF}
    COMMENT "Copying to esp/LUA.ELF"
    VERBATIM
)
list(APPEND USER_ELFS ${LUA_ELF})
list(APPEND USER_ESP_FILES ${LUA_ESP})

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

# Optional extra files for the boot disk that do not belong in the repository. firmware/RTW8821C.BIN is the firmware of the Realtek
# RTL8821CE Wi-Fi card (linux-firmware rtw88/rtw8821c_fw.bin, Realtek's licence: you fetch it yourself, see kernel/net/rtl8821ce.c).
# The driver looks for a file of that name in the top folder of any drive. The folder is ignored by git.
set(HARIS_EXTRA_FILES "")
if(EXISTS ${CMAKE_SOURCE_DIR}/firmware/RTW8821C.BIN)
    list(APPEND HARIS_EXTRA_FILES ${CMAKE_SOURCE_DIR}/firmware/RTW8821C.BIN)
    message(STATUS "Wi-Fi firmware found: firmware/RTW8821C.BIN goes onto haris.img")
endif()

# pconfig/WIFI.CFG (git ignores the folder): the name and passphrase of the Wi-Fi network to join, two lines "ssid=NAME" and "psk=PASSPHRASE".
# It goes into the top folder of the boot disk; the driver reads it at start-up and never prints the passphrase.
if(EXISTS ${CMAKE_SOURCE_DIR}/pconfig/WIFI.CFG)
    list(APPEND HARIS_EXTRA_FILES ${CMAKE_SOURCE_DIR}/pconfig/WIFI.CFG)
    message(STATUS "Wi-Fi settings found: pconfig/WIFI.CFG goes onto haris.img")
endif()

add_custom_command(
    OUTPUT  ${HARIS_IMG}
    COMMAND ${MKIMG_EXE} ${HARIS_IMG} ${CMAKE_BINARY_DIR}/BOOTX64.EFI ${CMAKE_BINARY_DIR}/KERNEL.EFI ${USER_ELFS} ${HARIS_EXTRA_FILES}
    DEPENDS ${MKIMG_EXE} ${CMAKE_BINARY_DIR}/BOOTX64.EFI ${KERNEL_BIN} ${USER_ELFS} ${HARIS_EXTRA_FILES}
    COMMENT "Creating haris.img"
    VERBATIM
)

add_custom_target(haris ALL DEPENDS ${ESP_LOADER} ${ESP_KERNEL} ${USER_ESP_FILES} ${HARIS_IMG})

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
        COMMAND ${QEMU_EXECUTABLE} -machine q35 -m 256M -smp 4 -accel tcg
                -drive if=pflash,format=raw,unit=0,readonly=on,file=${FW_CODE}
                -drive if=pflash,format=raw,unit=1,file=${CMAKE_BINARY_DIR}/ovmf_vars.fd
                -drive format=raw,file=fat:rw:esp
                -nic user,model=e1000
                -device qemu-xhci -device usb-mouse -device usb-kbd
        DEPENDS haris
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL VERBATIM)
    # Same machine, with the kernel's serial port (COM1: boot log, self tests, crash report) written to build/serial.log.
    # Read it while it runs with `tail -f build/serial.log`; it contains escape codes, so grep needs -a.
    add_custom_target(run-serial
        COMMAND ${QEMU_EXECUTABLE} -machine q35 -m 256M -smp 4 -accel tcg
                -drive if=pflash,format=raw,unit=0,readonly=on,file=${FW_CODE}
                -drive if=pflash,format=raw,unit=1,file=${CMAKE_BINARY_DIR}/ovmf_vars.fd
                -drive format=raw,file=fat:rw:esp
                -serial file:${CMAKE_BINARY_DIR}/serial.log
                -nic user,model=e1000
                -device qemu-xhci -device usb-mouse -device usb-kbd
        DEPENDS haris
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL VERBATIM)
elseif(QEMU_EXECUTABLE AND FW_MODE STREQUAL "bios")
    add_custom_target(run
        COMMAND ${QEMU_EXECUTABLE} -machine q35 -m 256M -smp 4 -accel tcg
                -bios ${FW_CODE}
                -drive format=raw,file=fat:rw:esp
                -nic user,model=e1000
                -device qemu-xhci -device usb-mouse -device usb-kbd
        DEPENDS haris
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL VERBATIM)
    add_custom_target(run-serial
        COMMAND ${QEMU_EXECUTABLE} -machine q35 -m 256M -smp 4 -accel tcg
                -bios ${FW_CODE}
                -drive format=raw,file=fat:rw:esp
                -serial file:${CMAKE_BINARY_DIR}/serial.log
                -nic user,model=e1000
                -device qemu-xhci -device usb-mouse -device usb-kbd
        DEPENDS haris
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        USES_TERMINAL VERBATIM)
else()
    message(WARNING
        "Build works, but the 'run' target is unavailable: QEMU or the UEFI firmware was not found.\n"
        "Pass the firmware with -DOVMF_CODE=<path to edk2-x86_64-code.fd or OVMF.fd>.")
endif()
