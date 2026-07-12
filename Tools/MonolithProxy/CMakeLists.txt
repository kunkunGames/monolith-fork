cmake_minimum_required(VERSION 3.16)
project(MonolithProxy LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(MONOLITH_PLUGIN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
file(TO_NATIVE_PATH "${MONOLITH_PLUGIN_ROOT}" MONOLITH_PLUGIN_ROOT_NATIVE)
execute_process(
    COMMAND
        "${Python3_EXECUTABLE}"
        "${MONOLITH_PLUGIN_ROOT}/Scripts/source_generation_hash.py"
        --plugin-root "${MONOLITH_PLUGIN_ROOT}"
        --tool proxy
    RESULT_VARIABLE MONOLITH_PROXY_HASH_RESULT
    OUTPUT_VARIABLE MONOLITH_PROXY_SOURCE_HASH
    ERROR_VARIABLE MONOLITH_PROXY_HASH_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(LENGTH "${MONOLITH_PROXY_SOURCE_HASH}" MONOLITH_PROXY_HASH_LENGTH)
if(NOT MONOLITH_PROXY_HASH_RESULT EQUAL 0 OR
   NOT MONOLITH_PROXY_HASH_LENGTH EQUAL 16 OR
   NOT MONOLITH_PROXY_SOURCE_HASH MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "Could not compute canonical Monolith Proxy source generation: "
        "result=${MONOLITH_PROXY_HASH_RESULT}, "
        "output='${MONOLITH_PROXY_SOURCE_HASH}', "
        "error='${MONOLITH_PROXY_HASH_ERROR}'"
    )
endif()
add_executable(monolith_proxy
    monolith_proxy.cpp
    monolith_proxy_offline.cpp
)
target_include_directories(monolith_proxy PRIVATE ThirdParty)
target_compile_definitions(monolith_proxy PRIVATE SOURCE_HASH="${MONOLITH_PROXY_SOURCE_HASH}")
target_link_libraries(monolith_proxy PRIVATE winhttp bcrypt iphlpapi)
if(MSVC)
    set_property(TARGET monolith_proxy PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(monolith_proxy PRIVATE
        /Brepro
        /experimental:deterministic
        "/pathmap:${MONOLITH_PLUGIN_ROOT_NATIVE}=."
    )
    target_link_options(monolith_proxy PRIVATE /Brepro)
endif()
