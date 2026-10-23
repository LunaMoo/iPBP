cmake_minimum_required(VERSION 3.14)
project(iPBP VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IPBP_BUILD_CLI "Build the ipbp command line tool" ON)
option(IPBP_BUILD_TESTS "Build unit tests" ON)
option(IPBP_SHARED "Build iPBP as a shared library" OFF)
# PSOne EBOOTs need zlib: an existing target (e.g. a parent project's bundled zlib), else the system's.
set(IPBP_ZLIB_TARGET "" CACHE STRING "zlib target to link for PSOne EBOOT support (empty: find_package(ZLIB))")

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
    src/psx.cpp
)

function(ipbp_configure target)
    target_include_directories(${target} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                                                $<INSTALL_INTERFACE:include>
                                         PRIVATE src)
    target_link_libraries(${target} PUBLIC Threads::Threads)
    if(IPBP_ZLIB)
        target_link_libraries(${target} PRIVATE ${IPBP_ZLIB})
        target_compile_definitions(${target} PRIVATE IPBP_WITH_PSX)
    endif()
    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /wd4244 /wd4267 /utf-8)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

find_package(Threads REQUIRED)
if(IPBP_ZLIB_TARGET AND TARGET ${IPBP_ZLIB_TARGET})
    set(IPBP_ZLIB ${IPBP_ZLIB_TARGET})
else()
    find_package(ZLIB)
    if(ZLIB_FOUND)
        set(IPBP_ZLIB ZLIB::ZLIB)
    else()
        message(STATUS "iPBP: zlib not found, building without PSOne EBOOT support")
    endif()
endif()

if(IPBP_SHARED)
    add_library(ipbp SHARED ${IPBP_SOURCES})
    target_compile_definitions(ipbp PUBLIC IPBP_SHARED PRIVATE IPBP_BUILDING)
    set_target_properties(ipbp PROPERTIES CXX_VISIBILITY_PRESET hidden)
    # The shared library exports only the C API, while the CLI and the tests use the C++ API,
    # so they link a static build of the same sources.
    add_library(ipbp_static STATIC ${IPBP_SOURCES})
    ipbp_configure(ipbp_static)
    set(IPBP_CPP_LIB ipbp_static)
else()
    add_library(ipbp STATIC ${IPBP_SOURCES})
    set(IPBP_CPP_LIB ipbp)
endif()
ipbp_configure(ipbp)
# iPBP.dll / iPBP.so / iPBP.dylib / iPBP.lib / iPBP.a everywhere, without the platform's "lib" prefix.
set_target_properties(ipbp PROPERTIES OUTPUT_NAME iPBP PREFIX "" IMPORT_PREFIX "")

if(IPBP_BUILD_CLI)
    add_executable(ipbp_cli cli/main.cpp)
    set_target_properties(ipbp_cli PROPERTIES OUTPUT_NAME ipbp)
    target_link_libraries(ipbp_cli PRIVATE ${IPBP_CPP_LIB})
    if(WIN32)
        target_link_libraries(ipbp_cli PRIVATE shell32)
        target_compile_definitions(ipbp_cli PRIVATE NOMINMAX)
    endif()
    if(MSVC)
        target_compile_definitions(ipbp_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    install(TARGETS ipbp_cli RUNTIME DESTINATION bin)
endif()

if(IPBP_BUILD_TESTS)
    enable_testing()
    add_executable(ipbp_tests tests/tests.cpp)
    target_link_libraries(ipbp_tests PRIVATE ${IPBP_CPP_LIB})
    target_include_directories(ipbp_tests PRIVATE src)
    add_test(NAME ipbp_tests COMMAND ipbp_tests)
endif()

install(TARGETS ipbp ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY include/ipbp DESTINATION include)
