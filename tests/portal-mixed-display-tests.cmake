# Real native protocol coverage; optional on systems without virtual KWin/X11.
find_package(X11 QUIET)
find_program(EXPLORER_TEST_MIXED_KWIN kwin_wayland)
find_program(EXPLORER_TEST_MIXED_XWAYLAND Xwayland)
if(X11_FOUND AND EXPLORER_TEST_MIXED_KWIN AND EXPLORER_TEST_MIXED_XWAYLAND)
    add_executable(portal_mixed_display_tests tests/PortalMixedDisplayTest.cpp)
    target_link_libraries(portal_mixed_display_tests PRIVATE
        Qt6::Widgets Qt6::DBus Qt6::Test explorer_portal_wire X11::X11)
    add_dependencies(portal_mixed_display_tests explorer_portal)
    add_test(NAME portal_mixed_display_tests COMMAND bash
        "${PROJECT_SOURCE_DIR}/tests/run-portal-mixed-display-test.sh"
        "${PROJECT_BINARY_DIR}" "${EXPLORER_TEST_MIXED_KWIN}")
    set_tests_properties(portal_mixed_display_tests PROPERTIES TIMEOUT 120)
endif()
