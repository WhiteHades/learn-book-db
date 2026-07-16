cmake_minimum_required(VERSION 3.25)

project(
  learn_book_db
  VERSION 1.0.1
  DESCRIPTION "Source-grounded learning and quiz database CLI"
  LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

option(LBDB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(LBDB_BUILD_TESTS "Build the CTest suite" ${BUILD_TESTING})

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)
if(TARGET SQLite3::SQLite3)
  set(LBDB_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
  set(LBDB_SQLITE_TARGET SQLite::SQLite3)
else()
  message(FATAL_ERROR "FindSQLite3 did not provide an imported target")
endif()

set(LBDB_SOURCES
    src/app.c
    src/command.c
    src/commands_bank.c
    src/commands_corpus.c
    src/commands_db.c
    src/commands_learning_report.c
    src/commands_quiz.c
    src/commands_tag_template.c
    src/database.c
    src/error.c
    src/json.c
    src/path.c
    src/schema.c
    src/sha256.c
    src/statement.c)

add_library(learn_book_db_lib STATIC ${LBDB_SOURCES})
set_target_properties(learn_book_db_lib PROPERTIES OUTPUT_NAME learn-book-db)
target_compile_features(learn_book_db_lib PRIVATE c_std_23)
target_compile_definitions(
  learn_book_db_lib
  PRIVATE _POSIX_C_SOURCE=200809L
          _XOPEN_SOURCE=700
          LBDB_VERSION_STRING="${PROJECT_VERSION}")
target_include_directories(
  learn_book_db_lib
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(learn_book_db_lib PRIVATE ${LBDB_SQLITE_TARGET})

add_executable(learn-book-db src/main.c)
target_compile_features(learn-book-db PRIVATE c_std_23)
target_include_directories(learn-book-db PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(learn-book-db PRIVATE learn_book_db_lib)

function(lbdb_enable_warnings target)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(
      ${target}
      PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2
              -Wstrict-prototypes -Wmissing-prototypes)
  endif()
endfunction()

lbdb_enable_warnings(learn_book_db_lib)
lbdb_enable_warnings(learn-book-db)

if(LBDB_ENABLE_SANITIZERS)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "LBDB_ENABLE_SANITIZERS requires GCC or Clang")
  endif()
  foreach(target learn_book_db_lib learn-book-db)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endforeach()
endif()

install(TARGETS learn-book-db RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
set(LBDB_DOC_INSTALL_DIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/learn-book-db")
install(FILES LICENSE README.md SECURITY.md DESTINATION "${LBDB_DOC_INSTALL_DIR}")
install(DIRECTORY docs/ DESTINATION "${LBDB_DOC_INSTALL_DIR}")
install(DIRECTORY examples/ DESTINATION "${LBDB_DOC_INSTALL_DIR}/examples")

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/project-version.txt" "${PROJECT_VERSION}\n")

configure_file(cmake/uninstall.cmake.in cmake_uninstall.cmake @ONLY)
add_custom_target(uninstall COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)

if(LBDB_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

set(CPACK_PACKAGE_NAME "learn-book-db")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "learn-book-db-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
include(CPack)
