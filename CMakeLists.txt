cmake_minimum_required(VERSION 3.20)

project(Straw_SamExtensions VERSION 0.1.86 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED F4SE_SDK)
    set(F4SE_SDK "$ENV{F4SE_SDK}")
endif()

if(NOT F4SE_SDK)
    message(FATAL_ERROR "F4SE_SDK is not set. Point it at the Fallout 4 src folder or extracted F4SE SDK root.")
endif()

file(TO_CMAKE_PATH "${F4SE_SDK}" F4SE_SDK)

set(F4SE_INCLUDE_ROOT "")
set(F4SE_PLUGIN_API "")

if(EXISTS "${F4SE_SDK}/f4se/PluginAPI.h")
    set(F4SE_INCLUDE_ROOT "${F4SE_SDK}")
    set(F4SE_PLUGIN_API "${F4SE_SDK}/f4se/PluginAPI.h")
elseif(EXISTS "${F4SE_SDK}/PluginAPI.h")
    get_filename_component(F4SE_INCLUDE_ROOT "${F4SE_SDK}" DIRECTORY)
    set(F4SE_PLUGIN_API "${F4SE_SDK}/PluginAPI.h")
else()
    file(GLOB_RECURSE F4SE_PLUGIN_API_CANDIDATES CONFIGURE_DEPENDS "${F4SE_SDK}/PluginAPI.h")
    foreach(candidate IN LISTS F4SE_PLUGIN_API_CANDIDATES)
        file(TO_CMAKE_PATH "${candidate}" candidate_norm)
        if(candidate_norm MATCHES "/f4se/PluginAPI\\.h$")
            get_filename_component(f4se_dir "${candidate_norm}" DIRECTORY)
            get_filename_component(F4SE_INCLUDE_ROOT "${f4se_dir}" DIRECTORY)
            set(F4SE_PLUGIN_API "${candidate_norm}")
            break()
        endif()
    endforeach()
endif()

if(NOT F4SE_PLUGIN_API)
    message(FATAL_ERROR "Could not find PluginAPI.h below F4SE_SDK='${F4SE_SDK}'. Expected a path like <sdk>/f4se/PluginAPI.h.")
endif()

get_filename_component(F4SE_SRC_PARENT "${F4SE_INCLUDE_ROOT}" DIRECTORY)
set(COMMON_INCLUDE_ROOT_CANDIDATE "${F4SE_SRC_PARENT}/common")

set(COMMON_INCLUDE_ROOT "")
if(EXISTS "${COMMON_INCLUDE_ROOT_CANDIDATE}/common/ITypes.h")
    set(COMMON_INCLUDE_ROOT "${COMMON_INCLUDE_ROOT_CANDIDATE}")
elseif(EXISTS "${F4SE_SDK}/common/ITypes.h")
    set(COMMON_INCLUDE_ROOT "${F4SE_SDK}")
elseif(EXISTS "${F4SE_SDK}/ITypes.h")
    get_filename_component(COMMON_INCLUDE_ROOT "${F4SE_SDK}" DIRECTORY)
else()
    file(GLOB_RECURSE ITYPES_CANDIDATES CONFIGURE_DEPENDS "${F4SE_SDK}/ITypes.h")
    foreach(candidate IN LISTS ITYPES_CANDIDATES)
        file(TO_CMAKE_PATH "${candidate}" candidate_norm)
        if(candidate_norm MATCHES "/common/ITypes\\.h$")
            get_filename_component(common_dir "${candidate_norm}" DIRECTORY)
            get_filename_component(COMMON_INCLUDE_ROOT "${common_dir}" DIRECTORY)
            break()
        endif()
    endforeach()
endif()

if(NOT COMMON_INCLUDE_ROOT)
    message(FATAL_ERROR "Could not find common/ITypes.h. Expected it near '${COMMON_INCLUDE_ROOT_CANDIDATE}/common/ITypes.h'.")
endif()

message(STATUS "F4SE_SDK candidate: ${F4SE_SDK}")
message(STATUS "F4SE PluginAPI:   ${F4SE_PLUGIN_API}")
message(STATUS "F4SE include root:${F4SE_INCLUDE_ROOT}")
message(STATUS "Common include root:${COMMON_INCLUDE_ROOT}")

set(REQUIRED_SDK_SOURCES)
foreach(candidate
    "${F4SE_INCLUDE_ROOT}/f4se_common/Relocation.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/GameAPI.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/GameTypes.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/GameUtilities.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/GameExtraData.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/GameForms.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/PapyrusInterfaces.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/PapyrusVM.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/PapyrusValue.cpp"
    "${F4SE_INCLUDE_ROOT}/f4se/PapyrusArgs.cpp"
)
    if(EXISTS "${candidate}")
        list(APPEND REQUIRED_SDK_SOURCES "${candidate}")
        message(STATUS "Adding SDK source: ${candidate}")
    else()
        message(FATAL_ERROR "Required SDK source missing: ${candidate}")
    endif()
endforeach()

add_library(Straw_SamExtensions SHARED
    src/Plugin.cpp
    ${REQUIRED_SDK_SOURCES}
)

target_include_directories(Straw_SamExtensions PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${COMMON_INCLUDE_ROOT}"
    "${COMMON_INCLUDE_ROOT}/common"
    "${F4SE_INCLUDE_ROOT}"
    "${F4SE_INCLUDE_ROOT}/f4se"
    "${F4SE_INCLUDE_ROOT}/f4se_common"
)


target_link_libraries(Straw_SamExtensions PRIVATE Psapi.lib)

target_compile_definitions(Straw_SamExtensions PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

if(MSVC)
    target_compile_options(Straw_SamExtensions PRIVATE /EHsc /W3 /FI"${CMAKE_CURRENT_SOURCE_DIR}/src/CompatPch.h")
    set_property(TARGET Straw_SamExtensions PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set_target_properties(Straw_SamExtensions PROPERTIES
    OUTPUT_NAME "Straw_SamExtensions"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out"
)
