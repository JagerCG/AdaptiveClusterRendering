cmake_minimum_required(VERSION 3.24)
project(AdaptiveClusterRendering VERSION 0.1.0 LANGUAGES C CXX)

find_package(Vulkan 1.2 REQUIRED COMPONENTS glslc)

include(FetchContent)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG a74efa0d5628b74adc0426af4c5710e287fa7c2c # GLFW 3.4
)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 8d1fd52e5ab5590e2c81768ace50c72bae28f2ed # GLM 1.0.3
)
FetchContent_Declare(tinyobjloader
    GIT_REPOSITORY https://github.com/tinyobjloader/tinyobjloader.git
    GIT_TAG 2945a967c5303b2c8c14174117c45f3302591150 # v2.0.0rc13
)
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG 6d910d5487d11ca567b61c7824b0c78c569d62f0 # v1.92.5
)
FetchContent_MakeAvailable(glfw glm tinyobjloader imgui)

add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(imgui PRIVATE glfw Vulkan::Vulkan)

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/$<CONFIG>")
set(SHADER_SOURCES)
set(COMPILED_SHADERS)
foreach(SHADER IN ITEMS mesh.vert mesh.frag shadow.vert shadow.frag shadow_debug.vert shadow_debug.frag bounds.vert bounds.frag generate_geometry.comp)
    set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER}")
    set(SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER}.inc")
    add_custom_command(
        OUTPUT "${SHADER_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
        COMMAND Vulkan::glslc --target-env=vulkan1.2 -mfmt=num "${SHADER_SOURCE}" -o "${SHADER_OUTPUT}"
        DEPENDS "${SHADER_SOURCE}" Vulkan::glslc
        VERBATIM
    )
    list(APPEND SHADER_SOURCES "${SHADER_SOURCE}")
    list(APPEND COMPILED_SHADERS "${SHADER_OUTPUT}")
endforeach()

set_source_files_properties(${SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)
source_group("Shaders" FILES ${SHADER_SOURCES})
source_group("Generated Shaders" REGULAR_EXPRESSION "[.]inc$")

add_library(acr_scene STATIC
    src/RenderPlanning.cpp
    src/RenderPlanning.h
    src/Benchmark.cpp
    src/Benchmark.h
    src/ClusterBuilder.cpp
    src/ClusterBuilder.h
    src/QemClusterBuilder.cpp
    src/QemClusterBuilder.h
    src/MeshSource.cpp
    src/MeshSource.h
    src/ObjLoader.cpp
    src/ObjLoader.h
    src/SceneData.cpp
    src/SceneData.h
    src/SceneDiagnostics.cpp
    src/SceneDiagnostics.h
    src/TessellationFixtures.cpp
    src/TessellationFixtures.h
    src/ClusterSelection.cpp
    src/ClusterSelection.h
    src/GeometryGenerationPlan.cpp
    src/GeometryGenerationPlan.h
    src/GeometryValidation.cpp
    src/GeometryValidation.h
    src/HeightField.cpp
    src/HeightField.h
    src/ClusterGpu.h
)
target_include_directories(acr_scene PUBLIC src)
target_compile_features(acr_scene PUBLIC cxx_std_17)
target_link_libraries(acr_scene PUBLIC glm::glm PRIVATE tinyobjloader)

add_executable(AdaptiveClusterRendering
    src/main.cpp
    src/OrbitCamera.cpp
    src/OrbitCamera.h
    src/Viewer.cpp
    src/Viewer.h
    src/VulkanRenderer.cpp
    src/VulkanRendererShadow.cpp
    tests/RenderIntegrationChecks.cpp
    src/VulkanRenderer.h
    src/SceneGpuData.h
    src/ComputeGeometry.cpp
    src/ComputeGeometry.h
    ${SHADER_SOURCES}
    ${COMPILED_SHADERS}
)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT AdaptiveClusterRendering)
target_include_directories(AdaptiveClusterRendering PRIVATE "${SHADER_OUTPUT_DIR}")
target_compile_features(AdaptiveClusterRendering PRIVATE cxx_std_17)
set_target_properties(AdaptiveClusterRendering PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(AdaptiveClusterRendering PRIVATE GLFW_INCLUDE_NONE ACR_BUILD_CONFIG="$<CONFIG>")
target_link_libraries(AdaptiveClusterRendering PRIVATE acr_scene Vulkan::Vulkan glfw imgui)
if(WIN32)
    target_link_libraries(AdaptiveClusterRendering PRIVATE comdlg32)
endif()

file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:AdaptiveClusterRendering>/assets/suzanne.obj"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj"
    NEWLINE_STYLE UNIX
)
file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:AdaptiveClusterRendering>/assets/heightmaps/ramp-u.pgm"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmaps/ramp-u.pgm"
    NEWLINE_STYLE UNIX
)

