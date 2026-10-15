find_program(EXPLORER_TEST_PORTAL_FRONTEND NAMES xdg-desktop-portal
    PATHS /usr/libexec /usr/lib/xdg-desktop-portal NO_DEFAULT_PATH REQUIRED)
find_program(EXPLORER_TEST_DBUS_MONITOR NAMES dbus-monitor REQUIRED)
add_executable(portal_frontend_tests tests/PortalFrontendTest.cpp)
target_link_libraries(portal_frontend_tests PRIVATE explorer_portal_service Qt6::Test)
target_compile_definitions(portal_frontend_tests PRIVATE
    PORTAL_FRONTEND="${EXPLORER_TEST_PORTAL_FRONTEND}"
    PORTAL_CLIENT="$<TARGET_FILE:explorer_portal_client>"
    PORTAL_BACKEND="$<TARGET_FILE:explorer_portal>"
    PORTAL_DBUS_MONITOR="${EXPLORER_TEST_DBUS_MONITOR}"
    PORTAL_FRONTEND_LOG="${PROJECT_BINARY_DIR}/portal-frontend.log")
add_dependencies(portal_frontend_tests explorer_portal_client explorer_portal)
add_test(NAME portal_frontend_tests COMMAND bash
    ${PROJECT_SOURCE_DIR}/tests/run-portal-frontend-test.sh $<TARGET_FILE:portal_frontend_tests>)
set_tests_properties(portal_frontend_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion" TIMEOUT 120)
add_test(NAME portal_activation_tests COMMAND bash
    ${PROJECT_SOURCE_DIR}/tests/run-portal-activation-test.sh ${PROJECT_BINARY_DIR})
set_tests_properties(portal_activation_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion" TIMEOUT 90)
