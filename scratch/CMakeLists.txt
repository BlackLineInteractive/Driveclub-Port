cmake_minimum_required(VERSION 3.24)
project(test_translate C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE RECOMPILER_SOURCES
    ../shadps4/src/shader_recompiler/*.cpp
    ../shadps4/externals/sirit/src/*.cpp
    ../shadps4/src/video_core/amdgpu/pixel_format.cpp
    ../shadps4/src/common/decoder.cpp
    ../shadps4/src/common/signal_context.cpp
    ../shadps4/tests/stubs/signals_stub.cpp
    ../shadps4/externals/zydis/src/*.c
    ../shadps4/externals/zydis/dependencies/zycore/src/*.c
)

add_executable(test_translate
    test_translate.cpp
    ${RECOMPILER_SOURCES}
)

target_include_directories(test_translate PRIVATE
    ../shadps4
    ../shadps4/src
    ../shadps4/externals/sirit/include
    ../shadps4/externals/sirit/src
    ../shadps4/externals/sirit/externals/SPIRV-Headers/include
    ../shadps4/externals/magic_enum/include
    ../shadps4/externals/xxhash
    ../shadps4/externals/robin-map/include
    ../shadps4/externals/half/include
    ../shadps4/externals/json/include
    ../shadps4/externals/xbyak
    ../shadps4/externals/zydis/include
    ../shadps4/externals/zydis/src
    ../shadps4/externals/zydis/dependencies/zycore/include
    /usr/local/include
)

target_compile_definitions(test_translate PRIVATE
    ZYCORE_STATIC_BUILD
    ZYDIS_STATIC_BUILD
)

target_compile_options(test_translate PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wno-deprecated-declarations>
    -O3
)

target_link_libraries(test_translate PRIVATE
    /usr/local/lib/libspirv-cross-msl.a
    /usr/local/lib/libspirv-cross-glsl.a
    /usr/local/lib/libspirv-cross-core.a
    /usr/local/lib/libfmt.a
)
