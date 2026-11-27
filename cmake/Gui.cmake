if(PACKSTACKER_QT_FROM_VCPKG)
    set(qt_install_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
    cmake_path(IS_PREFIX qt_install_prefix "${Qt6_DIR}" NORMALIZE qt_from_vcpkg)
    if(NOT qt_from_vcpkg)
        message(FATAL_ERROR "Qt was found outside vcpkg. Use a fresh build directory without external Qt paths.")
    endif()
endif()
qt_standard_project_setup(REQUIRES 6.10)

add_library(packstacker_gui_support STATIC
    src/gui/pack_controller.cpp
    include/gui/pack_controller.hpp
    src/gui/pack_catalog.cpp
    include/gui/pack_catalog.hpp
    src/gui/localization.cpp
    include/gui/localization.hpp
)
target_link_libraries(packstacker_gui_support
    PUBLIC packstacker_core Qt6::Core
    PRIVATE Qt6::Gui nlohmann_json::nlohmann_json
)
qt_extract_metatypes(packstacker_gui_support)
packstacker_warnings(packstacker_gui_support)

qt_add_executable(packstacker_gui WIN32 src/gui/main.cpp)
target_include_directories(packstacker_gui PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_include_directories(packstacker_gui PRIVATE ${PROJECT_SOURCE_DIR}/include/gui)
set_target_properties(packstacker_gui PROPERTIES OUTPUT_NAME packstacker-gui)
target_link_libraries(packstacker_gui
    PRIVATE packstacker_gui_support Qt6::Quick Qt6::QuickControls2 Qt6::QuickDialogs2
)
set(PACKSTACKER_QML_FILES
    src/gui/qml/Main.qml
    src/gui/qml/PackPanels.qml
    src/gui/qml/OutputSettings.qml
    src/gui/qml/ExportBar.qml
    src/gui/qml/RulesEditor.qml
    src/gui/qml/UiTheme.qml
    src/gui/qml/Glyph.qml
    src/gui/qml/ActionButton.qml
    src/gui/qml/EntryField.qml
    src/gui/qml/PackRow.qml
    src/gui/qml/Choice.qml
)
foreach(qml_file IN LISTS PACKSTACKER_QML_FILES)
    get_filename_component(qml_name "${qml_file}" NAME)
    set_source_files_properties("${qml_file}" PROPERTIES QT_RESOURCE_ALIAS "${qml_name}")
endforeach()
qt_add_qml_module(packstacker_gui
    URI PackStacker VERSION 1.0
    SOURCES include/gui/qml_types.hpp
    QML_FILES ${PACKSTACKER_QML_FILES}
)
packstacker_warnings(packstacker_gui)
