# Include from the top-level BUILD_TESTING block after explorer_ui and Qt6::Test.
add_executable(thumbnail_loading_tests ${CMAKE_CURRENT_SOURCE_DIR}/tests/ThumbnailLoadingTest.cpp)
target_link_libraries(thumbnail_loading_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME thumbnail_loading_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:thumbnail_loading_tests>)
set_tests_properties(thumbnail_loading_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 120
)
