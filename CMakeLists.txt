cmake_minimum_required(VERSION 3.24)
project(HairHarpy VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

# Vulkan: headers + a GLSL → SPIR-V compiler at build time; the loader (vulkan-1.dll) comes with the graphics driver.
# With the Vulkan SDK installed both come from it (and its validation layers serve Debug builds); without it the
# headers are fetched from Khronos' GitHub and shaders are compiled by glslang's release build.
find_package(Vulkan QUIET COMPONENTS glslc)
if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
  set(HM_SHADER_CC ${Vulkan_GLSLC_EXECUTABLE})
  set(HM_SHADER_ARGS --target-env=vulkan1.1 -O)
  set(HM_SHADER_OUT -o)
else()
  FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                       GIT_TAG v1.4.363 GIT_SHALLOW TRUE SOURCE_SUBDIR none)
  FetchContent_MakeAvailable(vulkan_headers)
  if(WIN32)
    FetchContent_Declare(glslang_bin
      URL https://github.com/KhronosGroup/glslang/releases/download/16.5.0/glslang-16.5.0-windows-x86_64-release.zip
      URL_HASH SHA256=06B71298B750268C127F2EE7AE0EF7525E2068120C6C8A3A08B2F58CA6F325CE SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(glslang_bin)
    set(HM_SHADER_CC ${glslang_bin_SOURCE_DIR}/bin/glslang.exe)
  else()   # (Linux: the distribution's glslang — glslang-tools on Debian / Ubuntu)
    find_program(HM_SHADER_CC NAMES glslangValidator glslang REQUIRED)
  endif()
  set(Vulkan_INCLUDE_DIR ${vulkan_headers_SOURCE_DIR}/include CACHE PATH "" FORCE)   # (Qt's WrapVulkanHeaders reads it)
  if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${Vulkan_INCLUDE_DIR})
  endif()
  set(HM_SHADER_ARGS -V --target-env vulkan1.1)
  set(HM_SHADER_OUT -o)
endif()

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Widgets Network)
qt_standard_project_setup()

