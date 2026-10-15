# Include from the existing BUILD_TESTING block after explorer_ui_tests setup.
add_executable(navigation_state_tests tests/NavigationStateTest.cpp)
target_link_libraries(navigation_state_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME navigation_state_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:navigation_state_tests>)
set_tests_properties(navigation_state_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 60
)
