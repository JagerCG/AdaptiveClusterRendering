# Save a complete real viewer state, relaunch using just the preset and compare the GPU result.
set(view "${DIR}/replay.acrview")
function(run_view)
    execute_process(COMMAND "${VIEWER}" ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 55)
    if(NOT rc EQUAL 0 OR "${out}${err}" MATCHES "Vulkan validation (error|warning)")
        message(FATAL_ERROR "Viewer failed (${rc}): ${out}\n${err}")
    endif()
endfunction()
get_filename_component(viewerDir "${VIEWER}" DIRECTORY)
run_view(--view-config "${viewerDir}/assets/views/m2-grid-mound.acrview" --verify-gpu
    --benchmark "${DIR}/bundled-mound.json" --benchmark-warmup 1 --benchmark-samples 2)
file(READ "${DIR}/bundled-mound.json" mound)
string(JSON source GET "${mound}" counts source_triangles)
string(JSON drawn GET "${mound}" counts drawn_triangles)
string(JSON segments GET "${mound}" counts segments_per_edge)
string(JSON displaced GET "${mound}" flags displacement_enabled)
string(JSON heightMap GET "${mound}" labels height_name)
string(JSON amplitude GET "${mound}" values amplitude)
if(NOT source EQUAL 8 OR NOT drawn EQUAL 128 OR NOT segments EQUAL 4 OR NOT displaced
    OR NOT heightMap STREQUAL "Mound" OR amplitude LESS 0.79999 OR amplitude GREATER 0.80001)
    message(FATAL_ERROR "Bundled blue mound did not reproduce the featured grid and height displacement")
endif()
run_view("${ASSET}" --selection first-last --cluster-colors --shadows --depth-view --subdivision 2 --displacement 0.08 --hide-ui
    --save-view "${view}" --benchmark "${DIR}/live-view.json" --benchmark-warmup 1 --benchmark-samples 3)
run_view(--view-config "${view}" --verify-gpu --benchmark "${DIR}/restored-view.json")
file(READ "${DIR}/live-view.json" live)
file(READ "${DIR}/restored-view.json" replay)
foreach(key selected_cluster_ids view_projection_column_major)
    string(JSON a GET "${live}" ${key})
    string(JSON b GET "${replay}" ${key})
    if(NOT a STREQUAL b)
        message(FATAL_ERROR "Restoring ${key} changed the view")
    endif()
endforeach()
foreach(key selected_triangles selected_clusters drawn_triangles generated_triangles shadow_triangles width_pixels height_pixels)
    string(JSON a GET "${live}" counts ${key})
    string(JSON b GET "${replay}" counts ${key})
    if(NOT a EQUAL b OR b LESS_EQUAL 0)
        message(FATAL_ERROR "Incorrect restored ${key}")
    endif()
endforeach()
string(JSON selectCount GET "${replay}" counts selected_triangles)
string(JSON drawCount GET "${replay}" counts drawn_triangles)
math(EXPR expected "4 * ${selectCount}")
if(NOT expected EQUAL drawCount OR selectCount GREATER_EQUAL 3936)
    message(FATAL_ERROR "Fixed-list GPU subdivision did not feed the draw")
endif()
foreach(key cluster_colors camera_depth_available camera_depth_solid verification_readback_excluded)
    string(JSON enabled GET "${replay}" flags ${key})
    if(NOT enabled)
        message(FATAL_ERROR "Missing restored ${key}")
    endif()
endforeach()
string(JSON timing GET "${replay}" flags gpu_timestamps_supported)
foreach(metric cpu_frame gpu_main gpu_shadow)
    if(metric MATCHES "^gpu_" AND NOT timing)
        continue()
    endif()
    string(JSON count GET "${replay}" summary_ms ${metric} count)
    if(NOT count EQUAL 3)
        message(FATAL_ERROR "Missing measured ${metric} samples")
    endif()
endforeach()
# Replay a saved v3 view from the full M2 renderer. Its camera GPU choices have retired.
set(oldView "${DIR}/legacy-view.acrview")
file(WRITE "${oldView}" [=[ACR_VIEW 3
source 9 ""
window 640 480 1
camera 0 0 1.5
lod 0 1 96 1
draw 1 0 0 -1 0 0 0
light 1 .002 .6 .8 1
preview 0 0
geometry 1 2 1 .08
height 0 ""
sample 1 3
culling 3 1 1 1
shadow_cull 1 .3
]=])
run_view(--view-config "${oldView}" --benchmark "${DIR}/legacy-view.json")
file(READ "${DIR}/legacy-view.json" old)
string(JSON cameraTriangles GET "${old}" counts drawn_triangles)
string(JSON lightTriangles GET "${old}" counts shadow_triangles)
if(NOT cameraTriangles EQUAL 32 OR NOT lightTriangles EQUAL 16)
    message(FATAL_ERROR "Incorrect fixed-selection or shadow count after legacy replay")
endif()
file(READ "${view}" valid)
set(invalid "${DIR}/invalid-view.acrview")
file(WRITE "${invalid}" "${valid}unexpected-field\n")
execute_process(COMMAND "${VIEWER}" --view-config "${invalid}" --save-view "${DIR}/invalid-replay-output.acrview"
    RESULT_VARIABLE rc ERROR_VARIABLE err TIMEOUT 20)
if(rc EQUAL 0 OR NOT err MATCHES "Malformed view configuration")
    message(FATAL_ERROR "An invalid preset must be rejected before drawing")
endif()
message(STATUS "Saved view reproduced the camera, ordered cluster IDs, draw counts and enabled passes.")
