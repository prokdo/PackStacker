function(packstacker_library target)
    add_library(${target} STATIC ${ARGN})
    target_include_directories(${target} PUBLIC "${PROJECT_SOURCE_DIR}/include")
    target_compile_features(${target} PUBLIC cxx_std_23)
    packstacker_warnings(${target})
endfunction()

packstacker_library(packstacker_unicode
    include/core/unicode.hpp
    src/core/unicode.cpp
)
target_link_libraries(packstacker_unicode PRIVATE utf8proc::utf8proc)

packstacker_library(packstacker_paths
    include/core/filesystem.hpp
    include/core/paths.hpp
    src/core/filesystem.cpp
    src/core/paths.cpp
)
target_link_libraries(packstacker_paths PRIVATE packstacker_unicode)
target_compile_definitions(packstacker_paths PRIVATE $<$<PLATFORM_ID:Linux>:_GNU_SOURCE>)
if(WIN32)
    target_link_libraries(packstacker_paths PRIVATE advapi32)
endif()

packstacker_library(packstacker_pattern
    include/core/filters.hpp
    include/core/pattern.hpp
    src/core/filters.cpp
    src/core/pattern.cpp
)
target_link_libraries(packstacker_pattern PRIVATE packstacker_unicode)

packstacker_library(packstacker_streams
    include/core/streams.hpp
    src/core/streams.cpp
)

packstacker_library(packstacker_metadata
    include/core/metadata.hpp
    include/core/sounds.hpp
    src/core/metadata.cpp
    src/core/sounds.cpp
)
target_link_libraries(packstacker_metadata
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE packstacker_pattern packstacker_paths packstacker_unicode
)

packstacker_library(packstacker_source
    include/core/pack_source.hpp
    src/core/pack_source.cpp
    src/core/inspection.cpp
    include/core/zip_archive.hpp
)
target_link_libraries(packstacker_source
    PUBLIC packstacker_streams
    PRIVATE packstacker_paths packstacker_unicode packstacker_metadata libzip::zip
)

packstacker_library(packstacker_resolver
    include/core/rules.hpp
    src/core/rules.cpp
    src/core/resolve.cpp
)
target_link_libraries(packstacker_resolver
    PUBLIC packstacker_streams
    PRIVATE
        packstacker_source
        packstacker_metadata
        packstacker_pattern
        packstacker_paths
        packstacker_unicode
)

packstacker_library(packstacker_core
    include/core/model.hpp
    include/core/pack.hpp
    src/core/export.cpp
    include/core/io.hpp
)
target_link_libraries(packstacker_core
    PUBLIC packstacker_streams
    PRIVATE
        packstacker_resolver
        packstacker_source
        packstacker_unicode
        packstacker_paths
        packstacker_pattern
        packstacker_metadata
        nlohmann_json::nlohmann_json
        libzip::zip
)

packstacker_library(packstacker_cli_options include/cli/options.hpp src/cli/options.cpp)
target_link_libraries(packstacker_cli_options PRIVATE CLI11::CLI11)

add_executable(packstacker_cli src/cli/main.cpp)
set_target_properties(packstacker_cli PROPERTIES OUTPUT_NAME packstacker-cli)
target_link_libraries(packstacker_cli PRIVATE packstacker_core packstacker_cli_options)
target_include_directories(packstacker_cli PRIVATE "${PROJECT_BINARY_DIR}/generated")
if(MINGW)
    target_link_options(packstacker_cli PRIVATE -municode)
endif()
packstacker_warnings(packstacker_cli)
