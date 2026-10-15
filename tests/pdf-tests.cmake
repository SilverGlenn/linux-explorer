# Parent integration: add src/ui/PdfPreviewRenderer.cpp to explorer_ui, then
# include this file inside BUILD_TESTING after Qt6::Test is available.
add_executable(pdf_preview_tests tests/PdfPreviewTest.cpp)
target_link_libraries(pdf_preview_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME pdf_preview_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:pdf_preview_tests>)
set_tests_properties(pdf_preview_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 120
)
