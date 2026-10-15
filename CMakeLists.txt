cmake_minimum_required(VERSION 3.16)
project(Explorer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Without this a plain "cmake -B build" produces an unoptimised binary
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# DBus tells KDED to load or unload its automounter module
find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent Multimedia DBus UiPlugin)
find_package(KF6FrameworkIntegration REQUIRED)
# Every file operation goes through KIO, nothing is reimplemented here
find_package(KF6KIO REQUIRED)
find_package(KF6CoreAddons REQUIRED)
# KConfig reads and writes the desktop's own config files in their format
find_package(KF6Config REQUIRED)
find_package(KF6WidgetsAddons REQUIRED)
find_package(KF6WindowSystem REQUIRED)
# Solid knows a places entry's block device, which stands in for a drive letter
find_package(KF6Solid REQUIRED)
find_package(AeroQt REQUIRED)
if(TARGET AeroQt::AeroQt)
    set(EXPLORER_AERO_TARGET AeroQt::AeroQt)
else()
    set(EXPLORER_AERO_TARGET AeroQt)
endif()

# Share the real UI with the regression tests, including its resources.
add_library(explorer_ui OBJECT
    src/ui/win7ui.qrc
    src/aero/artwork.cpp
    src/aero/arrows.cpp
    src/aero/capacitybar.cpp
    src/aero/icons.cpp
    src/aero/listview.cpp
    src/aero/sounds.cpp
    src/aero/buttons.cpp
    src/aero/strips.cpp
    src/aero/taskdialog.cpp
    src/aero/text.cpp
    src/ui/AboutDialog.cpp
    src/ui/HelpDialog.cpp
    src/ui/AccessDialogs.cpp
    src/data/AutoMount.cpp
    src/data/Branding.cpp
    src/data/FileManagerService.cpp
    src/data/Settings.cpp
    src/model/Archives.cpp
    src/model/ComputerModel.cpp
    src/model/DirectoryModel.cpp
    src/model/DriveLabel.cpp
    src/model/FileOps.cpp
    src/model/GroupingProxy.cpp
    src/ui/ComputerView.cpp
    src/ui/FileProgressDialog.cpp
    src/ui/FileView.cpp
    src/ui/MapDriveDialog.cpp
    src/ui/MountDialog.cpp
    src/ui/OptionsDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/NavigationPane.cpp
    src/ui/PreviewPane.cpp
    src/ui/PdfPreviewRenderer.cpp
    src/ui/DetailsPane.cpp
    src/ui/BreadcrumbBar.cpp
    src/ui/SearchBox.cpp
    src/ui/LibrariesView.cpp
    src/ui/ViewModeMenu.cpp
)

target_include_directories(explorer_ui PUBLIC
    src/data
    src/model
    src/ui
    src
)

target_link_libraries(explorer_ui PUBLIC
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::DBus
    KF6::KIOCore
    KF6::KIOGui
    KF6::KIOWidgets
    KF6::KIOFileWidgets
    KF6::ConfigCore
    KF6::CoreAddons
    KF6::WidgetsAddons
    KF6::WindowSystem
    KF6::Solid
    ${EXPLORER_AERO_TARGET}
)

add_executable(explorer src/main.cpp)
target_link_libraries(explorer PRIVATE explorer_ui)

set_target_properties(explorer PROPERTIES
    OUTPUT_NAME explorer-win7
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
)

install(TARGETS explorer RUNTIME DESTINATION bin)
install(FILES linux-explorer-win7.desktop linux-explorer-win7-dialog.desktop
        DESTINATION share/applications)
configure_file(cmake/InstallDesktopEntries.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/InstallDesktopEntries.cmake @ONLY)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/InstallDesktopEntries.cmake)
install(FILES LICENSE DESTINATION share/licenses/linux-explorer-win7)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    include(tests/address-tests.cmake)
    include(tests/thumbnail-tests.cmake)
    include(tests/details-tests.cmake)
    include(tests/list-tests.cmake)
    include(tests/navigation-tests.cmake)
    include(tests/pdf-tests.cmake)
    add_executable(explorer_ui_tests tests/ExplorerUiTest.cpp)
    target_link_libraries(explorer_ui_tests PRIVATE explorer_ui Qt6::Test)
    add_test(NAME explorer_ui_tests COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:explorer_ui_tests>)
    set_tests_properties(explorer_ui_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
        TIMEOUT 120
    )
endif()
