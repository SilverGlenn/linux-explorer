# Standard portal backend is optional integration, not a public widget SDK.
function(explorer_install_portal service_target)
    set_target_properties(${service_target} PROPERTIES
        INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
        INSTALL_RPATH_USE_LINK_PATH TRUE)
    add_executable(explorer_portal_client "${PROJECT_SOURCE_DIR}/tools/portal-client.cpp")
    target_link_libraries(explorer_portal_client PRIVATE explorer_portal_wire Qt6::Core Qt6::DBus)
    set_target_properties(explorer_portal_client PROPERTIES OUTPUT_NAME explorer-win7-portal-client)
    install(TARGETS ${service_target} explorer_portal_client
        RUNTIME DESTINATION bin COMPONENT Portal)
    install(PROGRAMS "${PROJECT_SOURCE_DIR}/tools/portal-preferences.py"
        DESTINATION bin RENAME explorer-win7-portal-setup COMPONENT Portal)
    configure_file("${PROJECT_SOURCE_DIR}/dbus/org.freedesktop.impl.portal.desktop.explorerwin7.service.in"
        "${PROJECT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.explorerwin7.service" @ONLY)
    install(FILES "${PROJECT_BINARY_DIR}/org.freedesktop.impl.portal.desktop.explorerwin7.service"
        DESTINATION share/dbus-1/services COMPONENT Portal)
    install(FILES "${PROJECT_SOURCE_DIR}/portal/explorer-win7.portal"
        DESTINATION share/xdg-desktop-portal/portals COMPONENT Portal)
    install(FILES "${PROJECT_SOURCE_DIR}/linux-explorer-win7-dialog.desktop"
        DESTINATION share/applications COMPONENT Portal)
    configure_file("${PROJECT_SOURCE_DIR}/cmake/InstallPortalService.cmake.in"
        "${PROJECT_BINARY_DIR}/InstallPortalService.cmake" @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/InstallPortalService.cmake" COMPONENT Portal)
    install(FILES "${PROJECT_SOURCE_DIR}/docs/file-chooser-portal.md"
        DESTINATION share/doc/linux-explorer-win7 COMPONENT Portal)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION share/licenses/linux-explorer-win7 COMPONENT Portal)
    if(BUILD_TESTING)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME portal_preferences_tests COMMAND "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tests/PortalPreferencesTest.py")
        set_tests_properties(portal_preferences_tests PROPERTIES TIMEOUT 30)
    endif()
endfunction()
