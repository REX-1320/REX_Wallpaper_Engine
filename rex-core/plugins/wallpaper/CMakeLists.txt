add_library(wallpaper_plugin SHARED wallpaper_plugin.cpp)
target_include_directories(wallpaper_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(wallpaper_plugin PRIVATE $<TARGET_PROPERTY:rex-core,COMPILE_DEFINITIONS>)
