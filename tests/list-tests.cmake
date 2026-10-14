# Include from the existing BUILD_TESTING block after explorer_ui_tests setup.
add_executable(list_chrome_tests tests/ListChromeTest.cpp)
target_link_libraries(list_chrome_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME list_chrome_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:list_chrome_tests>)
set_tests_properties(list_chrome_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 30
)
