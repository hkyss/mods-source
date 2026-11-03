cmake_minimum_required(VERSION 3.24)

project(hkyssSkyrim LANGUAGES CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(
	CommonLibSSE
	GIT_REPOSITORY https://github.com/alandtse/CommonLibVR.git
	GIT_TAG v4.39.3
	GIT_SHALLOW TRUE
	GIT_SUBMODULES ""
)
FetchContent_MakeAvailable(CommonLibSSE)
include("${commonlibsse_SOURCE_DIR}/cmake/CommonLibSSE.cmake")
add_compile_definitions(NOMINMAX)

file(GLOB mods LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/*/src")
foreach(mod IN LISTS mods)
	add_subdirectory("${mod}")
endforeach()
