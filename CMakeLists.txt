cmake_minimum_required(VERSION 4.0)

option(PACKSTACKER_BUILD_GUI "Build the Qt Quick application" ON)
option(
    PACKSTACKER_QT_FROM_VCPKG
    "Install Qt through vcpkg instead of using an external SDK"
    OFF
)
include(cmake/Vcpkg.cmake)

project(PackStacker LANGUAGES C CXX)

set(PACKSTACKER_VERSION "dev" CACHE STRING "Version embedded in PackStacker executables")
if(NOT PACKSTACKER_VERSION MATCHES "^(dev|(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*))$")
    message(FATAL_ERROR "PACKSTACKER_VERSION must be dev or a version in X.Y.Z format.")
endif()
configure_file(include/core/version.hpp.in generated/core/version.hpp @ONLY)

if(WIN32 AND NOT MINGW)
    message(
        FATAL_ERROR
        "Windows builds require a MinGW toolchain (GCC or LLVM-MinGW)."
    )
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)
include(GNUInstallDirs)

function(packstacker_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

include(cmake/Dependencies.cmake)
include(cmake/Core.cmake)

if(PACKSTACKER_BUILD_GUI)
    find_package(
        Qt6 6.10 REQUIRED COMPONENTS
        Quick QuickControls2 QuickDialogs2 LinguistTools
    )
    block(SCOPE_FOR VARIABLES PROPAGATE PACKSTACKER_QML_FILES)
        include(cmake/Gui.cmake)
    endblock()
endif()

if(BUILD_TESTING)
    include(cmake/Tests.cmake)
endif()

if(PACKSTACKER_BUILD_GUI)
    set(translation_targets packstacker_gui)
    if(BUILD_TESTING)
        list(
            APPEND
            translation_targets packstacker_gui_tests
            packstacker_qml_smoke_tests
        )
    endif()
    qt_add_translations(
        TARGETS ${translation_targets}
        SOURCE_TARGETS packstacker_gui packstacker_gui_support
        TS_FILES translations/packstacker_en.ts translations/packstacker_ru.ts
        RESOURCE_PREFIX "/i18n"
        MERGE_QT_TRANSLATIONS
        QT_TRANSLATION_CATALOGS qtbase qtdeclarative
    )
endif()

include(cmake/Install.cmake)
