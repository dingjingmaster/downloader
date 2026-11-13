cmake_minimum_required(VERSION 3.20)

project(downloader VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(CURL REQUIRED)

set(BUILD_TESTING OFF)
set(BUILD_TESTING OFF CACHE BOOL "Disable clib tests in downloader" FORCE)
add_subdirectory(clib EXCLUDE_FROM_ALL)
add_subdirectory(3thrd)

add_library(download_engines STATIC protocols/engines.cpp protocols/aria.cpp protocols/media.c protocols/smb.c)
target_include_directories(download_engines PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/protocols")
target_include_directories(download_engines PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
target_link_libraries(download_engines PUBLIC clib::static downloader_aria2 downloader_avformat downloader_smb2)
find_library(DOWNLOADER_ANL_LIBRARY anl)
if(DOWNLOADER_ANL_LIBRARY)
    target_link_libraries(download_engines PUBLIC "${DOWNLOADER_ANL_LIBRARY}")
endif()

add_executable(downloader
    daemon/main.c
    daemon/config.c
    daemon/database.c
    daemon/task.c
    daemon/protocol.c
    daemon/filesystem.c
    daemon/scheduler.c
    daemon/ipc.c
    daemon/web.c
    daemon/web-api.c
    daemon/task-manager.c
    protocols/http.c
)

target_include_directories(
    downloader
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/daemon"
)

target_link_libraries(downloader PRIVATE download_engines clib::static Threads::Threads SQLite3::SQLite3 CURL::libcurl)
target_compile_definitions(downloader PRIVATE DOWNLOADER_WEB_ROOT="${CMAKE_INSTALL_PREFIX}/share/downloader/web")

set_target_properties(
    downloader
    PROPERTIES
        C_STANDARD 99
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
)

install(TARGETS downloader RUNTIME DESTINATION bin)
install(FILES config/download.ini DESTINATION /etc/downloader)
install(DIRECTORY web/ DESTINATION share/downloader/web)

option(DOWNLOADER_BUILD_TESTS "Build downloader tests" ON)
if(DOWNLOADER_BUILD_TESTS)
    enable_testing()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME downloader_http_transfer COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-download.py" $<TARGET_FILE:downloader>)
    add_test(NAME downloader_http_resilience COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-http-resilience.py" $<TARGET_FILE:downloader>)
    set_tests_properties(downloader_http_resilience PROPERTIES TIMEOUT 150)
    add_test(NAME downloader_bittorrent_transfer COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-bittorrent.py" $<TARGET_FILE:downloader>)
    add_test(NAME downloader_aria_adapter COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-aria-adapter.py" "${CMAKE_BINARY_DIR}")
    set_tests_properties(downloader_http_transfer downloader_bittorrent_transfer PROPERTIES TIMEOUT 180)
    set_tests_properties(downloader_aria_adapter PROPERTIES TIMEOUT 180)
    find_program(DOWNLOADER_FIXTURE_SMBD smbd)
    if(DOWNLOADER_FIXTURE_SMBD)
        add_test(NAME downloader_smb_transfer COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-smb.py" --build-dir "${CMAKE_BINARY_DIR}"
            --daemon $<TARGET_FILE:downloader>)
        set_tests_properties(downloader_smb_transfer PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    endif()
    find_program(DOWNLOADER_FIXTURE_FFMPEG ffmpeg)
    find_program(DOWNLOADER_FIXTURE_FFPROBE ffprobe)
    if(DOWNLOADER_FIXTURE_FFMPEG AND DOWNLOADER_FIXTURE_FFPROBE)
        add_test(NAME downloader_media_transfer COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-media.py" $<TARGET_FILE:downloader>)
        set_tests_properties(downloader_media_transfer PROPERTIES TIMEOUT 120)
    endif()
    add_executable(test_engines tests/test-engines.c)
    target_link_libraries(test_engines PRIVATE download_engines)
    add_test(NAME downloader_engines COMMAND test_engines)
    set_tests_properties(downloader_engines PROPERTIES
        ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/aria2-config")
    add_executable(test_media_adapter tests/test-media-adapter.c)
    target_include_directories(test_media_adapter PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_media_adapter PRIVATE clib::static downloader_avformat)
    add_test(NAME downloader_media_adapter COMMAND test_media_adapter)
    add_executable(test_config
        tests/test-config.c
        daemon/config.c
    )
    target_include_directories(test_config PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_config PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_config COMMAND test_config)

    add_executable(test_web_config
        tests/test-web-config.c
        daemon/config.c
    )
    target_include_directories(test_web_config PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_web_config PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_web_config COMMAND test_web_config)

    add_executable(test_web_lifecycle
        tests/test-web-lifecycle.c
        daemon/web.c
    )
    target_include_directories(test_web_lifecycle PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_web_lifecycle PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_web_lifecycle COMMAND test_web_lifecycle)

    add_executable(test_task_manager
        daemon/protocol.c
        daemon/scheduler.c
        tests/test-task-manager.c
        daemon/task-manager.c
        daemon/database.c
        daemon/task.c
        daemon/filesystem.c
    )
    target_include_directories(test_task_manager PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_task_manager PRIVATE clib::static Threads::Threads SQLite3::SQLite3)
    add_test(NAME downloader_task_manager COMMAND test_task_manager)

    add_executable(test_web_api
        daemon/protocol.c
        daemon/scheduler.c
        tests/test-web-api.c
        daemon/web-api.c
        daemon/task-manager.c
        daemon/database.c
        daemon/task.c
        daemon/filesystem.c
    )
    target_include_directories(test_web_api PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_web_api PRIVATE clib::static Threads::Threads SQLite3::SQLite3)
    add_test(NAME downloader_web_api COMMAND test_web_api)

    add_executable(test_web_static
        tests/test-web-static.c
        daemon/web.c
    )
    target_include_directories(test_web_static PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_web_static PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_web_static COMMAND test_web_static)

    add_executable(test_web_e2e
        tests/test-web-e2e.c
    )
    target_link_libraries(test_web_e2e PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_web_e2e COMMAND test_web_e2e $<TARGET_FILE:downloader>)

    add_executable(test_database
        tests/test-database.c
        daemon/task.c
        daemon/database.c
    )
    target_include_directories(test_database PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_database PRIVATE clib::static Threads::Threads SQLite3::SQLite3)
    add_test(NAME downloader_database COMMAND test_database)

    add_executable(test_protocol
        tests/test-protocol.c
        daemon/protocol.c
        protocols/http.c
    )
    target_include_directories(test_protocol PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_protocol PRIVATE clib::static Threads::Threads CURL::libcurl)
    add_test(NAME downloader_protocol COMMAND test_protocol)

    add_executable(test_scheduler
        tests/test-scheduler.c
        daemon/filesystem.c
        daemon/scheduler.c
        daemon/task.c
    )
    target_include_directories(test_scheduler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_scheduler PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_scheduler COMMAND test_scheduler)

    add_executable(test_ipc
        tests/test-ipc.c
        daemon/ipc.c
    )
    target_include_directories(test_ipc PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/daemon")
    target_link_libraries(test_ipc PRIVATE clib::static Threads::Threads)
    add_test(NAME downloader_ipc COMMAND test_ipc)
endif()
