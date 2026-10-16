# Include from the top-level BUILD_TESTING block after finding Qt6::Test.
add_executable(address_controls_tests
    tests/AddressControlsTest.cpp
    src/ui/BreadcrumbBar.cpp
    src/ui/SearchBox.cpp
    src/aero/fonts.cpp
    src/aero/text.cpp
)
target_include_directories(address_controls_tests PRIVATE src src/ui)
target_link_libraries(address_controls_tests PRIVATE Qt6::Widgets Qt6::Test ${EXPLORER_AERO_TARGET})
add_test(NAME address_controls_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:address_controls_tests>)
set_tests_properties(address_controls_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 60
)