# ufbx (MIT, one .c/.h pair): FBX import. SOURCE_SUBDIR points nowhere — only the sources are fetched, nothing of its build
FetchContent_Declare(ufbx GIT_REPOSITORY https://github.com/ufbx/ufbx.git GIT_TAG v0.21.5 GIT_SHALLOW TRUE SOURCE_SUBDIR none)
FetchContent_MakeAvailable(ufbx)
add_library(ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(ufbx PUBLIC ${ufbx_SOURCE_DIR})
# Eigen (MPL2, headers only): the UV solvers' sparse factorisations. Only the headers are fetched; EIGEN_MPL2_ONLY keeps
# out anything under another licence
FetchContent_Declare(eigen URL https://gitlab.com/libeigen/eigen/-/archive/3.4.0/eigen-3.4.0.tar.gz SOURCE_SUBDIR none)
FetchContent_MakeAvailable(eigen)
add_library(eigen INTERFACE)
target_include_directories(eigen SYSTEM INTERFACE ${eigen_SOURCE_DIR})
target_compile_definitions(eigen INTERFACE EIGEN_MPL2_ONLY)

set(HM_CORE_SOURCES src/scene/Scene.cpp src/scene/CurveMath.cpp src/scene/Walls.cpp src/scene/Cocoons.cpp src/scene/HairState.cpp src/scene/Display.cpp src/scene/Network.cpp src/scene/Convert.cpp src/scene/AutoFix.cpp src/scene/Edit.cpp src/scene/Cut.cpp src/scene/AnchorOps.cpp src/scene/Crown.cpp src/io/ProjectIO.cpp src/io/MeshImport.cpp src/scene/Refs.cpp src/scene/Topo.cpp src/scene/TopoEdit.cpp src/scene/Braid.cpp src/scene/Tubes.cpp src/view/Camera.cpp)

file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
if(WIN32)
  set(HM_PLATFORM_SOURCES resources/app.rc)   # (app.rc: the exe's icon; on Linux the icon is the .desktop file's)
endif()
qt_add_executable(HairHarpy WIN32 ${APP_SOURCES} ${HM_PLATFORM_SOURCES})
target_include_directories(HairHarpy PRIVATE src)
target_link_libraries(HairHarpy PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Vulkan::Headers ufbx eigen)
if(WIN32)
  target_link_libraries(HairHarpy PRIVATE winmm)   # (PlaySound: the achievements' chime, the confetti's arpeggio)
endif()   # (Network: the live bridge to Blender / Maya on 127.0.0.1)

# console check of the core (no GPU): hm_selftest <project.hairmesh>
qt_add_executable(hm_selftest tools/selftest.cpp ${HM_CORE_SOURCES})
target_include_directories(hm_selftest PRIVATE src)
target_link_libraries(hm_selftest PRIVATE Qt6::Core Qt6::Gui ufbx)

# console check of the UV solvers: hm_uvtest <project.hairmesh> — every mesh's shells unfolded anew along their UV
# borders, old way against new: flipped triangles, stretch, time
qt_add_executable(hm_uvtest tools/uvtest.cpp ${HM_CORE_SOURCES} src/scene/Uv.cpp src/scene/UvSolve.cpp src/scene/UvPack.cpp src/scene/UvFace.cpp src/scene/UvAlign.cpp src/scene/UvCheck.cpp)
target_include_directories(hm_uvtest PRIVATE src)
target_link_libraries(hm_uvtest PRIVATE Qt6::Core Qt6::Gui ufbx eigen)

foreach(t HairHarpy hm_selftest hm_uvtest)
  if(MSVC)
    target_compile_options(${t} PRIVATE /W3 /permissive- /utf-8)
    target_compile_definitions(${t} PRIVATE NOMINMAX _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
  endif()
endforeach()

# GLSL -> SPIR-V at build time, embedded as Qt resources (:/shaders/<name>.spv)
set(SHADER_SOURCES shaders/mesh.vert shaders/mesh.frag shaders/line.vert shaders/line.frag
                   shaders/point.vert shaders/point.frag shaders/mesh_tex.vert shaders/mesh_tex.frag)
set(SPV_FILES)
foreach(src ${SHADER_SOURCES})
  get_filename_component(name ${src} NAME)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${name}.spv)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
    COMMAND ${HM_SHADER_CC} ${HM_SHADER_ARGS} ${CMAKE_CURRENT_SOURCE_DIR}/${src} ${HM_SHADER_OUT} ${out}
    DEPENDS ${src} VERBATIM)
  list(APPEND SPV_FILES ${out})
endforeach()
set_source_files_properties(${SPV_FILES} PROPERTIES GENERATED TRUE)
qt_add_resources(HairHarpy shaders PREFIX / BASE ${CMAKE_CURRENT_BINARY_DIR} FILES ${SPV_FILES})
# the pictures the HTML version embedded (tool icons, Mara, achievements, the puzzle): :/img/<name>.<ext>
file(GLOB HM_IMAGES CONFIGURE_DEPENDS resources/images/*)
qt_add_resources(HairHarpy images PREFIX /img BASE resources/images FILES ${HM_IMAGES})
# the bridge scripts (Blender add-on, Maya script) — saved from the Bridge panel
file(GLOB HM_BRIDGE CONFIGURE_DEPENDS resources/bridge/*)
qt_add_resources(HairHarpy bridge PREFIX /bridge BASE resources/bridge FILES ${HM_BRIDGE})
# the FAQ (its sections, the search's synonyms and stop words — taken from the HTML version)
qt_add_resources(HairHarpy faq PREFIX /faq BASE resources/faq FILES resources/faq/faq.json)

# windeployqt next to the exe after every build (Qt DLLs + platform plugin). Vulkan only: no D3D / OpenGL fallbacks,
# no network (TLS, network information) — build.ps1 -Package uses the same flags
set(HM_DEPLOY_FLAGS --no-translations --no-system-d3d-compiler --no-system-dxc-compiler --no-opengl-sw
                    --skip-plugin-types tls,networkinformation,generic)
if(WIN32)   # (Linux: the system's Qt, or the one installer/linux/build-linux.sh bundles)
  get_target_property(QMAKE_EXE Qt6::qmake IMPORTED_LOCATION)
  get_filename_component(QT_BIN_DIR ${QMAKE_EXE} DIRECTORY)
  add_custom_command(TARGET HairHarpy POST_BUILD
    COMMAND ${QT_BIN_DIR}/windeployqt.exe ${HM_DEPLOY_FLAGS} $<TARGET_FILE:HairHarpy>
    VERBATIM)
endif()

# the session recording (the session's record, its annotations, the crash dumps): on in every build, the published one
# too (TRUST.md describes it); -DHM_DEVTOOLS=OFF builds the program without it
option(HM_DEVTOOLS "The session recording, its annotations and the crash dumps" ON)
if(HM_DEVTOOLS)
  target_compile_definitions(HairHarpy PRIVATE HM_DEVTOOLS=1)
  if(MSVC)   # (a crash: the dump written into the session folder; the pdb and the map kept in the build to read it)
    target_compile_options(HairHarpy PRIVATE /Zi)
    target_link_options(HairHarpy PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP)
    target_link_libraries(HairHarpy PRIVATE dbghelp)
  endif()
endif()
