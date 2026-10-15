# Include from BUILD_TESTING after adding FileChooserDialog.cpp to explorer_ui.
add_executable(file_chooser_tests tests/FileChooserTest.cpp)
target_link_libraries(file_chooser_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME file_chooser_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:file_chooser_tests>)
set_tests_properties(file_chooser_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 90
)
