cmake_minimum_required(VERSION 3.19)
project(PackStacker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets LinguistTools)

include(FetchContent)

FetchContent_Declare(
    zlib
    GIT_REPOSITORY https://github.com/madler/zlib.git
    GIT_TAG        v1.3.1
)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(zlib)

if(TARGET zlibstatic)
    set(ZLIB_LIBRARY zlibstatic)
elseif(TARGET zlib)
    set(ZLIB_LIBRARY zlib)
else()
    message(FATAL_ERROR "ZLIB target not found! FetchContent failed?")
endif()

set(ZLIB_FOUND TRUE)
set(ZLIB_INCLUDE_DIR ${zlib_SOURCE_DIR} ${zlib_BINARY_DIR})
set(ZLIB_LIBRARIES ${ZLIB_LIBRARY})

if(NOT TARGET ZLIB::ZLIB)
    add_library(ZLIB::ZLIB ALIAS ${ZLIB_LIBRARY})
endif()

FetchContent_Declare(
    quazip
    GIT_REPOSITORY https://github.com/stachenov/quazip.git
    GIT_TAG        v1.4
    OVERRIDE_FIND_PACKAGE
)

set(QUAZIP_QT_MAJOR_VERSION 6 CACHE STRING "" FORCE)
set(QUAZIP_USE_QT_ZLIB OFF CACHE BOOL "" FORCE)

set(QUAZIP_BZIP2 OFF CACHE BOOL "" FORCE)

set(QUAZIP_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(quazip)

qt_standard_project_setup()

qt_add_executable(PackStacker
    main.cpp
    main_window.cpp
    main_window.hpp
    main_window.ui
    storage/pack_storage.hpp storage/pack_storage.cpp
)

qt_add_translations(
    TARGETS PackStacker
    TS_FILES PackStacker_ru_RU.ts
)

target_link_libraries(PackStacker
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        QuaZip::QuaZip
        ${ZLIB_LIBRARY}
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
