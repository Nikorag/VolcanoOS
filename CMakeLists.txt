cmake_minimum_required(VERSION 3.20)
project(VolcanoOS VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  # libarchive is keg-only on Homebrew.
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew/opt/libarchive /usr/local/opt/libarchive /opt/homebrew /usr/local)
endif()

find_package(SDL2 REQUIRED CONFIG)
find_package(SDL2_ttf REQUIRED CONFIG)
find_package(SDL2_image REQUIRED CONFIG)
find_package(CURL REQUIRED)
find_package(LibArchive REQUIRED)

set(VOS_SOURCES
  src/main.cpp
  src/app.cpp
  src/app_ui.cpp
  src/util.cpp
  src/sha256.cpp
  src/http.cpp
  src/api.cpp
  src/auth.cpp
  src/tokenstore.cpp
  src/installer.cpp
  src/extract.cpp
  src/runner.cpp
  src/webview.cpp
  src/input.cpp
  src/gfx.cpp
  src/art.cpp
  src/background.cpp
  src/widgets.cpp
  src/osk.cpp
  src/pages.cpp
  src/bridge.cpp
  src/overlay.cpp
)
if(APPLE)
  list(APPEND VOS_SOURCES src/webview_mac.mm src/overlay_mac.mm src/tokenstore_mac.cpp)
endif()

add_executable(VolcanoOS ${VOS_SOURCES})
if(APPLE)
  set_source_files_properties(src/webview_mac.mm src/overlay_mac.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
endif()
target_include_directories(VolcanoOS PRIVATE src third_party ${LibArchive_INCLUDE_DIRS})
target_compile_definitions(VolcanoOS PRIVATE
  VOS_VERSION="${PROJECT_VERSION}"
  VOS_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")

if(MSVC)
  target_compile_options(VolcanoOS PRIVATE /W3 /utf-8)
else()
  target_compile_options(VolcanoOS PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

target_link_libraries(VolcanoOS PRIVATE
  SDL2::SDL2 SDL2_ttf::SDL2_ttf SDL2_image::SDL2_image
  CURL::libcurl ${LibArchive_LIBRARIES})

if(TARGET SDL2::SDL2main AND WIN32)
  target_link_libraries(VolcanoOS PRIVATE SDL2::SDL2main)
endif()
if(WIN32)
  target_link_libraries(VolcanoOS PRIVATE ws2_32 bcrypt)
endif()
if(APPLE)
  target_link_libraries(VolcanoOS PRIVATE
    "-framework Cocoa" "-framework WebKit" "-framework Security" "-framework QuartzCore" "-framework CoreFoundation")
endif()
if(UNIX AND NOT APPLE)
  target_link_libraries(VolcanoOS PRIVATE pthread)
endif()

# Ship the fonts beside the binary so the build tree is relocatable.
add_custom_command(TARGET VolcanoOS POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:VolcanoOS>/assets)

install(TARGETS VolcanoOS RUNTIME DESTINATION bin)
install(DIRECTORY assets DESTINATION bin)
