cmake_minimum_required(VERSION 3.24)

project(DriveclubNative LANGUAGES C CXX OBJC OBJCXX)

if (NOT APPLE)
    message(FATAL_ERROR "Driveclub Native targets macOS + Metal only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# PS4 code runs natively, so the host must be x86_64 (Intel Mac).
set(CMAKE_OSX_ARCHITECTURES x86_64)

find_package(SDL3 REQUIRED CONFIG)

add_executable(driveclub
    src/main.mm
    src/common/log.h
    src/common/sha1.h
    src/common/types.h
    src/core/address_space.cpp
    src/core/address_space.h
    src/core/loader/elf_types.h
    src/core/loader/linker.cpp
    src/core/loader/linker.h
    src/core/loader/module.cpp
    src/core/loader/module.h
    src/core/loader/self.cpp
    src/core/loader/self.h
    src/core/memory.cpp
    src/core/memory.h
    src/core/runtime.cpp
    src/core/runtime.h
    src/core/signals.cpp
    src/core/signals.h
    src/core/tls.cpp
    src/core/tls.h
    src/gpu/renderer.h
    src/gpu/liverpool/pm4.h
    src/gpu/liverpool/command_processor.h
    src/gpu/liverpool/command_processor.cpp
    src/gpu/liverpool/registers.h
    src/gpu/shader/gcn.h
    src/gpu/shader/gcn_decode.cpp
    src/gpu/shader/program.h
    src/gpu/shader/program.cpp
    src/gpu/shader/msl.h
    src/gpu/shader/msl.cpp
    src/gpu/shader/msl_prelude.h
    src/gpu/metal/mtl_device.h
    src/gpu/metal/mtl_device.mm
    src/gpu/metal/mtl_presenter.h
    src/gpu/metal/mtl_presenter.mm
    src/gpu/metal/mtl_formats.h
    src/gpu/metal/mtl_renderer.h
    src/gpu/metal/mtl_renderer.mm
    src/hle/hle.cpp
    src/hle/hle.h
    src/hle/kernel/kernel.cpp
    src/hle/kernel/kernel.h
    src/hle/kernel/equeue.h
    src/hle/kernel/file.cpp
    src/hle/kernel/memory.cpp
    src/hle/kernel/misc.cpp
    src/hle/kernel/pthread.cpp
    src/hle/kernel/sync.cpp
    src/hle/kernel/time.cpp
    src/hle/video_out/video_out.h
    src/hle/video_out/video_out.cpp
    src/hle/gnmdriver/gnmdriver.h
    src/hle/gnmdriver/gnmdriver.cpp
    src/hle/playgo/playgo.h
    src/hle/playgo/playgo.cpp
    src/hle/user_service/user_service.h
    src/hle/user_service/user_service.cpp
    src/hle/system_service/system_service.h
    src/hle/system_service/system_service.cpp
    src/hle/save_data/save_data.h
    src/hle/save_data/save_data.cpp
    src/hle/pad/pad.h
    src/hle/pad/pad.cpp
    src/hle/rtc/rtc.h
    src/hle/rtc/rtc.cpp
    src/hle/sysmodule/sysmodule.h
    src/hle/sysmodule/sysmodule.cpp
    src/hle/net/net.h
    src/hle/net/net.cpp
    src/hle/np/np.h
    src/hle/np/np.cpp
    src/hle/font/font.h
    src/hle/font/font.cpp
    src/hle/avplayer/avplayer.h
    src/hle/avplayer/avplayer.cpp
    src/hle/services/misc_services.h
    src/hle/services/misc_services.cpp
    src/hle/nid_db.inc
)

target_include_directories(driveclub PRIVATE src)

target_compile_options(driveclub PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<CONFIG:Release>:-O3 -march=native>
)

# Guest memory is mapped at its PS4 addresses (executable at 0x400000), which
# sit inside the default 4 GB __PAGEZERO. Shrink it, move our own image above
# every guest range, and pin the zero-fill reservations from address_space.cpp
# to the guest ranges so the kernel maps them before any library can.
target_link_options(driveclub PRIVATE
    -Wl,-no_pie,-pagezero_size,0x4000,-image_base,0x700000000000
    -Wl,-segaddr,GUEST_TCB,0x4000
    -Wl,-segaddr,GUEST_SYSMGD,0x400000
    -Wl,-segaddr,GUEST_SYSRSV,0x7FFFFC000
    -Wl,-segaddr,GUEST_USER,0x1000000000
)

target_link_libraries(driveclub PRIVATE
    SDL3::SDL3
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
)

# Developer tool: GCN disassembler + MSL translation check (gcn_dis shader.bin [vs|ps] [--msl]).
add_executable(gcn_dis EXCLUDE_FROM_ALL
    src/tools/gcn_dis.mm
    src/gpu/shader/gcn_decode.cpp
    src/gpu/shader/program.cpp
    src/gpu/shader/msl.cpp
)
target_include_directories(gcn_dis PRIVATE src)
target_compile_options(gcn_dis PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(gcn_dis PRIVATE "-framework Metal" "-framework Foundation")
