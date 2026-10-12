# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport build flavours. Two independent switches (docs/vr/public-online-build.md):
#   BBPORT_ONLINE (default ON): the online CLIENT: shadNet login, NP / Matching2 / signaling, HTTP and SSL
#       libraries, the room bells. OFF defines BB_NO_ONLINE=1 and builds the single-player-only variant.
#   BBPORT_PUBLIC (default OFF): a public build. Defines BB_PUBLIC_ALPHA=1: the dev tools are compiled out
#       (BB_DEBUG_GIVE / BB_DEBUG_USE, speffect, BB_NET_TRACE, BB_NET_BREAK / net state monitor, arbitrary
#       item or insight changes); only the room features that need them stay, restricted to the bells.
# Public release: -DBBPORT_ONLINE=ON -DBBPORT_PUBLIC=ON. Private dev build: both defaults.
# No server address, port or account is compiled in: the player's settings supply them.
# BBPORT_ONLINE=OFF alone (the older single-player public variant) still implies a public build.
set(BBPORT_ONLINE ON)
set(BBPORT_PUBLIC ON)
set(BB_PUBLIC_BUILD OFF)
if (BBPORT_PUBLIC OR NOT BBPORT_ONLINE)
    set(BB_PUBLIC_BUILD ON)
    add_compile_definitions(BB_PUBLIC_ALPHA=1)
endif()
if (NOT BBPORT_ONLINE)
    add_compile_definitions(BB_NO_ONLINE=1)
endif()
if (BB_PUBLIC_BUILD)
    # A public exe must not carry the builder's folder names (__FILE__, assert and panic strings, debug info).
    # The sources are rewritten to /src, the build tree to /build, in every translation unit of this tree.
    get_filename_component(BB_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    add_compile_options("-ffile-prefix-map=${BB_REPO_ROOT}=/src"
                        "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/build"
                        "-fmacro-prefix-map=${BB_REPO_ROOT}=/src"
                        "-fdebug-prefix-map=${BB_REPO_ROOT}=/src")
endif()

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
# bbport (Windows): MSYS2 has no magic_enum/miniz packages; the submodules are used instead.
find_package(magic_enum QUIET)
if (NOT magic_enum_FOUND)
    add_library(magic_enum INTERFACE)
    add_library(magic_enum::magic_enum ALIAS magic_enum)
    target_include_directories(magic_enum INTERFACE third_party/magic_enum/include
                                                    third_party/magic_enum/include/magic_enum)
endif()
find_package(tsl-robin-map REQUIRED)
find_package(VulkanMemoryAllocator CONFIG QUIET)
if (NOT VulkanMemoryAllocator_FOUND)
    # MSYS2 ships the header (include/vma/vk_mem_alloc.h) without a CMake package.
    find_path(VMA_INCLUDE_DIR vk_mem_alloc.h PATH_SUFFIXES vma REQUIRED)
    add_library(VulkanMemoryAllocator INTERFACE)
    add_library(GPUOpen::VulkanMemoryAllocator ALIAS VulkanMemoryAllocator)
    target_include_directories(VulkanMemoryAllocator INTERFACE ${VMA_INCLUDE_DIR})
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT WIN32)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz QUIET)
if (NOT miniz_FOUND)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
    add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
    add_library(miniz::miniz ALIAS miniz)
