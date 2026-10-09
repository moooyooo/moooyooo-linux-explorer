cmake_minimum_required(VERSION 3.24)
project(moooyooo-linux-explorer VERSION 0.2.0 LANGUAGES CXX)

cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${CMAKE_BINARY_DIR}" NORMALIZE MLEX_BUILD_IN_SOURCE)
if(MLEX_BUILD_IN_SOURCE)
    message(FATAL_ERROR "Use an out-of-source build; see CMakePresets.json")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0>=2.80)
find_package(Threads REQUIRED)
include(CTest)
option(MLEX_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)
add_compile_options(-Wall -Wextra -Wpedantic)
if(MLEX_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

add_library(mlex_core STATIC
    src/domain/project.cpp
    src/domain/file_operation.cpp
    src/application/operation_service.cpp
    src/persistence/project_store.cpp
    src/persistence/recovery_store.cpp
    src/platform/paths.cpp)
target_include_directories(mlex_core PUBLIC src)
target_link_libraries(mlex_core PUBLIC Qt6::Core)
qt_add_resources(mlex_core project_schema PREFIX /mlex FILES schemas/project.schema.json)

add_library(mlex_operations STATIC src/workers/operation_engine.cpp)
target_link_libraries(mlex_operations PUBLIC mlex_core PkgConfig::GIO)
add_executable(mlex-file-worker src/workers/file_worker.cpp)
target_link_libraries(mlex-file-worker PRIVATE mlex_operations Threads::Threads)

add_library(mlex_ui STATIC
    src/ui/folder_pane.cpp
    src/ui/command_registry.cpp
    src/ui/file_commands.cpp
    src/ui/file_clipboard.cpp
    src/ui/main_window.cpp)
target_link_libraries(mlex_ui PUBLIC mlex_core Qt6::Widgets)
add_executable(moooyooo-linux-explorer src/main.cpp)
target_link_libraries(moooyooo-linux-explorer PRIVATE mlex_ui)
add_dependencies(moooyooo-linux-explorer mlex-file-worker)
target_compile_definitions(moooyooo-linux-explorer PRIVATE MLEX_VERSION="${PROJECT_VERSION}")
install(TARGETS moooyooo-linux-explorer mlex-file-worker RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
    add_executable(test_project tests/test_project.cpp)
    target_link_libraries(test_project PRIVATE mlex_core Qt6::Test)
    target_compile_definitions(test_project PRIVATE MLEX_SAMPLE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/examples/sample-project.json")
    add_test(NAME project COMMAND test_project)
    add_executable(test_workspace tests/test_workspace.cpp)
    target_link_libraries(test_workspace PRIVATE mlex_ui Qt6::Test)
    add_dependencies(test_workspace mlex-file-worker)
    add_test(NAME workspace COMMAND test_workspace)
    add_executable(test_operations tests/test_operations.cpp)
    target_link_libraries(test_operations PRIVATE mlex_operations Qt6::Test)
    add_dependencies(test_operations mlex-file-worker)
    add_test(NAME operations COMMAND test_operations)
    set_tests_properties(operations PROPERTIES TIMEOUT 90)
    set_tests_properties(workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
    add_test(NAME cli_help COMMAND moooyooo-linux-explorer --help)
    add_test(NAME cli_version COMMAND moooyooo-linux-explorer --version)
    set_tests_properties(cli_help cli_version PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=unavailable" TIMEOUT 5)
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    add_test(NAME cli_processes COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_cli.py $<TARGET_FILE:moooyooo-linux-explorer>)
    set_tests_properties(cli_processes PROPERTIES TIMEOUT 25)
endif()
