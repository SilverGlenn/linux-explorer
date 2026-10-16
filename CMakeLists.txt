cmake_minimum_required(VERSION 3.19)
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
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Concurrent Multimedia DBus UiPlugin NetworkAuth)
find_package(Qt6Keychain REQUIRED)
include(cmake/GoogleDriveOAuth.cmake)
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
    src/aero/contextmenu.cpp
    src/aero/fonts.cpp
    src/aero/icons.cpp
    src/aero/listview.cpp
    src/aero/sounds.cpp
    src/aero/buttons.cpp
    src/aero/strips.cpp
    src/aero/taskdialog.cpp
    src/aero/text.cpp
    src/ui/ApplicationAppearance.cpp
    src/ui/ChooserWindow.cpp
    src/ui/AboutDialog.cpp
    src/ui/HelpDialog.cpp
    src/ui/AccessDialogs.cpp
    src/data/AutoMount.cpp
    src/data/Branding.cpp
    src/data/FileManagerService.cpp
    src/data/Settings.cpp
    src/data/GoogleDriveService.cpp
    src/data/GoogleDriveAuth.cpp
    src/data/GoogleDriveSecretStore.cpp
    src/data/GoogleDriveConnection.cpp
    src/data/GoogleDriveSyncController.cpp
    src/model/Archives.cpp
    src/model/ComputerModel.cpp
    src/model/DirectoryModel.cpp
    src/model/DriveLabel.cpp
    src/model/PropertiesData.cpp
    src/model/FolderAllocation.cpp
    src/model/FileOps.cpp
    src/model/GroupingProxy.cpp
    src/ui/ComputerView.cpp
    src/ui/FileProgressDialog.cpp
    src/ui/FileView.cpp
    src/ui/MapDriveDialog.cpp
    src/ui/GoogleDriveDialog.cpp
    src/ui/GoogleDriveSignInDialog.cpp
    src/ui/GoogleDriveSyncDialog.cpp
    src/ui/MountDialog.cpp
    src/ui/OptionsDialog.cpp
    src/ui/PropertiesDialog.cpp
    src/ui/FileChooserDialog.cpp
    src/ui/BrowserNavigation.cpp
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
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

target_link_libraries(explorer_ui PUBLIC
    Qt6::Widgets
    Qt6::Concurrent
    Qt6::Multimedia
    Qt6::DBus
    Qt6::NetworkAuth
    qt6keychain
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

# Private chooser service seam, shared only with its executable and tests.
add_library(explorer_chooser_service STATIC
    src/data/ChooserService.h
    src/data/ChooserService.cpp
    src/data/ChooserDBusTypes.h
    src/data/ChooserDBusTypes.cpp
)
target_link_libraries(explorer_chooser_service PUBLIC explorer_ui)
# Forward the objects too: a static archive would otherwise discard the UI's
# resource-only translation unit when linking the chooser or service tests.
target_sources(explorer_chooser_service INTERFACE $<TARGET_OBJECTS:explorer_ui>)

add_executable(explorer_chooser src/chooser-main.cpp)
target_link_libraries(explorer_chooser PRIVATE explorer_chooser_service)
set_target_properties(explorer_chooser PROPERTIES
    OUTPUT_NAME explorer-win7-chooser
    BUILD_RPATH "$ORIGIN"
)

# Standard portal wire types are reusable by non-GUI clients.
add_library(explorer_portal_wire STATIC src/data/PortalDBusTypes.cpp)
target_include_directories(explorer_portal_wire PUBLIC src/data)
target_link_libraries(explorer_portal_wire PUBLIC Qt6::Core Qt6::DBus)

add_library(explorer_portal_service STATIC
    src/data/PortalChooserService.cpp
    src/data/PortalChooserOptions.cpp
    src/data/ChooserPresenter.cpp
)
target_link_libraries(explorer_portal_service PUBLIC explorer_ui explorer_portal_wire)
target_sources(explorer_portal_service INTERFACE $<TARGET_OBJECTS:explorer_ui>)
add_executable(explorer_portal src/portal-main.cpp)
target_link_libraries(explorer_portal PRIVATE explorer_portal_service)
set_target_properties(explorer_portal PROPERTIES OUTPUT_NAME explorer-win7-portal BUILD_RPATH "$ORIGIN")

add_executable(explorer src/main.cpp)
target_link_libraries(explorer PRIVATE explorer_ui)

set_target_properties(explorer PROPERTIES
    OUTPUT_NAME explorer-win7
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
)

install(TARGETS explorer RUNTIME DESTINATION bin)
install(PROGRAMS scripts/google-drive-rclone.py
        DESTINATION bin RENAME explorer-win7-google-drive)
configure_file(scripts/google-drive-rclone.py
    ${CMAKE_CURRENT_BINARY_DIR}/explorer-win7-google-drive COPYONLY)
install(PROGRAMS scripts/google-drive-sync.py
        DESTINATION bin RENAME explorer-win7-google-drive-sync)
configure_file(scripts/google-drive-sync.py
    ${CMAKE_CURRENT_BINARY_DIR}/explorer-win7-google-drive-sync COPYONLY)
install(FILES linux-explorer-win7.desktop linux-explorer-win7-dialog.desktop
        linux-explorer-win7-chooser-dialog.desktop
        DESTINATION share/applications)
configure_file(cmake/InstallDesktopEntries.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/InstallDesktopEntries.cmake @ONLY)
install(SCRIPT ${CMAKE_CURRENT_BINARY_DIR}/InstallDesktopEntries.cmake)
install(FILES LICENSE DESTINATION share/licenses/linux-explorer-win7)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    include(tests/properties-tests.cmake)
    add_executable(font_tests tests/FontTest.cpp)
    target_link_libraries(font_tests PRIVATE explorer_ui Qt6::Test)
    add_test(NAME font_tests COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:font_tests>)
    set_tests_properties(font_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion;QT_FONT_DPI=96"
        TIMEOUT 60)
    include(tests/address-tests.cmake)
    include(tests/thumbnail-tests.cmake)
    include(tests/details-tests.cmake)
    include(tests/list-tests.cmake)
    include(tests/navigation-tests.cmake)
    include(tests/browser-navigation-tests.cmake)
    include(tests/google-drive-tests.cmake)
    include(tests/file-chooser-tests.cmake)
    include(tests/chooser-service-tests.cmake)
    include(tests/portal-chooser-tests.cmake)
    include(tests/portal-mixed-display-tests.cmake)
    add_executable(chooser_presentation_tests tests/ChooserPresentationTest.cpp)
    target_link_libraries(chooser_presentation_tests PRIVATE explorer_ui Qt6::Test)
    add_test(NAME chooser_presentation_tests COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:chooser_presentation_tests>)
    set_tests_properties(chooser_presentation_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
        TIMEOUT 120
    )
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

include(cmake/ChooserDeployment.cmake)
explorer_install_chooser(explorer_chooser)
include(cmake/PortalDeployment.cmake)
explorer_install_portal(explorer_portal)
if(BUILD_TESTING)
    include(tests/portal-frontend-tests.cmake)
endif()
