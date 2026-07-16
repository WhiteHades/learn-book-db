if(NOT DEFINED LBDB_BINARY_DIR OR NOT DEFINED LBDB_INSTALL_PREFIX)
  message(FATAL_ERROR "install smoke paths are required")
endif()

file(REMOVE_RECURSE "${LBDB_INSTALL_PREFIX}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${LBDB_BINARY_DIR}" --prefix "${LBDB_INSTALL_PREFIX}"
  RESULT_VARIABLE install_result
  OUTPUT_VARIABLE install_output
  ERROR_VARIABLE install_error)
if(NOT install_result EQUAL 0)
  message(FATAL_ERROR "install failed: ${install_output}${install_error}")
endif()

foreach(required_file
        bin/learn-book-db
        share/doc/learn-book-db/README.md
        share/doc/learn-book-db/examples/manifest.json
        share/doc/learn-book-db/examples/bank.json
        share/doc/learn-book-db/examples/corpus/chapter-1.md)
  if(NOT EXISTS "${LBDB_INSTALL_PREFIX}/${required_file}")
    message(FATAL_ERROR "installed package is missing ${required_file}")
  endif()
endforeach()

file(GLOB_RECURSE installed_headers "${LBDB_INSTALL_PREFIX}/include/*")
file(GLOB_RECURSE installed_libraries "${LBDB_INSTALL_PREFIX}/*.a")
file(GLOB_RECURSE installed_cmake_packages "${LBDB_INSTALL_PREFIX}/*/cmake/*")
if(installed_headers OR installed_libraries OR installed_cmake_packages)
  message(FATAL_ERROR "private headers, libraries, or CMake package files were installed")
endif()
