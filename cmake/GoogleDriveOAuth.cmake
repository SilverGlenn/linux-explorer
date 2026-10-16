# This is maintainer/release configuration, never an end-user account setting.
set(EXPLORER_GOOGLE_DRIVE_CLIENT_JSON "" CACHE FILEPATH
    "Maintainer-owned Google Desktop OAuth client JSON, outside the source tree")
option(EXPLORER_REQUIRE_GOOGLE_DRIVE_CLIENT
    "Reject a Google-Drive-enabled release without its app-owned OAuth client" OFF)

set(EXPLORER_GOOGLE_CLIENT_ID "")
set(EXPLORER_GOOGLE_CLIENT_SECRET "")
if(EXPLORER_GOOGLE_DRIVE_CLIENT_JSON)
    if(NOT EXISTS "${EXPLORER_GOOGLE_DRIVE_CLIENT_JSON}")
        message(FATAL_ERROR "The maintainer Google Desktop OAuth client file is missing.")
    endif()
    file(REAL_PATH "${EXPLORER_GOOGLE_DRIVE_CLIENT_JSON}" _google_client_path)
    file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" _google_source_root)
    string(FIND "${_google_client_path}" "${_google_source_root}/" _google_in_source)
    if(_google_in_source EQUAL 0)
        message(FATAL_ERROR "Keep the maintainer OAuth client file outside the source repository.")
    endif()
    file(SIZE "${EXPLORER_GOOGLE_DRIVE_CLIENT_JSON}" _google_client_size)
    if(_google_client_size GREATER 65536)
        message(FATAL_ERROR "The Google Desktop OAuth client file is too large.")
    endif()
    file(READ "${EXPLORER_GOOGLE_DRIVE_CLIENT_JSON}" _google_client_json)
    string(JSON EXPLORER_GOOGLE_CLIENT_ID ERROR_VARIABLE _google_id_error
        GET "${_google_client_json}" installed client_id)
    string(JSON EXPLORER_GOOGLE_CLIENT_SECRET ERROR_VARIABLE _google_secret_error
        GET "${_google_client_json}" installed client_secret)
    if(_google_id_error OR _google_secret_error
        OR NOT EXPLORER_GOOGLE_CLIENT_ID MATCHES "^[A-Za-z0-9_-]+\\.apps\\.googleusercontent\\.com$"
        OR NOT EXPLORER_GOOGLE_CLIENT_SECRET MATCHES "^[A-Za-z0-9._~-]+$")
        message(FATAL_ERROR "A valid Google Desktop app OAuth client is required. Web/service-account credentials are unsupported.")
    endif()
    string(LENGTH "${EXPLORER_GOOGLE_CLIENT_ID}" _google_id_length)
    string(LENGTH "${EXPLORER_GOOGLE_CLIENT_SECRET}" _google_secret_length)
    if(_google_id_length GREATER 256 OR _google_secret_length GREATER 4096)
        message(FATAL_ERROR "The Google Desktop OAuth client fields are too large.")
    endif()
    unset(_google_client_json)
elseif(EXPLORER_REQUIRE_GOOGLE_DRIVE_CLIENT)
    message(FATAL_ERROR "This release requires the maintainer-owned Desktop OAuth client. Set EXPLORER_GOOGLE_DRIVE_CLIENT_JSON.")
endif()

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/data/GoogleDriveBuildConfig.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/GoogleDriveBuildConfig.h" @ONLY)
unset(EXPLORER_GOOGLE_CLIENT_SECRET)
unset(EXPLORER_GOOGLE_CLIENT_ID)
