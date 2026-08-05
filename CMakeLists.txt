cmake_minimum_required(VERSION 3.22)
project(lite_llm LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(LITE_LLM_USE_MOCK_RUNTIME "Build the bundled DeepSeek-backed runtime" ON)
option(LITE_LLM_BUILD_SERVER "Build the standalone lite-server module" ON)
option(LITE_LLM_BUILD_TESTS "Build lite-server tests" ON)
set(LITE_LLM_RUNTIME_LIBRARY "" CACHE FILEPATH
    "Path to liblite_llm_runtime.so when the mock runtime is disabled")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(LITE_LLM_RUNTIME_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/runtime)

if(LITE_LLM_USE_MOCK_RUNTIME)
    include(FetchContent)
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
    set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
    FetchContent_Declare(mbedtls
        URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.5/mbedtls-3.6.5.tar.bz2
        URL_HASH SHA256=4a11f1777bb95bf4ad96721cac945a26e04bf19f57d905f241fe77ebeddf46d8
    )
    FetchContent_MakeAvailable(mbedtls)

    add_library(lite_llm_runtime SHARED runtime/mock/lite_llm_mock.cpp)
    target_include_directories(lite_llm_runtime PUBLIC ${LITE_LLM_RUNTIME_ROOT}/include)
    target_link_libraries(lite_llm_runtime PRIVATE mbedtls)
    target_compile_options(lite_llm_runtime PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(lite_llm_runtime PROPERTIES
        PREFIX "lib"
        OUTPUT_NAME lite_llm_runtime
        SUFFIX ".so"
    )
    if(CMAKE_SYSTEM_NAME STREQUAL "OHOS")
        target_link_libraries(lite_llm_runtime PRIVATE libhilog_ndk.z.so)
    endif()
else()
    if(LITE_LLM_RUNTIME_LIBRARY STREQUAL "")
        if(NOT DEFINED OHOS_ARCH)
            message(FATAL_ERROR
                "Set LITE_LLM_RUNTIME_LIBRARY or OHOS_ARCH when the mock runtime is disabled")
        endif()
        set(LITE_LLM_RUNTIME_LIBRARY
            ${LITE_LLM_RUNTIME_ROOT}/lib/${OHOS_ARCH}/liblite_llm_runtime.so)
    endif()
    if(NOT EXISTS ${LITE_LLM_RUNTIME_LIBRARY})
        message(FATAL_ERROR "Inference runtime library not found: ${LITE_LLM_RUNTIME_LIBRARY}")
    endif()
    add_library(lite_llm_runtime SHARED IMPORTED GLOBAL)
    set_target_properties(lite_llm_runtime PROPERTIES
        IMPORTED_LOCATION ${LITE_LLM_RUNTIME_LIBRARY}
        IMPORTED_NO_SONAME TRUE
        INTERFACE_INCLUDE_DIRECTORIES ${LITE_LLM_RUNTIME_ROOT}/include
    )
endif()

find_package(Threads REQUIRED)

add_library(lite_llm SHARED
    src/json_value.cpp
    src/lite_llm.cpp
)
target_include_directories(lite_llm
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${LITE_LLM_RUNTIME_ROOT}/include
)
target_link_libraries(lite_llm PRIVATE lite_llm_runtime Threads::Threads)
target_compile_options(lite_llm PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(lite_llm PROPERTIES
    PREFIX "lib"
    OUTPUT_NAME lite_llm
    SUFFIX ".so"
    BUILD_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>"
    INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path,$ORIGIN>"
)
if(NOT LITE_LLM_USE_MOCK_RUNTIME)
    add_custom_command(TARGET lite_llm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${LITE_LLM_RUNTIME_LIBRARY}
            ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/liblite_llm_runtime.so
    )
endif()

if(LITE_LLM_BUILD_SERVER)
    add_subdirectory(lite_server)
endif()

install(TARGETS lite_llm
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION lib
)
install(FILES include/lite_llm.h DESTINATION include)
if(LITE_LLM_USE_MOCK_RUNTIME)
    install(TARGETS lite_llm_runtime
        LIBRARY DESTINATION lib
        RUNTIME DESTINATION lib
    )
else()
    install(FILES ${LITE_LLM_RUNTIME_LIBRARY} DESTINATION lib)
endif()
