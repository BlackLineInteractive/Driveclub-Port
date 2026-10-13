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
    src/gpu/renderer.h
    src/gpu/metal/mtl_device.h
    src/gpu/metal/mtl_device.mm
    src/gpu/metal/mtl_presenter.h
    src/gpu/metal/mtl_presenter.mm
)

target_include_directories(driveclub PRIVATE src)

target_compile_options(driveclub PRIVATE
    -Wall -Wextra
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    $<$<CONFIG:Release>:-O3 -march=native>
)

target_link_libraries(driveclub PRIVATE
    SDL3::SDL3
    "-framework Metal"
    "-framework QuartzCore"
    "-framework Foundation"
)
