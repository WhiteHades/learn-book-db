add_executable(lbdb_unit unit_core.c)
target_compile_features(lbdb_unit PRIVATE c_std_23)
target_include_directories(lbdb_unit PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(lbdb_unit PRIVATE learn_book_db_lib)
lbdb_enable_warnings(lbdb_unit)

add_executable(lbdb_integration integration_cli.c)
target_compile_features(lbdb_integration PRIVATE c_std_23)
target_compile_definitions(
  lbdb_integration
  PRIVATE _POSIX_C_SOURCE=200809L
          _XOPEN_SOURCE=700
          LBDB_CLI_PATH="$<TARGET_FILE:learn-book-db>"
          LBDB_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
          LBDB_TEST_WORK_DIR="${CMAKE_CURRENT_BINARY_DIR}/fixtures")
target_link_libraries(lbdb_integration PRIVATE ${LBDB_SQLITE_TARGET})
lbdb_enable_warnings(lbdb_integration)
add_dependencies(lbdb_integration learn-book-db)

if(LBDB_ENABLE_SANITIZERS)
  foreach(target lbdb_unit lbdb_integration)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endforeach()
endif()

add_test(NAME unit_core COMMAND lbdb_unit)
add_test(NAME integration_cli COMMAND lbdb_integration)
add_test(
  NAME install_smoke
  COMMAND
    ${CMAKE_COMMAND} -DLBDB_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DLBDB_INSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/install-smoke -P
    ${CMAKE_CURRENT_SOURCE_DIR}/install_smoke.cmake)
set_tests_properties(
  unit_core integration_cli install_smoke
  PROPERTIES ENVIRONMENT
             "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
             TIMEOUT 60)
