cmake_minimum_required(VERSION 3.14)
project(iPBP VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IPBP_BUILD_CLI "Build the ipbp command line tool" ON)
option(IPBP_BUILD_TESTS "Build unit tests" ON)
option(IPBP_SHARED "Build iPBP as a shared library" OFF)

set(IPBP_SOURCES
    src/capi.cpp
    src/convert.cpp
    src/io.cpp
    src/util.cpp
    src/crypto/aes.cpp
    src/crypto/ecdsa.cpp
    src/crypto/sha1.cpp
    src/psp/amctrl.cpp
    src/psp/iso9660.cpp
    src/psp/lzrc.cpp
    src/psp/sfo.cpp
)

if(IPBP_SHARED)
    add_library(ipbp SHARED ${IPBP_SOURCES})
    target_compile_definitions(ipbp PUBLIC IPBP_SHARED PRIVATE IPBP_BUILDING)
    set_target_properties(ipbp PROPERTIES CXX_VISIBILITY_PRESET hidden)
else()
    add_library(ipbp STATIC ${IPBP_SOURCES})
endif()
target_include_directories(ipbp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                       $<INSTALL_INTERFACE:include>
                                PRIVATE src)
find_package(Threads REQUIRED)
target_link_libraries(ipbp PUBLIC Threads::Threads)
if(MSVC)
    target_compile_options(ipbp PRIVATE /W4 /wd4244 /wd4267)
    target_compile_definitions(ipbp PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(ipbp PRIVATE -Wall -Wextra)
endif()

if(IPBP_BUILD_CLI)
    add_executable(ipbp_cli cli/main.cpp)
    set_target_properties(ipbp_cli PROPERTIES OUTPUT_NAME ipbp)
    target_link_libraries(ipbp_cli PRIVATE ipbp)
    if(WIN32)
        target_link_libraries(ipbp_cli PRIVATE shell32)
    endif()
    if(MSVC)
        target_compile_definitions(ipbp_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    install(TARGETS ipbp_cli RUNTIME DESTINATION bin)
endif()

if(IPBP_BUILD_TESTS)
    enable_testing()
    add_executable(ipbp_tests tests/tests.cpp)
    target_link_libraries(ipbp_tests PRIVATE ipbp)
    target_include_directories(ipbp_tests PRIVATE src)
    add_test(NAME ipbp_tests COMMAND ipbp_tests)
endif()

install(TARGETS ipbp ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ipbp DESTINATION include)
