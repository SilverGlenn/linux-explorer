add_executable(google_drive_tests tests/GoogleDriveTest.cpp)
add_dependencies(google_drive_tests explorer)
target_link_libraries(google_drive_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_tests>)
set_tests_properties(google_drive_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 90
)

add_executable(google_drive_computer_tests tests/GoogleDriveComputerTest.cpp)
target_link_libraries(google_drive_computer_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_computer_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_computer_tests>)
set_tests_properties(google_drive_computer_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 90)

add_executable(google_drive_navigation_tests tests/GoogleDriveNavigationTest.cpp)
target_link_libraries(google_drive_navigation_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_navigation_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_navigation_tests>)
set_tests_properties(google_drive_navigation_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 60)

add_executable(google_drive_auth_tests tests/GoogleDriveAuthTest.cpp)
target_link_libraries(google_drive_auth_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_auth_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_auth_tests>)
set_tests_properties(google_drive_auth_tests PROPERTIES TIMEOUT 90)

add_executable(google_drive_connection_tests tests/GoogleDriveConnectionTest.cpp)
target_link_libraries(google_drive_connection_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_connection_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_connection_tests>)
set_tests_properties(google_drive_connection_tests PROPERTIES TIMEOUT 90)

add_executable(google_drive_signin_tests tests/GoogleDriveSignInDialogTest.cpp)
target_link_libraries(google_drive_signin_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_signin_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_signin_tests>)
set_tests_properties(google_drive_signin_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion"
    TIMEOUT 90)

find_package(Python3 COMPONENTS Interpreter QUIET)
add_executable(google_drive_secret_store_tests tests/GoogleDriveSecretStoreTest.cpp)
target_link_libraries(google_drive_secret_store_tests PRIVATE explorer_ui Qt6::Test)
add_test(NAME google_drive_secret_store_tests COMMAND bash
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh $<TARGET_FILE:google_drive_secret_store_tests>)
set_tests_properties(google_drive_secret_store_tests PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion" TIMEOUT 90)
foreach(sync_suite IN ITEMS controller dialog)
    if(sync_suite STREQUAL "controller")
        set(sync_source tests/GoogleDriveSyncControllerTest.cpp)
    else()
        set(sync_source tests/GoogleDriveSyncDialogTest.cpp)
    endif()
    add_executable(google_drive_sync_${sync_suite}_tests ${sync_source})
    target_link_libraries(google_drive_sync_${sync_suite}_tests PRIVATE explorer_ui Qt6::Test)
    add_test(NAME google_drive_sync_${sync_suite}_tests COMMAND bash
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-ui-tests.sh
        $<TARGET_FILE:google_drive_sync_${sync_suite}_tests>)
    set_tests_properties(google_drive_sync_${sync_suite}_tests PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion" TIMEOUT 90)
endforeach()
if(Python3_Interpreter_FOUND)
    add_test(NAME google_drive_sync_helper_tests
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/GoogleDriveSyncHelperTest.py)
    set_tests_properties(google_drive_sync_helper_tests PROPERTIES TIMEOUT 120)
    add_test(NAME google_drive_mount_helper_tests
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/GoogleDriveMountHelperTest.py)
    set_tests_properties(google_drive_mount_helper_tests PROPERTIES TIMEOUT 90)
    add_test(NAME google_drive_release_config_tests
        COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/GoogleDriveReleaseConfigTest.py)
    set_tests_properties(google_drive_release_config_tests PROPERTIES TIMEOUT 90)
endif()
