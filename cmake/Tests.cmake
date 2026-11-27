function(packstacker_test_runtime)
    set_tests_properties(${ARGN} PROPERTIES TIMEOUT 60)
    if(WIN32)
        get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
        set(runtime_environment)
        if(VCPKG_TOOLCHAIN)
            list(APPEND runtime_environment
                "PATH=path_list_prepend:${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin")
        endif()
        list(APPEND runtime_environment "PATH=path_list_prepend:${compiler_directory}")
        set_tests_properties(${ARGN} PROPERTIES ENVIRONMENT_MODIFICATION
            "${runtime_environment}"
        )
    endif()
endfunction()

function(packstacker_test name)
    set(target packstacker_${name}_tests)
    add_executable(${target} tests/${name}_tests.cpp)
    packstacker_warnings(${target})
    add_test(NAME packstacker_${name} COMMAND ${target} ${ARGN})
    packstacker_test_runtime(packstacker_${name})
endfunction()

packstacker_test(model)
target_include_directories(packstacker_model_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(packstacker_model_tests PRIVATE cxx_std_23)

packstacker_test(metadata)
target_link_libraries(packstacker_metadata_tests PRIVATE packstacker_metadata)

packstacker_test(source)
target_link_libraries(packstacker_source_tests PRIVATE packstacker_source libzip::zip)

packstacker_test(resolver)
target_link_libraries(packstacker_resolver_tests PRIVATE packstacker_resolver)

packstacker_test(rules)
target_link_libraries(packstacker_rules_tests
    PRIVATE packstacker_core packstacker_paths nlohmann_json::nlohmann_json
)

packstacker_test(unicode)
target_link_libraries(packstacker_unicode_tests PRIVATE packstacker_unicode)

packstacker_test(paths)
target_link_libraries(packstacker_paths_tests PRIVATE packstacker_paths packstacker_unicode)

packstacker_test(pattern)
target_link_libraries(packstacker_pattern_tests PRIVATE packstacker_pattern)

packstacker_test(streams)
target_link_libraries(packstacker_streams_tests PRIVATE packstacker_streams)

packstacker_test(filesystem)
target_link_libraries(packstacker_filesystem_tests PRIVATE packstacker_paths)

packstacker_test(pipeline)
target_link_libraries(packstacker_pipeline_tests PRIVATE packstacker_core)

packstacker_test(core)
target_link_libraries(packstacker_core_tests
    PRIVATE packstacker_core packstacker_paths nlohmann_json::nlohmann_json libzip::zip
)

packstacker_test(export)
target_link_libraries(packstacker_export_tests
    PRIVATE packstacker_core packstacker_paths nlohmann_json::nlohmann_json
)

packstacker_test(semantics)
target_compile_definitions(packstacker_semantics_tests PRIVATE
    PACKSTACKER_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures"
)
target_link_libraries(packstacker_semantics_tests
    PRIVATE packstacker_core packstacker_paths nlohmann_json::nlohmann_json
)

packstacker_test(cli_options)
target_link_libraries(packstacker_cli_options_tests PRIVATE packstacker_cli_options)

packstacker_test(cli $<TARGET_FILE:packstacker_cli>)
target_sources(packstacker_cli_tests PRIVATE tests/process.cpp)
target_include_directories(packstacker_cli_tests PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_link_libraries(packstacker_cli_tests
    PRIVATE packstacker_core packstacker_paths nlohmann_json::nlohmann_json
)
if(MINGW)
    target_link_options(packstacker_cli_tests PRIVATE -municode)
endif()
add_dependencies(packstacker_cli_tests packstacker_cli)

if(PACKSTACKER_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    packstacker_test(gui)
    qt_add_resources(packstacker_gui_tests gui_test_ui
        PREFIX "/qt/qml/PackStacker"
        BASE "${PROJECT_SOURCE_DIR}/src/gui/qml"
        FILES ${PACKSTACKER_QML_FILES}
    )
    target_link_libraries(packstacker_gui_tests
        PRIVATE packstacker_gui_support Qt6::Quick Qt6::Test nlohmann_json::nlohmann_json
    )
    target_compile_definitions(packstacker_gui_tests PRIVATE
        PACKSTACKER_QML_SOURCE="${PROJECT_SOURCE_DIR}/src/gui/qml/Main.qml"
    )
    packstacker_test(qml_smoke)
    qt_add_resources(packstacker_qml_smoke_tests smoke_test_ui
        PREFIX "/qt/qml/PackStacker"
        BASE "${PROJECT_SOURCE_DIR}/src/gui/qml"
        FILES ${PACKSTACKER_QML_FILES}
    )
    target_link_libraries(packstacker_qml_smoke_tests
        PRIVATE packstacker_gui_support Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2
    )
    set_tests_properties(packstacker_gui packstacker_qml_smoke PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_FORCE_STDERR_LOGGING=1"
    )
    if(WIN32)
        set_property(TEST packstacker_gui packstacker_qml_smoke APPEND PROPERTY
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
                "QT_QPA_FONTDIR=set:$ENV{WINDIR}/Fonts"
        )
    endif()
endif()
