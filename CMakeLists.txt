cmake_minimum_required(VERSION 3.16)

project(moroder)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Prefer Homebrew/system headers over stray frameworks (e.g. Mono ships an old
# libpng/libjpeg in /Library/Frameworks that would shadow the real ones).
if(APPLE)
    set(CMAKE_FIND_FRAMEWORK LAST)
endif()

set(
    DISCORD_SDK_ROOT
    "${CMAKE_SOURCE_DIR}/lib/discord_social_sdk"
    CACHE PATH
    "Path to the Discord Social SDK"
)
set(DISCORD_SDK_LIB_DIR "${DISCORD_SDK_ROOT}/lib/release")
set(DISCORD_SDK_INCLUDE_DIR "${DISCORD_SDK_ROOT}/include")

if(APPLE)
    set(DISCORD_LIB_PATH "${DISCORD_SDK_LIB_DIR}/libdiscord_partner_sdk.dylib")
else()
    set(DISCORD_LIB_PATH "${DISCORD_SDK_LIB_DIR}/libdiscord_partner_sdk.so")
endif()
set(DISCORD_SHARED_LIB "${DISCORD_LIB_PATH}")

# MPRIS needs D-Bus, which only makes sense on Linux.
# Discord rich presence needs the (non-redistributable) Discord Social SDK.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_moroder_mpris_default ON)
else()
    set(_moroder_mpris_default OFF)
endif()
if(EXISTS "${DISCORD_LIB_PATH}")
    set(_moroder_discord_default ON)
else()
    set(_moroder_discord_default OFF)
endif()
option(MORODER_ENABLE_MPRIS "Enable MPRIS (D-Bus) support" ${_moroder_mpris_default})
option(MORODER_ENABLE_DISCORD "Enable Discord rich presence" ${_moroder_discord_default})

set(MORODER_VERSION "0.0.0-dev" CACHE STRING "Moroder version")

include(FetchContent)

FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.13.0
)
FetchContent_MakeAvailable(spdlog)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
    GIT_TAG v7.0.3
)
FetchContent_MakeAvailable(ftxui)

FetchContent_Declare(
    ftxui-image-view
    GIT_REPOSITORY https://github.com/orrnithogalum/ftxui-image-view-async.git
    GIT_TAG 1501b3263789b9b0a4aaa61143d78e02d48ad550
)
FetchContent_MakeAvailable(ftxui-image-view)
# tiv links X11 but never uses it (CImg display is disabled); there is no X11 on macOS.
if(APPLE)
    set_target_properties(tiv PROPERTIES LINK_LIBRARIES "" INTERFACE_LINK_LIBRARIES "")
endif()

# Covers: without these, CImg downloads through a curl subprocess and hands
# every JPEG to an external ImageMagick ('magick'/'convert'), so covers silently
# vanish when ImageMagick isn't installed. Decode in-process when the libs exist.
find_package(JPEG)
find_package(PNG)
find_package(CURL REQUIRED)
if(JPEG_FOUND)
    target_compile_definitions(ftxui-image-view PRIVATE cimg_use_jpeg)
    target_link_libraries(ftxui-image-view PUBLIC JPEG::JPEG)
endif()
if(PNG_FOUND)
    target_compile_definitions(ftxui-image-view PRIVATE cimg_use_png)
    target_link_libraries(ftxui-image-view PUBLIC PNG::PNG)
endif()
target_compile_definitions(ftxui-image-view PRIVATE cimg_use_curl)
target_link_libraries(ftxui-image-view PUBLIC CURL::libcurl)

if(MORODER_ENABLE_MPRIS)
    FetchContent_Declare(
        sdbus-cpp
        GIT_REPOSITORY https://github.com/Kistler-Group/sdbus-cpp.git
        GIT_TAG v2.2.1
    )
    FetchContent_MakeAvailable(sdbus-cpp)
endif()

