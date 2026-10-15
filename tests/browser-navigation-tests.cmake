# The real extracted navigation surface, without a MainWindow or file operations.
add_executable(browser_navigation_tests tests/BrowserNavigationTest.cpp)
target_link_libraries(browser_navigation_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME browser_navigation_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:browser_navigation_tests>)
set_tests_properties(browser_navigation_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 60
)
