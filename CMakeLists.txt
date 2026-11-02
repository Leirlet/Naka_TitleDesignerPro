cmake_minimum_required(VERSION 3.24)
project(NAKATitleDesignerPro LANGUAGES CXX RC)

set(PR_SDK "C:/tools/Premiere Pro 26.0 C++ SDK Windows/Premiere Pro 26.0 C++ SDK" CACHE PATH "Premiere Pro C++ SDK root")
set(PR_SDK_HEADERS "${PR_SDK}/Examples/Headers")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: nothing extra to install next to Premiere.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/utf-8> $<$<COMPILE_LANGUAGE:CXX>:/W3>)
add_compile_definitions(UNICODE _UNICODE)

set(OUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")

# ---------------------------------------------------------------- 3D shaders (fxc from the Windows SDK)
file(GLOB FXC_HINTS "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64")
list(SORT FXC_HINTS ORDER DESCENDING)
find_program(FXC fxc HINTS ${FXC_HINTS} REQUIRED)
set(SHADER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/common/tdp_3d.hlsl")
set(SURFACE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/common/tdp_surface.hlsl")
set(LIGHT_INC "${CMAKE_CURRENT_SOURCE_DIR}/src/common/tdp_light.hlsli")
set(SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(SHADER_HEADERS
	"${SHADER_DIR}/tdp_3d_vs.h" "${SHADER_DIR}/tdp_3d_ps.h"
	"${SHADER_DIR}/tdp_surf_vs.h" "${SHADER_DIR}/tdp_surf_seed.h" "${SHADER_DIR}/tdp_surf_jump.h" "${SHADER_DIR}/tdp_surf_height.h" "${SHADER_DIR}/tdp_surf_blur.h" "${SHADER_DIR}/tdp_surf_shade.h")
add_custom_command(
	OUTPUT ${SHADER_HEADERS}
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_DIR}"
	COMMAND "${FXC}" /nologo /O3 /T vs_4_0 /E VSMain /Vn g_vs3d /Fh "${SHADER_DIR}/tdp_3d_vs.h" "${SHADER_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T ps_4_0 /E PSMain /Vn g_ps3d /Fh "${SHADER_DIR}/tdp_3d_ps.h" "${SHADER_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T vs_4_0 /E VSFull /Vn g_vsSurf /Fh "${SHADER_DIR}/tdp_surf_vs.h" "${SURFACE_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T ps_4_0 /E PSSeed /Vn g_psSeed /Fh "${SHADER_DIR}/tdp_surf_seed.h" "${SURFACE_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T ps_4_0 /E PSJump /Vn g_psJump /Fh "${SHADER_DIR}/tdp_surf_jump.h" "${SURFACE_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T ps_4_0 /E PSHeight /Vn g_psHeight /Fh "${SHADER_DIR}/tdp_surf_height.h" "${SURFACE_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T ps_4_0 /E PSBlur /Vn g_psBlur /Fh "${SHADER_DIR}/tdp_surf_blur.h" "${SURFACE_SRC}"
	COMMAND "${FXC}" /nologo /O3 /T ps_4_0 /E PSShade /Vn g_psShade /Fh "${SHADER_DIR}/tdp_surf_shade.h" "${SURFACE_SRC}"
	DEPENDS "${SHADER_SRC}" "${SURFACE_SRC}" "${LIGHT_INC}"
	COMMENT "Compiling shaders"
	VERBATIM)

# ---------------------------------------------------------------- shared renderer
add_library(tdp_core STATIC
	src/common/tdp_json.cpp
	src/common/tdp_doc.cpp
	src/common/tdp_media.cpp
	src/common/tdp_render.cpp
	${SHADER_HEADERS})
target_include_directories(tdp_core PUBLIC src/common)
target_include_directories(tdp_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
target_compile_definitions(tdp_core PRIVATE NOMINMAX)
target_link_libraries(tdp_core PUBLIC d3d11 dxgi d2d1 dwrite dxguid windowscodecs shell32 ole32)

# ---------------------------------------------------------------- importer implementation DLL
add_library(tdp_importer_impl SHARED src/importer/tdp_importer.cpp)
target_include_directories(tdp_importer_impl PRIVATE "${PR_SDK_HEADERS}")
target_compile_definitions(tdp_importer_impl PRIVATE PRWIN_ENV MSWindows)
target_link_libraries(tdp_importer_impl PRIVATE tdp_core user32 gdi32 shell32 ole32)
set_target_properties(tdp_importer_impl PROPERTIES
	OUTPUT_NAME "NAKAfx_TitleDesignerPro_impl"
	RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")

# ---------------------------------------------------------------- loader .prm (PiPL)
set(PIPL_R   "${CMAKE_CURRENT_SOURCE_DIR}/src/importer/TdpImporterPiPL.r")
set(PIPL_RR  "${CMAKE_CURRENT_BINARY_DIR}/TdpImporterPiPL.rr")
set(PIPL_RCP "${CMAKE_CURRENT_BINARY_DIR}/TdpImporterPiPL.rcp")
add_custom_command(
	OUTPUT "${PIPL_RCP}"
	COMMAND cl /nologo /D PRWIN_ENV /I "${PR_SDK_HEADERS}" /EP /P "/Fi${PIPL_RR}" "${PIPL_R}"
	COMMAND "${PR_SDK}/Examples/Resources/Win/CnvtPiPL.exe" "${PIPL_RR}" "${PIPL_RCP}"
	DEPENDS "${PIPL_R}"
	COMMENT "Converting PiPL"
	VERBATIM)
set_source_files_properties("${PIPL_RCP}" PROPERTIES GENERATED TRUE HEADER_FILE_ONLY TRUE)
set_source_files_properties(src/importer/tdp_loader.rc PROPERTIES OBJECT_DEPENDS "${PIPL_RCP}")

add_library(tdp_loader MODULE
	src/importer/tdp_loader.cpp
	src/importer/tdp_loader.rc
	"${PIPL_RCP}")
target_include_directories(tdp_loader PRIVATE "${PR_SDK_HEADERS}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(tdp_loader PRIVATE PRWIN_ENV MSWindows)
target_link_libraries(tdp_loader PRIVATE shell32 ole32)
set_target_properties(tdp_loader PROPERTIES
	OUTPUT_NAME "NAKAfx_TitleDesignerPro"
	PREFIX ""
	SUFFIX ".prm"
	LIBRARY_OUTPUT_DIRECTORY "${OUT_DIR}")

# ---------------------------------------------------------------- designer
set(WEBVIEW2_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/webview2/pkg/build/native")
add_executable(tdp_designer WIN32 src/designer/designer.cpp)
target_include_directories(tdp_designer PRIVATE "${WEBVIEW2_SDK}/include")
target_compile_definitions(tdp_designer PRIVATE NOMINMAX)
target_link_libraries(tdp_designer PRIVATE tdp_core user32 gdi32 shell32 ole32 advapi32 version
	"${WEBVIEW2_SDK}/x64/WebView2LoaderStatic.lib")
set_target_properties(tdp_designer PROPERTIES
	OUTPUT_NAME "NAKA_TitleDesignerPro"
	RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
# The HTML UI is served from <exe dir>/ui via a WebView2 virtual host (copied on every build).
add_custom_target(tdp_ui ALL
	COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/designer/ui" "${OUT_DIR}/ui"
	VERBATIM)
add_dependencies(tdp_designer tdp_ui)

# ---------------------------------------------------------------- renderer check tool
add_executable(tdp_render_test tools/render_test.cpp)
target_link_libraries(tdp_render_test PRIVATE tdp_core)
set_target_properties(tdp_render_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")

# Calls imInit of the deployed impl DLL (checks that the loader path works outside Premiere).
add_executable(tdp_init_test tools/init_test.cpp)
target_link_libraries(tdp_init_test PRIVATE shell32 ole32)
set_target_properties(tdp_init_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${OUT_DIR}")