FetchContent_Declare(
    ftxui-grid-container
    GIT_REPOSITORY https://github.com/mingsheng13/grid-container-ftxui
    GIT_TAG 5deab11f7b60ff9d75c5718b63637e7a1dbed86a
)
FetchContent_MakeAvailable(ftxui-grid-container)

set(SOURCES
    src/services/mpv.cpp
    src/services/music.cpp
    src/services/player.cpp
    src/services/notify.cpp

    src/setup/setup.cpp

    src/ui/components/carousel.cpp
    src/ui/components/chips.cpp
    src/ui/components/content.cpp
    src/ui/components/error.cpp
    src/ui/components/grid.cpp
    src/ui/components/playback_bar.cpp
    src/ui/components/search_bar.cpp
    src/ui/components/sidebar.cpp

    src/ui/constants/colors.cpp
    src/ui/cover_cache.cpp

    src/ytm/http.cpp
    src/ytm/metadata.cpp
    src/ytm/nav.cpp
    src/ytm/parsers.cpp
    src/ytm/sha1.cpp
    src/ytm/ytmusic.cpp

    src/main.cpp
)

if(MORODER_ENABLE_MPRIS)
    list(APPEND SOURCES src/services/mpris.cpp)
elseif(APPLE)
    # Same role as MPRIS: Now Playing in Control Center + media keys
    enable_language(OBJCXX)
    list(APPEND SOURCES src/services/mpris_macos.mm)
else()
    list(APPEND SOURCES src/services/mpris_stub.cpp)
endif()

# Session straight from the browser's cookie store (macOS)
if(APPLE)
    enable_language(OBJCXX)
    list(APPEND SOURCES src/macos/browser_cookies.mm)
else()
    list(APPEND SOURCES src/services/browser_cookies_stub.cpp)
endif()

if(MORODER_ENABLE_DISCORD)
    list(APPEND SOURCES src/services/social.cpp)
else()
    list(APPEND SOURCES src/services/social_stub.cpp)
endif()

# Embed default config
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen)

set(GENERATED_HEADER_NAME defaults)
set(DEFAULT_CONF ${CMAKE_CURRENT_SOURCE_DIR}/res/${PROJECT_NAME}.default.conf)
set(GENERATED_HEADER ${CMAKE_CURRENT_BINARY_DIR}/gen/${GENERATED_HEADER_NAME}.hpp)

add_custom_command(
    OUTPUT ${GENERATED_HEADER}

    COMMAND xxd -i -n ${PROJECT_NAME}_default ${DEFAULT_CONF} > ${GENERATED_HEADER}.tmp
    COMMAND sed -E "s/^(unsigned )/inline \\1/" ${GENERATED_HEADER}.tmp > ${GENERATED_HEADER}

    DEPENDS ${DEFAULT_CONF}
    COMMENT "Embedding default config into ${GENERATED_HEADER}"
    VERBATIM
)

add_custom_target(generate_${PROJECT_NAME}_conf_header ALL DEPENDS ${GENERATED_HEADER})
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(${PROJECT_NAME} ${SOURCES})
if(APPLE)
    set(MORODER_RPATH "@loader_path")
else()
    set(MORODER_RPATH "$ORIGIN")
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "${MORODER_RPATH}"
)
target_compile_definitions(${PROJECT_NAME} PRIVATE
    MORODER_VERSION="${MORODER_VERSION}"
)
add_dependencies(${PROJECT_NAME} generate_${PROJECT_NAME}_conf_header)

if(MORODER_ENABLE_DISCORD)
    target_include_directories(${PROJECT_NAME} PRIVATE ${DISCORD_SDK_INCLUDE_DIR})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${DISCORD_LIB_PATH})
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${DISCORD_SHARED_LIB}"
        $<TARGET_FILE_DIR:${PROJECT_NAME}>
    )
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE MORODER_NO_DISCORD)
endif()

