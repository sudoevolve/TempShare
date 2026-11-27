cmake_minimum_required(VERSION 3.20)
project(TempShare VERSION 0.4.0 LANGUAGES C CXX)

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
# 固定 Aino 提交的最小关闭请求钩子。所有替换必须命中；上游变化时配置失败，避免静默丢失退出确认。
file(READ "${aino_SOURCE_DIR}/aino.hpp" tempshare_aino)
function(tempshare_aino_replace before after)
    string(FIND "${tempshare_aino}" "${before}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Pinned Aino close hook no longer matches its source")
    endif()
    string(REPLACE "${before}" "${after}" tempshare_aino "${tempshare_aino}")
    set(tempshare_aino "${tempshare_aino}" PARENT_SCOPE)
endfunction()
tempshare_aino_replace(
    "    bool should_close() const;"
    "    void set_close_handler(std::function<void()> callback);\n    bool should_close() const;")
tempshare_aino_replace(
    "    std::atomic<bool> running{true};"
    "    std::function<void()> close_handler;\n    bool request_close() {\n        if (close_handler) { close_handler(); need_frame = true; wake_signal.notify(); return false; }\n        running = false; wake_signal.notify(); return true;\n    }\n    std::atomic<bool> running{true};")
tempshare_aino_replace(
    "bool  Window::should_close() const"
    "void Window::set_close_handler(std::function<void()> callback) { impl->close_handler = std::move(callback); }\nbool  Window::should_close() const")
tempshare_aino_replace(
    "case WM_CLOSE:\n        // WM_QUIT 属于整个线程；关闭本窗口不能结束同线程的其它窗口。\n        s->owner->running = false;"
    "case WM_CLOSE:\n        s->owner->request_close();")
tempshare_aino_replace(
    "if (Atom(e.xclient.data.l[0]) == s->wm_delete) { o->running = false; return false; }"
    "if (Atom(e.xclient.data.l[0]) == s->wm_delete) { if (o->request_close()) return false; break; }")
tempshare_aino_replace(
    "if (s->title_pressed == 1) s->owner->running = false;"
    "if (s->title_pressed == 1) s->owner->request_close();")
tempshare_aino_replace(
    "[](void* data, wl::Proxy*) { static_cast<WaylandState*>(data)->owner->running = false; }"
    "[](void* data, wl::Proxy*) { static_cast<WaylandState*>(data)->owner->request_close(); }")
tempshare_aino_replace(
    "inline void cocoa_window_closed(id self, SEL, id) {"
    "inline BOOL cocoa_window_should_close(id self, SEL, id) {\n    if (auto* s = cocoa_state_of(self)) return s->owner->request_close() ? YES : NO;\n    return YES;\n}\ninline void cocoa_window_closed(id self, SEL, id) {")
tempshare_aino_replace(
    "class_addMethod(view_cls, sel_registerName(\"windowWillClose:\"), reinterpret_cast<IMP>(cocoa_window_closed), \"v@:@\");"
    "class_addMethod(view_cls, sel_registerName(\"windowShouldClose:\"), reinterpret_cast<IMP>(cocoa_window_should_close), \"c@:@\");\n        class_addMethod(view_cls, sel_registerName(\"windowWillClose:\"), reinterpret_cast<IMP>(cocoa_window_closed), \"v@:@\");")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/aino")
set(tempshare_aino_path "${CMAKE_CURRENT_BINARY_DIR}/generated/aino/aino.hpp")
if(EXISTS "${tempshare_aino_path}")
    file(READ "${tempshare_aino_path}" tempshare_previous_aino)
endif()
if(NOT tempshare_aino STREQUAL tempshare_previous_aino)
    file(WRITE "${tempshare_aino_path}" "${tempshare_aino}")
endif()
unset(tempshare_aino)
unset(tempshare_previous_aino)
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
    src/core/folder_archive.cpp src/core/shared_source.cpp src/core/peer_link.cpp)
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
target_include_directories(tempshare SYSTEM BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/aino")
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
install(FILES docs/http-api.md docs/lan-https.md docs/ux-review.md docs/lan-interconnection-review.md
    DESTINATION share/tempshare/docs)
install(FILES "${aino_SOURCE_DIR}/LICENSE" DESTINATION share/tempshare/licenses RENAME aino.txt)
install(FILES "${httplib_SOURCE_DIR}/LICENSE" DESTINATION share/tempshare/licenses RENAME cpp-httplib.txt)
install(FILES "${qrcode_SOURCE_DIR}/Readme.markdown" DESTINATION share/tempshare/licenses RENAME QR-Code-generator.txt)
install(FILES "${miniz_SOURCE_DIR}/LICENSE" DESTINATION share/tempshare/licenses RENAME miniz.txt)

if(BUILD_TESTING)
    add_executable(tempshare_browser_fixture tests/browser_fixture.cpp)
    target_link_libraries(tempshare_browser_fixture PRIVATE tempshare_core Threads::Threads)
    if(MSVC)
        target_compile_options(tempshare_browser_fixture PRIVATE /utf-8)
    endif()
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
    set_tests_properties(share_roundtrip PROPERTIES TIMEOUT 60)
    if(WIN32)
        add_test(NAME desktop_gui_subsystem COMMAND
            powershell -NoProfile -Command "& { param([string]$exe) $b=[IO.File]::ReadAllBytes($exe); $pe=[BitConverter]::ToInt32($b,60); if ([BitConverter]::ToUInt16($b,$pe+92) -ne 2) { exit 1 } }" "$<TARGET_FILE:tempshare>")
    endif()
endif()
