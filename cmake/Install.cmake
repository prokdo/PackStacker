set(runtime_dependency_args)
if(WIN32)
    set(runtime_dependency_args RUNTIME_DEPENDENCY_SET packstacker_runtime)
endif()
install(TARGETS packstacker_cli ${runtime_dependency_args}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if(PACKSTACKER_BUILD_GUI)
    install(TARGETS packstacker_gui ${runtime_dependency_args}
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    qt_generate_deploy_qml_app_script(
        TARGET packstacker_gui
        OUTPUT_SCRIPT gui_deploy_script
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
    install(SCRIPT ${gui_deploy_script})
endif()

if(WIN32)
    get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(runtime_directories "${compiler_directory}")
    if(VCPKG_TOOLCHAIN)
        list(APPEND runtime_directories
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/$<$<CONFIG:Debug>:debug/>bin")
    endif()
    if(PACKSTACKER_BUILD_GUI)
        list(APPEND runtime_directories "$<TARGET_FILE_DIR:Qt6::Core>")
    endif()
    install(CODE [[
        if(POLICY CMP0207)
            cmake_policy(SET CMP0207 NEW)
        endif()
    ]])
    install(RUNTIME_DEPENDENCY_SET packstacker_runtime
        DIRECTORIES ${runtime_directories}
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "^[Qq][Tt]6"
        POST_EXCLUDE_REGEXES [[.*[/\\][Ss][Yy][Ss][Tt][Ee][Mm]32[/\\].*]]
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
