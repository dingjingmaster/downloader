cmake_minimum_required(VERSION 3.20)

project(downloader VERSION 0.1.0 LANGUAGES C)

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

target_link_libraries(downloader PRIVATE clib::static Threads::Threads SQLite3::SQLite3 CURL::libcurl)
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
