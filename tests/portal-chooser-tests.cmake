add_executable(portal_chooser_tests tests/PortalChooserTest.cpp)
target_link_libraries(portal_chooser_tests PRIVATE explorer_portal_service Qt6::Test)
target_compile_definitions(portal_chooser_tests PRIVATE
    PORTAL_BINARY="$<TARGET_FILE:explorer_portal>"
    PORTAL_TEST_BUS_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/session-bus.conf")
add_dependencies(portal_chooser_tests explorer_portal)
add_test(NAME portal_chooser_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:portal_chooser_tests>)
set_tests_properties(portal_chooser_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 120
)