if(MORODER_ENABLE_MPRIS)
    target_link_libraries(${PROJECT_NAME} PRIVATE sdbus-c++)
else()
    target_compile_definitions(${PROJECT_NAME} PRIVATE MORODER_NO_MPRIS)
    if(APPLE)
        set_source_files_properties(src/services/mpris_macos.mm src/macos/browser_cookies.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(${PROJECT_NAME} PRIVATE
            "-framework Foundation" "-framework AppKit" "-framework MediaPlayer" "-framework Security" sqlite3)
    endif()
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(MPV REQUIRED IMPORTED_TARGET mpv)
find_package(CURL REQUIRED)

target_link_libraries(${PROJECT_NAME}
    PRIVATE

    spdlog::spdlog
    nlohmann_json::nlohmann_json

    ftxui::screen
    ftxui::dom
    ftxui::component
    ftxui-image-view
    ftxui-grid-container

    PkgConfig::MPV
    CURL::libcurl
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        MORODER_LOG_PATH="${CMAKE_SOURCE_DIR}/logs"
    )
else()
    target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        MORODER_LOG_PATH=".local/state/moroder/logs"
    )
endif()

target_compile_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-O3>
)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# macOS notifications: a tiny helper app, "Moroder.app", so notifications carry
# Moroder's icon and the track's cover (osascript shows Script Editor's icon).
# The icon is drawn at build time; the bundle is ad-hoc signed.
if(APPLE)
    set(MORODER_HELPER_APP "${CMAKE_BINARY_DIR}/Moroder.app")

    add_executable(moroder-make-icon tools/make_icon.m)
    set_target_properties(moroder-make-icon PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(moroder-make-icon PRIVATE "-framework AppKit")

    set(ICONSET "${CMAKE_BINARY_DIR}/Moroder.iconset")
    set(ICNS "${CMAKE_BINARY_DIR}/Moroder.icns")
    set(icon_cmds "")
    foreach(sz 16 32 128 256 512)
        math(EXPR sz2 "${sz} * 2")
        list(APPEND icon_cmds
            COMMAND $<TARGET_FILE:moroder-make-icon> ${sz}  ${ICONSET}/icon_${sz}x${sz}.png
            COMMAND $<TARGET_FILE:moroder-make-icon> ${sz2} ${ICONSET}/icon_${sz}x${sz}@2x.png)
    endforeach()
    add_custom_command(
        OUTPUT ${ICNS}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${ICONSET}
        ${icon_cmds}
        COMMAND iconutil -c icns ${ICONSET} -o ${ICNS}
        DEPENDS moroder-make-icon
        COMMENT "Drawing the Moroder icon")

    add_executable(moroder-notify src/macos/notify_helper.m)
    set_target_properties(moroder-notify PROPERTIES
        COMPILE_OPTIONS "-fobjc-arc"
        RUNTIME_OUTPUT_DIRECTORY "${MORODER_HELPER_APP}/Contents/MacOS")
    target_link_libraries(moroder-notify PRIVATE "-framework AppKit")

    add_custom_target(moroder-helper-app ALL
        COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/res/macos/Info.plist ${MORODER_HELPER_APP}/Contents/Info.plist
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MORODER_HELPER_APP}/Contents/Resources
        COMMAND ${CMAKE_COMMAND} -E copy ${ICNS} ${MORODER_HELPER_APP}/Contents/Resources/Moroder.icns
        COMMAND codesign --force --sign - ${MORODER_HELPER_APP}
        DEPENDS moroder-notify ${ICNS}
        COMMENT "Assembling Moroder.app (notification helper)")

    add_dependencies(${PROJECT_NAME} moroder-helper-app)

    # Next to the binary's prefix: <prefix>/share/moroder/Moroder.app
    install(DIRECTORY ${MORODER_HELPER_APP} DESTINATION share/moroder USE_SOURCE_PERMISSIONS)
endif()
