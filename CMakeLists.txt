cmake_minimum_required(VERSION 3.16)
project(Detomatix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    webview
    GIT_REPOSITORY https://github.com/webview/webview.git
    GIT_TAG 0.12.0
)
FetchContent_MakeAvailable(webview)

add_executable(Detomatix WIN32
    ./src/main.cpp
    ./src/core.cpp
    ./src/hub.cpp
    ./src/yo.rc
)

set_target_properties(Detomatix PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/detomatix"
)

target_link_libraries(Detomatix PRIVATE webview::core_static psapi winhttp)

if(MINGW)
    target_link_options(Detomatix PRIVATE -static -static-libgcc -static-libstdc++)
endif()

add_custom_command(TARGET Detomatix POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:Detomatix>/assets"
    COMMENT "Copying assets -> $<TARGET_FILE_DIR:Detomatix>/assets"
)