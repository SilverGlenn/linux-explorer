# Include after explorer_ui and the run-ui-tests.sh registration are defined.
add_executable(details_pane_tests tests/DetailsPaneTest.cpp)
target_link_libraries(details_pane_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME details_pane_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:details_pane_tests>)
set_tests_properties(details_pane_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 120
)
