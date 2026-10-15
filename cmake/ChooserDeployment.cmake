# The chooser is a private executable/D-Bus API, not an installed widget SDK.
# Keep its install component separate so an update need not replace Explorer.
function(explorer_install_chooser service_target)
    set_target_properties(${service_target} PROPERTIES
        INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib"
        INSTALL_RPATH_USE_LINK_PATH TRUE)
    add_executable(explorer_chooser_client "${PROJECT_SOURCE_DIR}/tools/chooser-client.cpp")
    target_link_libraries(explorer_chooser_client PRIVATE Qt6::Core Qt6::DBus)
    set_target_properties(explorer_chooser_client PROPERTIES
        OUTPUT_NAME explorer-win7-chooser-client)

    install(TARGETS ${service_target} explorer_chooser_client
        RUNTIME DESTINATION bin COMPONENT Chooser)
    configure_file("${PROJECT_SOURCE_DIR}/dbus/org.win7explorer.FileChooser1.service.in"
        "${PROJECT_BINARY_DIR}/org.win7explorer.FileChooser1.service" @ONLY)
    install(FILES "${PROJECT_BINARY_DIR}/org.win7explorer.FileChooser1.service"
        DESTINATION share/dbus-1/services COMPONENT Chooser)
    # The same dialog identity as normal Explorer dialogs, not the captionless
    # browser class. Installing it does not register a file-manager association.
    install(FILES "${PROJECT_SOURCE_DIR}/linux-explorer-win7-dialog.desktop"
        DESTINATION share/applications COMPONENT Chooser)
    configure_file("${PROJECT_SOURCE_DIR}/cmake/InstallChooserService.cmake.in"
        "${PROJECT_BINARY_DIR}/InstallChooserService.cmake" @ONLY)
    install(SCRIPT "${PROJECT_BINARY_DIR}/InstallChooserService.cmake" COMPONENT Chooser)
    install(FILES "${PROJECT_SOURCE_DIR}/dbus/org.win7explorer.FileChooser1.xml"
        DESTINATION share/dbus-1/interfaces COMPONENT Chooser)
    install(FILES "${PROJECT_SOURCE_DIR}/docs/file-chooser-api.md"
        DESTINATION share/doc/linux-explorer-win7 COMPONENT Chooser)
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
        DESTINATION share/licenses/linux-explorer-win7 COMPONENT Chooser)

    if(BUILD_TESTING)
        add_executable(chooser_wayland_tests "${PROJECT_SOURCE_DIR}/tests/ChooserWaylandTest.cpp")
        target_link_libraries(chooser_wayland_tests PRIVATE Qt6::Widgets Qt6::DBus
            Qt6::Test KF6::WindowSystem explorer_portal_wire)
        add_dependencies(chooser_wayland_tests explorer_portal)
        find_program(EXPLORER_TEST_KWIN_WAYLAND kwin_wayland)
        if(EXPLORER_TEST_KWIN_WAYLAND)
            add_test(NAME chooser_wayland_tests COMMAND bash
                "${PROJECT_SOURCE_DIR}/tests/run-chooser-wayland-test.sh"
                "${PROJECT_BINARY_DIR}" "${EXPLORER_TEST_KWIN_WAYLAND}")
            set_tests_properties(chooser_wayland_tests PROPERTIES TIMEOUT 90)
        endif()
        add_test(NAME chooser_activation_tests COMMAND bash
            "${PROJECT_SOURCE_DIR}/tests/run-chooser-activation-test.sh" "${PROJECT_BINARY_DIR}")
        set_tests_properties(chooser_activation_tests PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
            TIMEOUT 90)
    endif()
endfunction()