if(MSVC)
    target_compile_options(acr_scene PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(AdaptiveClusterRendering PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(acr_scene PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(AdaptiveClusterRendering PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
option(ACR_ENABLE_GPU_TESTS "Run Vulkan smoke tests (requires a working GPU/window system)" OFF)
if(BUILD_TESTING)
    add_executable(render_planning_tests tests/RenderPlanningTests.cpp)
    target_link_libraries(render_planning_tests PRIVATE acr_scene)
    add_test(NAME render-planning COMMAND render_planning_tests)
    add_executable(benchmark_tests tests/BenchmarkTests.cpp)
    target_link_libraries(benchmark_tests PRIVATE acr_scene)
    if(MSVC)
        target_compile_options(benchmark_tests PRIVATE /utf-8)
    endif()
    foreach(CASE IN ITEMS stats session export)
        add_test(NAME benchmark-${CASE} COMMAND benchmark_tests ${CASE} "${CMAKE_CURRENT_BINARY_DIR}/test-data/benchmark")
    endforeach()
    add_executable(scene_tests tests/SceneTests.cpp)
    target_link_libraries(scene_tests PRIVATE acr_scene)
    foreach(CASE IN ITEMS mock roundtrip real-model invalid-scene corrupt-file qem-guided storage-stats)
        add_test(NAME scene-${CASE} COMMAND scene_tests ${CASE} "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj"
                 "${CMAKE_CURRENT_BINARY_DIR}/test-data/${CASE}")
    endforeach()
    add_executable(fixture_tests tests/FixtureTests.cpp)
    target_link_libraries(fixture_tests PRIVATE acr_scene)
    foreach(CASE IN ITEMS triangle plane grid selection)
        add_test(NAME fixture-${CASE} COMMAND fixture_tests ${CASE} "${CMAKE_CURRENT_BINARY_DIR}/test-data/fixture-${CASE}")
    endforeach()
    add_executable(geometry_copy_tests tests/GeometryCopyTests.cpp)
    target_link_libraries(geometry_copy_tests PRIVATE acr_scene)
    foreach(CASE IN ITEMS selection limits)
        add_test(NAME geometry-copy-${CASE} COMMAND geometry_copy_tests ${CASE})
    endforeach()
    add_executable(subdivision_tests tests/SubdivisionTests.cpp)
    target_link_libraries(subdivision_tests PRIVATE acr_scene)
    foreach(CASE IN ITEMS plans verifier)
        add_test(NAME subdivision-${CASE} COMMAND subdivision_tests ${CASE})
    endforeach()
    add_executable(displacement_tests tests/DisplacementTests.cpp)
    target_link_libraries(displacement_tests PRIVATE acr_scene)
    foreach(CASE IN ITEMS sampling parser verifier)
        add_test(NAME displacement-${CASE} COMMAND displacement_tests ${CASE} "${CMAKE_CURRENT_BINARY_DIR}/test-data/heights")
    endforeach()
    if(ACR_ENABLE_GPU_TESTS)
        add_test(NAME gpu-render-integration COMMAND AdaptiveClusterRendering --mock --render-smoke)
        set_tests_properties(gpu-render-integration PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu"
            FAIL_REGULAR_EXPRESSION "Vulkan validation (error|warning)")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu")
        foreach(CASE IN ITEMS all partial empty source shadows shadow-map)
            add_test(NAME gpu-benchmark-${CASE} COMMAND "${CMAKE_COMMAND}"
                "-DVIEWER=$<TARGET_FILE:AdaptiveClusterRendering>" "-DCONFIG=$<CONFIG>" "-DMODE=${CASE}"
                "-DREPORT=${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/benchmark-${CASE}.json"
                "-DHEIGHT=${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmaps/ramp-u.pgm"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BenchmarkReport.cmake")
            set_tests_properties(gpu-benchmark-${CASE} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu")
        endforeach()
        add_test(NAME scene-preprocess COMMAND AdaptiveClusterRendering --preprocess
                 "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj" "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/suzanne.acr")
        set_tests_properties(scene-preprocess PROPERTIES FIXTURES_SETUP cached_scene)
        add_test(NAME gpu-mock COMMAND AdaptiveClusterRendering --mock
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/mock-roundtrip.acr")
        add_test(NAME gpu-obj COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj"
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/obj-roundtrip.acr")
        add_test(NAME gpu-cached COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/suzanne.acr"
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/cached-roundtrip.acr")
        set_tests_properties(gpu-cached PROPERTIES FIXTURES_REQUIRED cached_scene)
        foreach(FIXTURE IN ITEMS triangle plane grid shadow)
            add_test(NAME gpu-fixture-${FIXTURE} COMMAND AdaptiveClusterRendering --fixture ${FIXTURE}
                     --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/${FIXTURE}-roundtrip.acr")
        endforeach()
        set_tests_properties(gpu-mock gpu-obj gpu-cached gpu-fixture-triangle gpu-fixture-plane gpu-fixture-grid gpu-fixture-shadow PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu"
            FAIL_REGULAR_EXPRESSION "Vulkan validation (error|warning)")
        add_test(NAME gpu-copy-mock COMMAND AdaptiveClusterRendering --mock --compute-copy
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/copy-mock.acr")
        add_test(NAME gpu-copy-obj COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj" --compute-copy
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/copy-obj.acr")
        add_test(NAME gpu-copy-cached COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/suzanne.acr" --compute-copy
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/copy-cached.acr")
        set_tests_properties(gpu-copy-cached PROPERTIES FIXTURES_REQUIRED cached_scene)
        foreach(FIXTURE IN ITEMS triangle plane grid)
            add_test(NAME gpu-copy-${FIXTURE} COMMAND AdaptiveClusterRendering --fixture ${FIXTURE} --compute-copy
                     --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/copy-${FIXTURE}.acr")
        endforeach()
        set_tests_properties(gpu-copy-mock gpu-copy-obj gpu-copy-cached gpu-copy-triangle gpu-copy-plane gpu-copy-grid PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu"
            FAIL_REGULAR_EXPRESSION "Vulkan validation (error|warning)")
        add_test(NAME gpu-subdivision-mock COMMAND AdaptiveClusterRendering --mock --subdivision 2
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/subdivision-mock.acr")
        add_test(NAME gpu-subdivision-obj COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj" --subdivision 2
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/subdivision-obj.acr")
        add_test(NAME gpu-subdivision-cached COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/suzanne.acr" --subdivision 2
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/subdivision-cached.acr")
        set_tests_properties(gpu-subdivision-cached PROPERTIES FIXTURES_REQUIRED cached_scene)
        foreach(FIXTURE IN ITEMS triangle plane grid)
            add_test(NAME gpu-subdivision-${FIXTURE} COMMAND AdaptiveClusterRendering --fixture ${FIXTURE} --subdivision 2
                     --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/subdivision-${FIXTURE}.acr")
        endforeach()
        set_tests_properties(gpu-subdivision-mock gpu-subdivision-obj gpu-subdivision-cached
                            gpu-subdivision-triangle gpu-subdivision-plane gpu-subdivision-grid PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu"
            FAIL_REGULAR_EXPRESSION "Vulkan validation (error|warning)")
        foreach(FIXTURE IN ITEMS triangle plane grid)
            add_test(NAME gpu-displacement-${FIXTURE} COMMAND AdaptiveClusterRendering --fixture ${FIXTURE}
                     --subdivision 2 --displacement 0.25 --heightmap "${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmaps/ramp-u.pgm"
                     --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/displacement-${FIXTURE}.acr")
        endforeach()
        add_test(NAME gpu-displacement-obj COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj"
                 --subdivision 2 --displacement 0.25 --heightmap "${CMAKE_CURRENT_SOURCE_DIR}/assets/heightmaps/ramp-u.pgm"
                 --smoke-roundtrip "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/displacement-obj.acr")
        set_tests_properties(gpu-displacement-triangle gpu-displacement-plane gpu-displacement-grid gpu-displacement-obj PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu"
            FAIL_REGULAR_EXPRESSION "Vulkan validation (error|warning)")
    endif()
endif()
