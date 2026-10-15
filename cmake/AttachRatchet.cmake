# Included by PS2RetroX's project() without changing upstream build files.
enable_testing()
function(ratchet_attach_game)
    file(GLOB RATCHET_GENERATED CONFIGURE_DEPENDS
        "${RATCHET_PROJECT_ROOT}/.local/recomp/boot/output/*.cpp")
    if(NOT RATCHET_GENERATED)
        message(FATAL_ERROR "Run tools/recompile-boot.py before building the native runner")
    endif()
    get_target_property(RATCHET_RUNNER_SOURCES ps2EntryRunner SOURCES)
    list(FILTER RATCHET_RUNNER_SOURCES EXCLUDE REGEX "/src/(runner/)?main\\.cpp$")
    list(FILTER RATCHET_RUNNER_SOURCES EXCLUDE REGEX "/src/runner/register_functions\\.cpp$")
    set_property(TARGET ps2EntryRunner PROPERTY SOURCES "${RATCHET_RUNNER_SOURCES}")
    target_sources(ps2EntryRunner PRIVATE
        "${RATCHET_PROJECT_ROOT}/native/main.cpp" ${RATCHET_GENERATED})
    target_include_directories(ps2EntryRunner BEFORE PRIVATE
        "${RATCHET_PROJECT_ROOT}/.local/recomp/boot/output")
    target_compile_options(ps2_runtime PUBLIC -msse4.1 -mavx2)
    set_target_properties(ps2EntryRunner PROPERTIES OUTPUT_NAME ratchet-recomp)

    find_package(Vulkan REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    find_program(RATCHET_GLSLANG glslangValidator REQUIRED)
    set(RATCHET_SHADER_ROOT "${RATCHET_PROJECT_ROOT}/native/shaders")
    set(RATCHET_SHADER_OUTPUT "${CMAKE_BINARY_DIR}/ratchet-shaders")
    set(RATCHET_FSR_HEADERS "${RATCHET_PROJECT_ROOT}/third_party/FidelityFX-FSR/ffx-fsr")
    set(RATCHET_GS_HEADERS "${CMAKE_SOURCE_DIR}/ps2xRuntime/include/runtime/gs")
    add_custom_command(OUTPUT "${RATCHET_SHADER_OUTPUT}/gs_tables.glsl"
        COMMAND "${Python3_EXECUTABLE}" "${RATCHET_PROJECT_ROOT}/tools/generate-gs-tables.py"
            "${RATCHET_GS_HEADERS}" "${RATCHET_SHADER_OUTPUT}/gs_tables.glsl"
        DEPENDS "${RATCHET_PROJECT_ROOT}/tools/generate-gs-tables.py"
            "${RATCHET_GS_HEADERS}/ps2_gs_psmct32.h" "${RATCHET_GS_HEADERS}/ps2_gs_psmct16.h"
            "${RATCHET_GS_HEADERS}/ps2_gs_psmt8.h" "${RATCHET_GS_HEADERS}/ps2_gs_psmt4.h")
    add_custom_command(OUTPUT "${RATCHET_SHADER_OUTPUT}/gs.spv"
        COMMAND "${RATCHET_GLSLANG}" -V --target-env vulkan1.1
            "-I${RATCHET_SHADER_OUTPUT}" -o "${RATCHET_SHADER_OUTPUT}/gs.spv" "${RATCHET_SHADER_ROOT}/gs.comp"
        DEPENDS "${RATCHET_SHADER_ROOT}/gs.comp" "${RATCHET_SHADER_OUTPUT}/gs_tables.glsl")
    foreach(RATCHET_FSR_STAGE easu rcas)
        set(RATCHET_FSR_DEFINE "-DRCAS=1")
        if(RATCHET_FSR_STAGE STREQUAL "easu")
            set(RATCHET_FSR_DEFINE "-DEASU=1")
        endif()
        add_custom_command(OUTPUT "${RATCHET_SHADER_OUTPUT}/${RATCHET_FSR_STAGE}.spv"
            COMMAND "${RATCHET_GLSLANG}" -V --target-env vulkan1.1 "${RATCHET_FSR_DEFINE}"
                "-I${RATCHET_FSR_HEADERS}" -o "${RATCHET_SHADER_OUTPUT}/${RATCHET_FSR_STAGE}.spv" "${RATCHET_SHADER_ROOT}/fsr.comp"
            DEPENDS "${RATCHET_SHADER_ROOT}/fsr.comp" "${RATCHET_FSR_HEADERS}/ffx_a.h" "${RATCHET_FSR_HEADERS}/ffx_fsr1.h")
    endforeach()
    add_custom_target(ratchet-vulkan-shaders DEPENDS "${RATCHET_SHADER_OUTPUT}/gs.spv"
        "${RATCHET_SHADER_OUTPUT}/easu.spv" "${RATCHET_SHADER_OUTPUT}/rcas.spv")
    add_library(ratchet-vulkan STATIC "${RATCHET_PROJECT_ROOT}/native/renderer/vulkan_gs.cpp")
    target_compile_features(ratchet-vulkan PRIVATE cxx_std_20)
    target_include_directories(ratchet-vulkan PUBLIC "${RATCHET_PROJECT_ROOT}/native/renderer")
    target_include_directories(ratchet-vulkan PRIVATE "${RATCHET_FSR_HEADERS}")
    target_compile_definitions(ratchet-vulkan PRIVATE RATCHET_SHADER_DIR="${RATCHET_SHADER_OUTPUT}")
    target_link_libraries(ratchet-vulkan PUBLIC ps2_runtime Vulkan::Vulkan SDL2::SDL2)
    add_dependencies(ratchet-vulkan ratchet-vulkan-shaders)
    target_link_libraries(ps2EntryRunner PRIVATE ratchet-vulkan)

    add_executable(ratchet-vulkan-tests EXCLUDE_FROM_ALL
        "${RATCHET_PROJECT_ROOT}/native/tests/vulkan-tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/test_function_table.cpp")
    target_link_libraries(ratchet-vulkan-tests PRIVATE ratchet-vulkan)
    add_test(NAME ratchet_vulkan_tests COMMAND ratchet-vulkan-tests)

    add_executable(ratchet-ee-tests EXCLUDE_FROM_ALL
        "${RATCHET_PROJECT_ROOT}/native/tests/ee-runtime-tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/ps2_runtime_interrupt_tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(ratchet-ee-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ps2xTest/include"
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/ps2xIOP/include")
    target_link_libraries(ratchet-ee-tests PRIVATE ps2_runtime ps2_iop)
    add_test(NAME ratchet_ee_interrupt_tests COMMAND ratchet-ee-tests)

    add_executable(ratchet-hardware-tests EXCLUDE_FROM_ALL
        "${RATCHET_PROJECT_ROOT}/native/tests/hardware-tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/ps2_memory_tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/ps2_gs_tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(ratchet-hardware-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ps2xTest/include"
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
        "${CMAKE_SOURCE_DIR}/ps2xIOP/include")
    target_link_libraries(ratchet-hardware-tests PRIVATE ps2_runtime ps2_iop)
    add_test(NAME ratchet_hardware_tests COMMAND ratchet-hardware-tests)

    add_executable(ratchet-mpeg-tests EXCLUDE_FROM_ALL
        "${RATCHET_PROJECT_ROOT}/native/tests/mpeg-runtime-tests.cpp"
        "${CMAKE_SOURCE_DIR}/ps2xTest/src/test_function_table.cpp")
    target_include_directories(ratchet-mpeg-tests PRIVATE
        "${CMAKE_SOURCE_DIR}/ps2xTest/include"
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/include"
        "${CMAKE_SOURCE_DIR}/ps2xRuntime/src/lib/Kernel"
        "${CMAKE_SOURCE_DIR}/ps2xIOP/include")
    target_link_libraries(ratchet-mpeg-tests PRIVATE ps2_runtime ps2_iop)
    add_test(NAME ratchet_mpeg_tests COMMAND ratchet-mpeg-tests)
endfunction()
cmake_language(DEFER CALL ratchet_attach_game)
