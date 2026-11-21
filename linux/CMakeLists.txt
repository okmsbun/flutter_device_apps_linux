cmake_minimum_required(VERSION 3.10)
project(flutter_device_apps_linux LANGUAGES CXX)

set(PLUGIN_NAME "flutter_device_apps_linux_plugin")
add_library(${PLUGIN_NAME} SHARED flutter_device_apps_linux_plugin.cc)
apply_standard_settings(${PLUGIN_NAME})
set_target_properties(${PLUGIN_NAME} PROPERTIES CXX_VISIBILITY_PRESET hidden)
target_include_directories(${PLUGIN_NAME} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(${PLUGIN_NAME} PRIVATE flutter PkgConfig::GTK)

set(flutter_device_apps_linux_bundled_libraries "" PARENT_SCOPE)