endif()
find_package(Zydis REQUIRED)
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SHADER_SRC}/${FILENAME}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SHADER_SRC}/${FILENAME} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fs_tri.vert fs_tri_vert)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(post_process.frag post_process_frag)
add_host_shader(xr_opaque.frag xr_opaque_frag)
add_host_shader(xr_fxaa.comp xr_fxaa_comp)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(xr_dlss_mask.comp xr_dlss_mask_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
add_host_shader(stereo_warp.comp stereo_warp_comp)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
set(COMMON
    shadps4/common/decoder.cpp shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
# Windows: a DLL cannot leave the runtime_* symbols for bb-probe.exe to resolve, so the
# library is static there and linked into the executable.
if (WIN32)
    set(BBGPU_KIND STATIC)
else()
    set(BBGPU_KIND SHARED)
endif()
add_library(bbgpu ${BBGPU_KIND} ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
if (BBPORT_ONLINE)
    # bbport co-op: PS4 network and PSN libraries with the shadNet client, from shadp2p
    # (github.com/Wozzardman/shadp2p, GPL-2.0), for private Bloodborne co-op servers.
    find_package(Protobuf CONFIG REQUIRED)
    set(SHADNET_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
    file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT}")
    add_custom_command(
        OUTPUT "${SHADNET_PROTO_OUT}/shadnet.pb.cc" "${SHADNET_PROTO_OUT}/shadnet.pb.h"
        COMMAND protobuf::protoc "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet"
                "--cpp_out=${SHADNET_PROTO_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto")
    file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS shadps4/core/libraries/network/*.cpp
        shadps4/core/libraries/np/*.cpp shadps4/shadnet/*.cpp)
    # Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
    # emulator's UI layers.
    list(FILTER BBNET_SOURCES EXCLUDE REGEX
        "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")
    find_package(nlohmann_json CONFIG REQUIRED)
    add_library(bbnet STATIC ${BBNET_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc" bbnet/bbnet_glue.cpp)
    target_include_directories(bbnet PRIVATE shim shadps4 shadps4/shadnet "${SHADNET_PROTO_OUT}"
        third_party/imgui third_party/httplib)
    target_link_libraries(bbnet PRIVATE nlohmann_json::nlohmann_json)
    # HTTPS (the community game server only speaks TLS) and gzip replies in cpp-httplib.
    find_package(OpenSSL REQUIRED)
    find_package(ZLIB REQUIRED)
    target_compile_definitions(bbnet PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
    target_link_libraries(bbnet PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
    if (WIN32)
        target_sources(bbnet PRIVATE third_party/wepoll/wepoll.c)
        target_include_directories(bbnet PRIVATE third_party/wepoll)
        find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc REQUIRED)
        find_library(MINIUPNPC_LIBRARY NAMES libminiupnpc.a miniupnpc REQUIRED)
        target_include_directories(bbnet PRIVATE "${MINIUPNPC_INCLUDE}")
        target_compile_definitions(bbnet PRIVATE MINIUPNP_STATICLIB)
        target_link_libraries(bbnet PRIVATE "${MINIUPNPC_LIBRARY}")
    endif()
    target_compile_definitions(bbnet PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
    target_compile_options(bbnet PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
    target_link_libraries(bbnet PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        protobuf::libprotobuf)
    if (WIN32)
        target_compile_definitions(bbnet PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
        target_compile_options(bbnet PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h>)
        target_link_libraries(bbnet PUBLIC ws2_32 iphlpapi bcrypt crypt32)
    endif()
    target_link_libraries(bbgpu PUBLIC bbnet)
endif()
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Vulkan backend, and an
# embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
    third_party/imgui/backends/imgui_impl_vulkan.cpp)
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
if (NOT WIN32)
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable ffx-vulkan::fsr4-v07-vulkan)
target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
    tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
if (WIN32)
    # Guest code is entered with the System V ABI; NOMINMAX/WIN32_LEAN_AND_MEAN keep
    # <windows.h> out of the way of std::min/max and the Vulkan/SDL headers.
    target_compile_definitions(bbgpu PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(bbgpu PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/shim/bb_win_compat.h)
    target_include_directories(bbgpu PRIVATE third_party/xbyak)
    target_link_libraries(bbgpu PUBLIC Vulkan::Vulkan ws2_32 bcrypt ntdll synchronization winmm)
else()
    target_link_libraries(bbgpu PUBLIC PkgConfig::X11)
endif()

# bbport (Windows): bbgpu is a static library there, so the loader and the HLE runtime are
# linked here, where CMake knows its dependencies (build.sh builds them with ninja).
if (WIN32)
    enable_language(RC)
    file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ../src/runtime*.c)
    if (BB_PUBLIC_BUILD)
        list(FILTER BB_RUNTIME EXCLUDE REGEX "runtime_vr_netstate\\.c$")
    endif()
    file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ../third_party/LibAtrac9/C/src/*.c)
    add_library(atrac9 STATIC ${BB_ATRAC9})
    target_compile_options(atrac9 PRIVATE -w)
    set_target_properties(atrac9 PROPERTIES C_STANDARD 99 INTERPROCEDURAL_OPTIMIZATION OFF)
    add_executable(bb-probe ../src/probe.c ${BB_RUNTIME} ../src/compat_win.c ../src/vulkan_smoke.c
        ../packaging/windows/bb-probe.rc)
    set_target_properties(bb-probe PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
    target_compile_options(bb-probe PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror>)
    if (BB_PUBLIC_BUILD)
        target_compile_definitions(bb-probe PRIVATE BB_PUBLIC_ALPHA=1)
    endif()
    if (NOT BBPORT_ONLINE)
        target_compile_definitions(bb-probe PRIVATE BB_NO_ONLINE=1)
    endif()
    target_include_directories(bb-probe PRIVATE .. ../src)
    target_link_libraries(bb-probe PRIVATE bbgpu atrac9 onecore bcrypt ws2_32 -pthread)
    # Crash reports (src/runtime_crash.c): the build id is the git revision at configure time; the
    # CodeView debug info and the .pdb beside bb-probe.exe let a minidump be read in WinDbg.
    execute_process(COMMAND git describe --always --dirty --abbrev=10
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} OUTPUT_VARIABLE BB_BUILD_ID OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (NOT BB_BUILD_ID)
        set(BB_BUILD_ID unknown)
    endif()
    set_property(SOURCE ../src/runtime_crash.c APPEND PROPERTY COMPILE_DEFINITIONS BB_BUILD_ID="${BB_BUILD_ID}")
    if (BB_PUBLIC_BUILD)
        # No .pdb: the linker would write its absolute build path into the exe (public builds carry no builder paths).
        target_link_options(bb-probe PRIVATE -fuse-ld=lld)
    else()
        target_compile_options(bb-probe PRIVATE $<$<COMPILE_LANGUAGE:C>:-gcodeview>)
        target_link_options(bb-probe PRIVATE -fuse-ld=lld -Wl,--pdb=$<TARGET_FILE_DIR:bb-probe>/bb-probe.pdb)
    endif()
    # The PS4 range (64 GiB - 1 TiB) must stay free: no high-entropy ASLR for heaps and stacks.
    # The guest main thread runs on the process stack: 16 MiB, committed (no stack probes).
    target_link_options(bb-probe PRIVATE -Wl,--disable-high-entropy-va "SHELL:-Xlinker --stack -Xlinker 16777216,16777216")
    add_executable(bb-gpu-capabilities ../tools/gpu_capabilities.c ../packaging/windows/bb-gpu-capabilities.rc)
    target_link_libraries(bb-gpu-capabilities PRIVATE Vulkan::Vulkan)
    # "Play Bloodborne.exe": the game with the launcher's saved settings, no launcher window.
    add_executable(bb-play WIN32 ../packaging/windows/play.c ../packaging/windows/play.rc)
    target_compile_options(bb-play PRIVATE -municode)
    target_link_options(bb-play PRIVATE -municode)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)
add_executable(stereo-warp-test EXCLUDE_FROM_ALL ../tests/test_stereo_warp.cpp)
target_compile_options(stereo-warp-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(stereo-warp-test PRIVATE ..)
# Command buffers copied at submit (pm4_snapshot.h): the "Unimplemented PM4 type 0" load crash.
add_executable(pm4-snapshot-test EXCLUDE_FROM_ALL ../tests/test_pm4_snapshot.cpp)
target_compile_options(pm4-snapshot-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(pm4-snapshot-test PRIVATE ..)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_taa_shader.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(taa-shader-test PRIVATE bbgpu)
add_dependencies(taa-shader-test host_shaders)

add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
add_dependencies(camera-motion-test host_shaders)
# VR true stereo P4.4 slice 3: eye-R descriptor substitution, arena eye flag, dispatch write policy.
add_executable(stereo-substitute-test EXCLUDE_FROM_ALL ../tests/test_stereo_substitute.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(stereo-substitute-test PRIVATE shim shadps4 third_party/gcn/include)
target_compile_options(stereo-substitute-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(stereo-substitute-test PRIVATE bbgpu)

# Host-call hook trampoline test (src/runtime_hooks.c). Self-contained: no GPU/Vulkan, no runtime.
add_executable(hooks-test EXCLUDE_FROM_ALL ../tests/test_hooks.c ../src/runtime_hooks.c)
set_target_properties(hooks-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(hooks-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(hooks-test PRIVATE ../src)

# Red zone guard equivalence test (src/runtime_redzone_guard.c): original vs relocated eboot functions.
add_executable(redzone-guard-test EXCLUDE_FROM_ALL ../tests/test_redzone_guard.c)
set_target_properties(redzone-guard-test PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_options(redzone-guard-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(redzone-guard-test PRIVATE ../src)

if (NOT BB_PUBLIC_BUILD)
# BB_NET_TRACE masking/formatting helpers (src/net_trace_fmt.h).
add_executable(net-trace-test EXCLUDE_FROM_ALL ../tests/test_net_trace.c)
set_target_properties(net-trace-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(net-trace-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(net-trace-test PRIVATE ../src)

endif()
# VR head tracking math (src/vr_head_math.h): camera composition, reference, FOV, fake head.
add_executable(vr-head-test EXCLUDE_FROM_ALL ../tests/test_vr_head.c)
set_target_properties(vr-head-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-head-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-head-test PRIVATE ../src)

# VR turn and heading model (src/vr_turn_math.h), shared with the C++ sandbox (gpu/xr_bridge/
# sandbox3d): the same tests built as C and as C++.
add_executable(vr-turn-test EXCLUDE_FROM_ALL ../tests/test_vr_turn.c)
set_target_properties(vr-turn-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-turn-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-turn-test PRIVATE ../src)
add_executable(vr-turn-test-cpp EXCLUDE_FROM_ALL ../tests/test_vr_turn_cpp.cpp)
target_compile_options(vr-turn-test-cpp PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-turn-test-cpp PRIVATE ../src)
# VR arm IK (src/vr_arm_math.h), shared with the C++ sandbox (sandbox3d/rig.cpp): findings/24.
add_executable(vr-arms-test EXCLUDE_FROM_ALL ../tests/test_vr_arms.c)
set_target_properties(vr-arms-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-arms-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-arms-test PRIVATE ../src)
add_executable(vr-arms-test-cpp EXCLUDE_FROM_ALL ../tests/test_vr_arms_cpp.cpp)
target_compile_options(vr-arms-test-cpp PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-arms-test-cpp PRIVATE ../src)
# NP signaling reuse decision (np_signaling_reuse.h): retire a lingering connection instead of reviving it.
add_executable(np-signaling-reuse-test EXCLUDE_FROM_ALL ../tests/test_np_signaling_reuse.cpp)
target_compile_options(np-signaling-reuse-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(np-signaling-reuse-test PRIVATE shadps4)
# P2P <-> matching server STUN framing (p2p_server_frame.h): 6-byte server header, echo, advertised address.
add_executable(p2p-server-frame-test EXCLUDE_FROM_ALL ../tests/test_p2p_server_frame.cpp)
target_compile_options(p2p-server-frame-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(p2p-server-frame-test PRIVATE shadps4)
# VR hit shape scaling (src/vr_hit_scale.h): radius x scale and extra reach, shared with the sandbox; findings/32.
add_executable(vr-hit-scale-test EXCLUDE_FROM_ALL ../tests/test_vr_hit_scale.c)
set_target_properties(vr-hit-scale-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-hit-scale-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-hit-scale-test PRIVATE ../src)
add_executable(vr-hit-scale-test-cpp EXCLUDE_FROM_ALL ../tests/test_vr_hit_scale_cpp.cpp)
target_compile_options(vr-hit-scale-test-cpp PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-hit-scale-test-cpp PRIVATE ../src ../tests)
# VR gun aim (src/vr_gun_math.h): the controller ray in the world and the bullet launch redirect, findings/27.
add_executable(vr-gun-test EXCLUDE_FROM_ALL ../tests/test_vr_gun.c)
set_target_properties(vr-gun-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-gun-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-gun-test PRIVATE ../src)
# VR throw animation speed-up state machine (src/vr_throw_anim.h, src/runtime_vr_atkspeed.c): findings/39.
add_executable(vr-throw-anim-test EXCLUDE_FROM_ALL ../tests/test_vr_throw_anim.c)
set_target_properties(vr-throw-anim-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-throw-anim-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-throw-anim-test PRIVATE ../src)
# VR gun-shot animation speed (src/vr_gun_speed.h, src/runtime_vr_atkspeed.c): findings/27 section 8.
add_executable(vr-gun-speed-test EXCLUDE_FROM_ALL ../tests/test_vr_gun_speed.c)
set_target_properties(vr-gun-speed-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-gun-speed-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-gun-speed-test PRIVATE ../src)
# VR hit-reaction animation speed (src/vr_hurt_speed.h, src/runtime_vr_atkspeed.c): findings/27 section 9.
add_executable(vr-hurt-speed-test EXCLUDE_FROM_ALL ../tests/test_vr_hurt_speed.c)
set_target_properties(vr-hurt-speed-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-hurt-speed-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-hurt-speed-test PRIVATE ../src)
# VR held-item fire block (gpu/xr_bridge/vr_held_fire.h): which attack inputs a hand holding an item loses.
add_executable(vr-held-fire-test EXCLUDE_FROM_ALL ../tests/test_vr_held_fire.c)
set_target_properties(vr-held-fire-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-held-fire-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-held-fire-test PRIVATE ..)
# Auto-bell state machine for rooms (src/vr_online_bell.h, src/runtime_online_bell.c).
add_executable(vr-online-bell-test EXCLUDE_FROM_ALL ../tests/test_vr_online_bell.c)
set_target_properties(vr-online-bell-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-online-bell-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-online-bell-test PRIVATE ../src)
# VR visceral window and thrust gate (src/vr_visceral_math.h, src/runtime_vr_visceral.c): findings/41 section 7.
add_executable(vr-visceral-test EXCLUDE_FROM_ALL ../tests/test_vr_visceral.c)
set_target_properties(vr-visceral-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-visceral-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-visceral-test PRIVATE ../src)
# VR hitbox overlay placement (src/vr_hitbox_math.h, src/runtime_vr_hitbox.c): findings/29.
add_executable(vr-hitbox-test EXCLUDE_FROM_ALL ../tests/test_vr_hitbox.c)
set_target_properties(vr-hitbox-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-hitbox-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-hitbox-test PRIVATE ../src)
# Custom character model override math (src/vr_skin_math.h, src/runtime_vr_skin.c).
add_executable(vr-skin-test EXCLUDE_FROM_ALL ../tests/test_vr_skin.c)
set_target_properties(vr-skin-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-skin-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-skin-test PRIVATE ../src)
# Replays a first-person trace (BB_VR_FP_TRACE) through the same turn model: findings/16 9.6.
add_executable(vr-turn-replay EXCLUDE_FROM_ALL ../tools/vr_turn_replay.c)
set_target_properties(vr-turn-replay PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-turn-replay PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-turn-replay PRIVATE ../src)

# VR hook profiler (src/vr_hook_prof.h, src/runtime_hooks.c): the 10 s "VR hooks:" line.
add_executable(vr-hook-prof-test EXCLUDE_FROM_ALL ../tests/test_vr_hook_prof.c)
set_target_properties(vr-hook-prof-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-hook-prof-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-hook-prof-test PRIVATE ../src)

# VR guest-memory guard cache (src/vr_mem_cache.h).
add_executable(vr-mem-cache-test EXCLUDE_FROM_ALL ../tests/test_vr_mem_cache.c)
set_target_properties(vr-mem-cache-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-mem-cache-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-mem-cache-test PRIVATE ../src)

# VR comfort dash (gpu/xr_bridge/vr_dash.h built as C, src/vr_dash_detect.h): docs/vr/findings/19.
add_executable(vr-dash-test EXCLUDE_FROM_ALL ../tests/test_vr_dash.c)
set_target_properties(vr-dash-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-dash-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-dash-test PRIVATE .. ../src)

# VR "misses are free" stamina tickets (src/vr_stamina_tickets.h, src/runtime_vr_stamina.c):
# docs/vr/findings/20 6.2 G3. The same tests built as C and as C++.
# VR UI, the game side (src/vr_ui_game.h, src/runtime_vr_ui.c): the item/quick-slot reader and the
# button executor on a fake PlayerGameData (docs/vr/findings/26-game-ui.md).
add_executable(vr-ui-test EXCLUDE_FROM_ALL ../tests/test_vr_ui.c)
set_target_properties(vr-ui-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-ui-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-ui-test PRIVATE ../src)
if (NOT BB_PUBLIC_BUILD)
# Debug give / use for co-op tests (src/vr_debug_give.h): the BB_DEBUG_GIVE and BB_DEBUG_USE parsers.
add_executable(vr-debug-give-test EXCLUDE_FROM_ALL ../tests/test_vr_debug_give.c)
set_target_properties(vr-debug-give-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-debug-give-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-debug-give-test PRIVATE ../src)
endif()
if (BBPORT_ONLINE)
# Free co-op bells (src/vr_free_bells.h)
add_executable(vr-free-bells-test EXCLUDE_FROM_ALL ../tests/test_vr_free_bells.c)
set_target_properties(vr-free-bells-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-free-bells-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-free-bells-test PRIVATE ../src)
endif()
if (NOT BB_PUBLIC_BUILD)
# BB_NET_BREAK dump spec parser (src/vr_netstate_spec.h)
add_executable(vr-netstate-spec-test EXCLUDE_FROM_ALL ../tests/test_vr_netstate_spec.c)
set_target_properties(vr-netstate-spec-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-netstate-spec-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-netstate-spec-test PRIVATE ../src)
endif()
add_executable(vr-stamina-test EXCLUDE_FROM_ALL ../tests/test_vr_stamina.c)
set_target_properties(vr-stamina-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-stamina-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-stamina-test PRIVATE ../src)
add_executable(vr-stamina-test-cpp EXCLUDE_FROM_ALL ../tests/test_vr_stamina_cpp.cpp)
target_compile_options(vr-stamina-test-cpp PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-stamina-test-cpp PRIVATE ../src)

# VR alternate-eye stereo (src/vr_stereo_math.h): eye schedule, per-eye offset, eye tag, frustum
# coverage, control file. docs/vr/findings/22.
add_executable(vr-stereo-test EXCLUDE_FROM_ALL ../tests/test_vr_stereo.c)
set_target_properties(vr-stereo-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-stereo-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-stereo-test PRIVATE ../src)
# Its off-axis projection hooks: the real eboot epilogue windows (ending in ret) through the
# trampoline (src/runtime_hooks.c).
add_executable(vr-stereo-hooks-test EXCLUDE_FROM_ALL ../tests/test_vr_stereo_hooks.c ../src/runtime_hooks.c)
set_target_properties(vr-stereo-hooks-test PROPERTIES C_STANDARD 11 C_EXTENSIONS OFF)
target_compile_options(vr-stereo-hooks-test PRIVATE -Wall -Wextra -Werror)
target_include_directories(vr-stereo-hooks-test PRIVATE ../src)

# bbport: FSR 4 benchmark outside the game (tools/fsr4_bench.cpp): ninja -C out/gpu fsr4-bench.
add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)

add_executable(fsr4-bench EXCLUDE_FROM_ALL tools/fsr4_bench.cpp
    shadps4/video_core/renderer_vulkan/fsr411/fsr411.cpp)
target_include_directories(fsr4-bench PRIVATE shadps4/video_core/renderer_vulkan/fsr411)
target_link_libraries(fsr4-bench PRIVATE ffx-vulkan::fsr4-v07-vulkan ffx-vulkan::fsr4-v07-assets Vulkan::Vulkan)

# bbport VR DLSS logic (renderer_vulkan/xr_dlss_logic.h): options, output size, fallback, per-eye
# jitter and history, motion vector math. docs/vr/findings/37-dlss-vr.md.
add_executable(xr-dlss-test EXCLUDE_FROM_ALL ../tests/test_xr_dlss.cpp)
target_compile_options(xr-dlss-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(xr-dlss-test PRIVATE ..)

# bbport VR frame rate counter (renderer_vulkan/xr_fps_logic.h): colour thresholds, smoothing,
# bottleneck verdict, bitmap text. docs/vr/findings/31-true-stereo.md section 10.9.
add_executable(xr-view-predict-test EXCLUDE_FROM_ALL ../tests/test_xr_view_predict.cpp)
target_compile_options(xr-view-predict-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(xr-view-predict-test PRIVATE ..)

add_executable(xr-fps-test EXCLUDE_FROM_ALL ../tests/test_xr_fps.cpp)
target_compile_options(xr-fps-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(xr-fps-test PRIVATE ..)

# bbport VR HUD centre gate (renderer_vulkan/xr_hud_center.h): the middle-of-the-view HUD quad is
# hidden in plain gameplay (HP and stamina are on the wrist); mode parse, message detection, hold.
add_executable(xr-hud-center-test EXCLUDE_FROM_ALL ../tests/test_xr_hud_center.cpp)
target_compile_options(xr-hud-center-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(xr-hud-center-test PRIVATE ..)

# bbport VR resize bookkeeping (renderer_vulkan/xr_resize_logic.h): frame size debounce, resize
# reason, swapchain size tracking (device-lost crashes on a one-frame eye size flip).
add_executable(xr-resize-test EXCLUDE_FROM_ALL ../tests/test_xr_resize.cpp)
target_compile_options(xr-resize-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(xr-resize-test PRIVATE ..)

# bbport VR DLSS-NR logic (renderer_vulkan/xr_dlssnr_logic.h): settings, pass regions, streams and
# slots, history reset. docs/vr/findings/42-dlss-nr.md.
add_executable(xr-dlssnr-test EXCLUDE_FROM_ALL ../tests/test_xr_dlssnr.cpp)
target_compile_options(xr-dlssnr-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(xr-dlssnr-test PRIVATE ..)

# bbport: the DLSS bridge (gpu/dlss_bridge, bbport_dlss.dll, MIT) links NVIDIA's DLSS SDK, which
# ships MSVC libraries only, so it is its own MSVC project. BB_DLSS_SDK_ROOT (a checkout or copy
# of github.com/NVIDIA/DLSS: include/ and lib/Windows_x86_64/) adds `ninja bbport-dlss`, which
# configures and builds it with Visual Studio 2022 into <build>/dlss-bridge/Release. Nothing of
# the SDK is compiled into or linked with bbgpu; the port loads the bridge at run time.
set(BB_DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root for the bbport-dlss target (optional)")
if (WIN32 AND BB_DLSS_SDK_ROOT)
    if (NOT EXISTS "${BB_DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
        message(FATAL_ERROR "BB_DLSS_SDK_ROOT=${BB_DLSS_SDK_ROOT} has no include/nvsdk_ngx_vk.h")
    endif()
    # Only the Vulkan headers: the toolchain's C headers must not reach MSVC.
    find_path(BB_VULKAN_HEADERS vulkan/vulkan.h REQUIRED)
    set(BB_DLSS_VKINCLUDE "${CMAKE_CURRENT_BINARY_DIR}/dlss-vkinclude")
    file(COPY "${BB_VULKAN_HEADERS}/vulkan" "${BB_VULKAN_HEADERS}/vk_video"
         DESTINATION "${BB_DLSS_VKINCLUDE}")
    find_program(BB_MSVC_CMAKE cmake.exe PATHS
        "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
        "C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
        NO_DEFAULT_PATH)
    if (NOT BB_MSVC_CMAKE)
        set(BB_MSVC_CMAKE "${CMAKE_COMMAND}")
    endif()
    add_custom_target(bbport-dlss
        COMMAND "${BB_MSVC_CMAKE}" -S "${CMAKE_CURRENT_SOURCE_DIR}/dlss_bridge"
                -B "${CMAKE_CURRENT_BINARY_DIR}/dlss-bridge" -G "Visual Studio 17 2022" -A x64
                "-DDLSS_SDK_ROOT=${BB_DLSS_SDK_ROOT}" "-DVULKAN_INCLUDE=${BB_DLSS_VKINCLUDE}"
        COMMAND "${BB_MSVC_CMAKE}" --build "${CMAKE_CURRENT_BINARY_DIR}/dlss-bridge" --config Release
        COMMENT "bbport_dlss.dll (MSVC, NVIDIA DLSS SDK at ${BB_DLSS_SDK_ROOT})"
        VERBATIM USES_TERMINAL)
endif()

# bbport: headless smoke test of a built bbport_dlss.dll (MIT, uses only the bridge's C interface):
# two VR eye features created and evaluated on synthetic images, no game. Needs an NVIDIA RTX GPU
# and nvngx_dlss.dll next to the bridge: dlss-bridge-smoke.exe <directory of the two DLLs>.
if (WIN32)
    add_executable(dlss-bridge-smoke EXCLUDE_FROM_ALL dlss_bridge/bbport_dlss_smoke.cpp)
    target_compile_options(dlss-bridge-smoke PRIVATE -Wno-missing-designated-field-initializers)
    target_link_libraries(dlss-bridge-smoke PRIVATE Vulkan::Vulkan)
endif()
