# SPDX-License-Identifier: GPL-3.0-or-later
# Loaded with CMAKE_PROJECT_yuzu_INCLUDE in an already configured Lichii build.
function(fefw_add_archive_helper)
    if(NOT FEFW_ARCHIVE_PYTHON)
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(FEFW_ARCHIVE_PYTHON "${Python3_EXECUTABLE}")
    endif()
    get_filename_component(helper_dir "${FEFW_ARCHIVE_SOURCE}" DIRECTORY)
    set(adapter "${CMAKE_BINARY_DIR}/fefw-archive-key-manager.cpp")
    execute_process(COMMAND "${FEFW_ARCHIVE_PYTHON}" "${helper_dir}/prepare_reader.py"
        "${PROJECT_SOURCE_DIR}/src/core/crypto/key_manager.cpp" "${adapter}"
        COMMAND_ERROR_IS_FATAL ANY)
    add_executable(fefw-archive EXCLUDE_FROM_ALL "${FEFW_ARCHIVE_SOURCE}" "${adapter}")
    target_link_libraries(fefw-archive PRIVATE core nlohmann_json::nlohmann_json)
    target_include_directories(fefw-archive PRIVATE "${PROJECT_SOURCE_DIR}/src")
    if(MINGW)
        target_link_options(fefw-archive PRIVATE -municode)
    endif()
    target_compile_features(fefw-archive PRIVATE cxx_std_20)
endfunction()
cmake_language(DEFER CALL fefw_add_archive_helper)
