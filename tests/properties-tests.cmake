add_executable(properties_data_tests tests/PropertiesDataTest.cpp)
target_link_libraries(properties_data_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME properties_data_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:properties_data_tests>)
set_tests_properties(properties_data_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 60)

add_executable(properties_dialog_tests tests/PropertiesDialogTest.cpp)
target_link_libraries(properties_dialog_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME properties_dialog_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:properties_dialog_tests>)
set_tests_properties(properties_dialog_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 90)
