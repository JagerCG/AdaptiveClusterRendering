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
foreach(SHADER IN ITEMS mesh.vert mesh.frag bounds.vert bounds.frag)
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
    src/ClusterBuilder.cpp
    src/ClusterBuilder.h
    src/MeshSource.cpp
    src/MeshSource.h
    src/ObjLoader.cpp
    src/ObjLoader.h
    src/SceneData.cpp
    src/SceneData.h
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
    src/VulkanRenderer.h
    src/SceneGpuData.h
    ${SHADER_SOURCES}
    ${COMPILED_SHADERS}
)
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT AdaptiveClusterRendering)
target_include_directories(AdaptiveClusterRendering PRIVATE "${SHADER_OUTPUT_DIR}")
target_compile_features(AdaptiveClusterRendering PRIVATE cxx_std_17)
set_target_properties(AdaptiveClusterRendering PROPERTIES CXX_EXTENSIONS OFF)
target_compile_definitions(AdaptiveClusterRendering PRIVATE GLFW_INCLUDE_NONE)
target_link_libraries(AdaptiveClusterRendering PRIVATE acr_scene Vulkan::Vulkan glfw imgui)
if(WIN32)
    target_link_libraries(AdaptiveClusterRendering PRIVATE comdlg32)
endif()

file(GENERATE
    OUTPUT "$<TARGET_FILE_DIR:AdaptiveClusterRendering>/assets/suzanne.obj"
    INPUT "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj"
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
    add_executable(scene_tests tests/SceneTests.cpp)
    target_link_libraries(scene_tests PRIVATE acr_scene)
    foreach(CASE IN ITEMS mock roundtrip real-model invalid-scene corrupt-file)
        add_test(NAME scene-${CASE} COMMAND scene_tests ${CASE} "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj"
                 "${CMAKE_CURRENT_BINARY_DIR}/test-data/${CASE}")
    endforeach()
    if(ACR_ENABLE_GPU_TESTS)
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu")
        add_test(NAME scene-preprocess COMMAND AdaptiveClusterRendering --preprocess
                 "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj" "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/suzanne.acr")
        set_tests_properties(scene-preprocess PROPERTIES FIXTURES_SETUP cached_scene)
        add_test(NAME gpu-mock COMMAND AdaptiveClusterRendering --mock --smoke-test)
        add_test(NAME gpu-obj COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_SOURCE_DIR}/assets/suzanne.obj" --smoke-test)
        add_test(NAME gpu-cached COMMAND AdaptiveClusterRendering "${CMAKE_CURRENT_BINARY_DIR}/test-data/gpu/suzanne.acr" --smoke-test)
        set_tests_properties(gpu-cached PROPERTIES FIXTURES_REQUIRED cached_scene)
        set_tests_properties(gpu-mock gpu-obj gpu-cached PROPERTIES
            TIMEOUT 60 RUN_SERIAL TRUE LABELS "gpu"
            FAIL_REGULAR_EXPRESSION "Vulkan validation (error|warning)")
    endif()
endif()
