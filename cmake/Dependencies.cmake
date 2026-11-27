if(NOT VCPKG_TOOLCHAIN)
    message(FATAL_ERROR "Dependencies require the vcpkg CMake toolchain. Use a fresh build directory.")
endif()

find_package(CLI11 2.7.2 CONFIG REQUIRED)
find_package(nlohmann_json 3.12.0 CONFIG REQUIRED)
find_package(ZLIB 1.3.2 REQUIRED)
find_package(libzip 1.11.4 CONFIG REQUIRED)
find_package(utf8proc 2.12.0 CONFIG REQUIRED)
