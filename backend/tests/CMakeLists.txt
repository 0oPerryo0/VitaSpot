cmake_minimum_required(VERSION 3.16)
project(vam_contract_test C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# mock      -> build against backend/mock/mock_backend.c (needs SDL2)
# librespot -> link the Rust backend static library
set(VAM_TEST_BACKEND "mock" CACHE STRING "mock or librespot")
set(VAM_BACKEND_LIB "" CACHE FILEPATH "Path to libvam_backend.a (librespot backend)")
set(VAM_BACKEND_LINK_LIBS "" CACHE STRING "Extra libraries for the Rust backend")

add_executable(contract_test contract_test.c)
target_include_directories(contract_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

if(VAM_TEST_BACKEND STREQUAL "mock")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl2)
    add_library(vam_mock STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../mock/mock_backend.c)
    target_include_directories(vam_mock PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include ${SDL_INCLUDE_DIRS})
    target_link_libraries(contract_test PRIVATE vam_mock PkgConfig::SDL m)
elseif(VAM_TEST_BACKEND STREQUAL "librespot")
    if(NOT VAM_BACKEND_LIB)
        message(FATAL_ERROR "Set -DVAM_BACKEND_LIB=<path to libvam_backend.a>")
    endif()
    if(NOT VAM_BACKEND_LINK_LIBS)
        message(FATAL_ERROR "Set -DVAM_BACKEND_LINK_LIBS to the libraries the Rust backend needs")
    endif()
    target_link_libraries(contract_test PRIVATE ${VAM_BACKEND_LIB} ${VAM_BACKEND_LINK_LIBS})
else()
    message(FATAL_ERROR "VAM_TEST_BACKEND must be 'mock' or 'librespot'")
endif()

if(WIN32)
    target_link_options(contract_test PRIVATE -mconsole)
endif()

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(contract_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
