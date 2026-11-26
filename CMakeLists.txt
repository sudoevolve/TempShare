cmake_minimum_required(VERSION 3.20)
project(TempShare VERSION 0.3.0 LANGUAGES C CXX)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
include(FetchContent)
include(CTest)
find_package(Threads REQUIRED)
option(TEMPSHARE_TLS "Enable HTTPS with OpenSSL and a user-provided certificate" OFF)

FetchContent_Declare(aino
    GIT_REPOSITORY https://github.com/sudoevolve/aino.git
    GIT_TAG 74d86d49b8c8848d5a8b3b9c75f917a23795d247
    SOURCE_SUBDIR tempshare-unused)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG 7800c3dc5b33285cbb18baa5998de4ff7ef63515
    SOURCE_SUBDIR tempshare-unused)
FetchContent_Declare(qrcode
    GIT_REPOSITORY https://github.com/nayuki/QR-Code-generator.git
    GIT_TAG 7ad95cedd8464a87f82221283612732ae4f3f305
    SOURCE_SUBDIR tempshare-unused)
FetchContent_Declare(miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG 174573d60290f447c13a2b1b3405de2b96e27d6c)
FetchContent_MakeAvailable(aino httplib qrcode miniz)
target_compile_definitions(miniz PRIVATE MINIZ_NO_ZLIB_COMPATIBLE_NAMES)

foreach(asset IN ITEMS index.html styles.css app.js appearance.js cast.html cast-host.js rtc.js resume.js)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/src/web/${asset}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" "${asset}")
endforeach()
set(icon "${CMAKE_CURRENT_SOURCE_DIR}/src/web/icon.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${icon}")
file(READ "${icon}" icon_hex HEX)
string(REGEX REPLACE "(..)" "0x\\1," icon_bytes "${icon_hex}")
configure_file(src/core/web_assets.hpp.in generated/web_assets.hpp @ONLY)

add_library(tempshare_core STATIC src/core/session.cpp src/core/http_server.cpp src/core/discovery.cpp
    src/core/folder_archive.cpp src/core/shared_source.cpp)
target_compile_features(tempshare_core PUBLIC cxx_std_17)
target_include_directories(tempshare_core PUBLIC src PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_include_directories(tempshare_core SYSTEM PRIVATE "${httplib_SOURCE_DIR}")
target_link_libraries(tempshare_core PRIVATE Threads::Threads miniz)
if(WIN32)
    target_link_libraries(tempshare_core PRIVATE ws2_32 iphlpapi bcrypt)
endif()
if(TEMPSHARE_TLS)
    find_package(OpenSSL 3 REQUIRED)
    target_compile_definitions(tempshare_core PUBLIC CPPHTTPLIB_OPENSSL_SUPPORT)
    target_link_libraries(tempshare_core PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

add_executable(tempshare src/client/main.cpp src/client/platform.cpp "${qrcode_SOURCE_DIR}/cpp/qrcodegen.cpp")
target_compile_definitions(tempshare PRIVATE TEMPSHARE_VERSION="${PROJECT_VERSION}")
target_include_directories(tempshare SYSTEM PRIVATE "${aino_SOURCE_DIR}" "${qrcode_SOURCE_DIR}/cpp")
target_link_libraries(tempshare PRIVATE tempshare_core Threads::Threads)

if(WIN32)
    enable_language(RC)
    target_sources(tempshare PRIVATE packaging/windows/tempshare.rc)
    set_target_properties(tempshare PROPERTIES WIN32_EXECUTABLE TRUE)
    if(MINGW)
        target_link_options(tempshare PRIVATE -municode)
    endif()
    target_link_libraries(tempshare PRIVATE user32 gdi32 ole32 windowscodecs shell32 comctl32 uuid advapi32)
elseif(APPLE)
    enable_language(OBJCXX)
    set_source_files_properties(src/client/main.cpp src/client/platform.cpp PROPERTIES LANGUAGE OBJCXX)
    set_target_properties(tempshare PROPERTIES OBJCXX_STANDARD 17 OBJCXX_STANDARD_REQUIRED YES)
    target_link_libraries(tempshare PRIVATE "-framework Cocoa" "-framework CoreText"
        "-framework CoreGraphics" "-framework ImageIO" "-framework ApplicationServices")
else()
    find_package(X11 REQUIRED)
    target_link_libraries(tempshare PRIVATE X11::X11 ${CMAKE_DL_LIBS})
endif()

foreach(target IN ITEMS tempshare tempshare_core)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

install(TARGETS tempshare RUNTIME DESTINATION bin)
install(FILES LICENSE README.md DESTINATION share/tempshare)
install(FILES docs/http-api.md docs/lan-https.md docs/ux-review.md DESTINATION share/tempshare/docs)
install(FILES "${aino_SOURCE_DIR}/LICENSE" DESTINATION share/tempshare/licenses RENAME aino.txt)
install(FILES "${httplib_SOURCE_DIR}/LICENSE" DESTINATION share/tempshare/licenses RENAME cpp-httplib.txt)
install(FILES "${qrcode_SOURCE_DIR}/Readme.markdown" DESTINATION share/tempshare/licenses RENAME QR-Code-generator.txt)
install(FILES "${miniz_SOURCE_DIR}/LICENSE" DESTINATION share/tempshare/licenses RENAME miniz.txt)

if(BUILD_TESTING)
    add_executable(tempshare_integration tests/integration.cpp)
    target_include_directories(tempshare_integration SYSTEM PRIVATE "${httplib_SOURCE_DIR}")
    target_link_libraries(tempshare_integration PRIVATE tempshare_core Threads::Threads)
    if(WIN32)
        target_link_libraries(tempshare_integration PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(tempshare_integration PRIVATE /utf-8)
    endif()
    add_test(NAME share_roundtrip COMMAND tempshare_integration)
    set_tests_properties(share_roundtrip PROPERTIES TIMEOUT 30)
    if(WIN32)
        add_test(NAME desktop_gui_subsystem COMMAND
            powershell -NoProfile -Command "& { param([string]$exe) $b=[IO.File]::ReadAllBytes($exe); $pe=[BitConverter]::ToInt32($b,60); if ([BitConverter]::ToUInt16($b,$pe+92) -ne 2) { exit 1 } }" "$<TARGET_FILE:tempshare>")
    endif()
endif()
