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
    src/common/types.h
    src/core/loader/elf_types.h
    src/core/loader/linker.cpp
    src/core/loader/linker.h
    src/core/loader/module.cpp
    src/core/loader/module.h
    src/core/loader/self.cpp
    src/core/loader/self.h
    src/gpu/renderer.h
    src/gpu/metal/mtl_device.h
    src/gpu/metal/mtl_device.mm
    src/gpu/metal/mtl_presenter.h
    src/gpu/metal/mtl_presenter.mm
    src/hle/hle.cpp
    src/hle/hle.h
)

target_include_directories(driveclub PRIVATE src)

target_compile_options(driveclub PRIVATE
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<CONFIG:Release>:-O3 -march=native>
)

# Guest code is mapped at its PS4 addresses (executable at 0x400000), which
# sit inside the default 4 GB __PAGEZERO. Shrink it to free that range, and
# move our own image far above every guest region so ASLR cannot slide it in.
target_link_options(driveclub PRIVATE -Wl,-no_pie,-pagezero_size,0x4000,-image_base,0x700000000000)

target_link_libraries(driveclub PRIVATE
    SDL3::SDL3
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
)
