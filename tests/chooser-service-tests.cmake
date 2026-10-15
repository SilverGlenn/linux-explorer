# Parent integration: explorer_chooser_service supplies ChooserService.cpp and
# ChooserDBusTypes.cpp, public src/data + src/ui include paths, and links the
# actual dialog/UI target, Qt6::DBus, Qt6::Widgets and KF6::WindowSystem.
# Include after Qt6::Test is available. No browser service is needed by tests.
add_executable(chooser_service_tests tests/ChooserServiceTest.cpp)
target_link_libraries(chooser_service_tests PRIVATE explorer_chooser_service Qt6::Test)
add_test(NAME chooser_service_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:chooser_service_tests>)
set_tests_properties(chooser_service_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 120
)
