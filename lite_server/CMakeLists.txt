cmake_minimum_required(VERSION 3.14)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(lite_server LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    option(LITE_LLM_BUILD_TESTS "Build lite-server tests" ON)
    set(LITE_LLM_INCLUDE_DIR "" CACHE PATH "Directory containing lite_llm.h")
    set(LITE_LLM_LIBRARY "" CACHE FILEPATH "Path to liblite_llm.so")

    if(LITE_LLM_INCLUDE_DIR STREQUAL "" OR LITE_LLM_LIBRARY STREQUAL "")
        message(FATAL_ERROR
            "Standalone lite_server requires LITE_LLM_INCLUDE_DIR and LITE_LLM_LIBRARY")
    endif()
    add_library(lite_llm_external SHARED IMPORTED GLOBAL)
    set_target_properties(lite_llm_external PROPERTIES
        IMPORTED_LOCATION ${LITE_LLM_LIBRARY}
        INTERFACE_INCLUDE_DIRECTORIES ${LITE_LLM_INCLUDE_DIR}
    )
    set(LITE_SERVER_LLM_TARGET lite_llm_external)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
else()
    set(LITE_SERVER_LLM_TARGET lite_llm)
endif()

add_library(lite_server_core STATIC
    src/json_value.cpp
    src/protocol_codec.cpp
    src/reverse_client.cpp
)
target_include_directories(lite_server_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lite_server_core PUBLIC ${LITE_SERVER_LLM_TARGET})
target_compile_options(lite_server_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(lite-server src/main.cpp)
target_link_libraries(lite-server PRIVATE lite_server_core)
target_compile_options(lite-server PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(lite-server PROPERTIES
    BUILD_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path/../lib,$ORIGIN/../lib>"
    INSTALL_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path/../lib,$ORIGIN/../lib>"
)

if(LITE_LLM_BUILD_TESTS)
    add_executable(lite-server-tests tests/lite_server_tests.cpp)
    target_link_libraries(lite-server-tests PRIVATE lite_server_core)
    target_compile_options(lite-server-tests PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(lite-server-tests PROPERTIES
        BUILD_RPATH "$<IF:$<PLATFORM_ID:Darwin>,@loader_path/../lib,$ORIGIN/../lib>"
    )
endif()

install(TARGETS lite-server RUNTIME DESTINATION bin)
