cmake_minimum_required(VERSION 4.0)
project(PackStacker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10 REQUIRED COMPONENTS Core Widgets LinguistTools)
qt_standard_project_setup()

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.12.0
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    libzip
    GIT_REPOSITORY https://github.com/nih-at/libzip.git
    GIT_TAG        v1.11.4
    GIT_SHALLOW    TRUE
)
set(BUILD_SHARED_LIBS  OFF CACHE BOOL "" FORCE)
set(LIBZIP_DO_INSTALL  OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS        OFF CACHE BOOL "" FORCE)
set(BUILD_REGRESS      OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(BUILD_DOC          OFF CACHE BOOL "" FORCE)
set(ENABLE_BZIP2       OFF CACHE BOOL "" FORCE)
set(ENABLE_LZMA        OFF CACHE BOOL "" FORCE)
set(ENABLE_ZSTD        OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENSSL     OFF CACHE BOOL "" FORCE)
set(ENABLE_GNUTLS      OFF CACHE BOOL "" FORCE)
set(ENABLE_MBEDTLS     OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json libzip)

qt_add_executable(PackStacker
    WIN32
    src/main.cpp
    src/gui/main_window.cpp
    include/gui/main_window.hpp
    forms/main_window.ui
    src/storage/pack_storage.cpp
    include/storage/pack_storage.hpp
)

set_target_properties(PackStacker PROPERTIES
    AUTOUIC_SEARCH_PATHS ${CMAKE_CURRENT_SOURCE_DIR}/forms
)

target_include_directories(PackStacker PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(PackStacker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(PackStacker
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        nlohmann_json::nlohmann_json
        libzip::zip
)

target_compile_options(PackStacker PRIVATE -Wall -Wextra -Wpedantic -Werror)

qt_add_translations(
    TARGETS PackStacker
    TS_FILES i18n/PackStacker_ru_RU.ts
)

include(GNUInstallDirs)
install(TARGETS PackStacker
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

qt_generate_deploy_app_script(
    TARGET PackStacker
    OUTPUT_SCRIPT deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
)
install(SCRIPT ${deploy_script})
