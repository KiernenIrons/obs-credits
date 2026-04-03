cmake_minimum_required(VERSION 3.16)
project(obs-credits VERSION 1.0.0)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(libobs REQUIRED)

add_library(obs-credits MODULE
	src/plugin-main.c
	src/credits-source.c
	src/credits-parser.c
	src/credits-renderer.c
	src/discord-fetch.c
	src/youtube-chat.c
	deps/cJSON/cJSON.c
)

target_include_directories(obs-credits PRIVATE deps/cJSON deps/curl)
target_link_libraries(obs-credits PRIVATE OBS::libobs OBS::libcurl OBS::obs-frontend-api)
target_compile_definitions(obs-credits PRIVATE OBS_CREDITS_VERSION="${PROJECT_VERSION}")

# Platform-specific install rules
if(WIN32)
	set(_bin_dest "bin/64bit")
else()
	set(_bin_dest "bin")
endif()

install(TARGETS obs-credits
	LIBRARY DESTINATION "${_bin_dest}"
	RUNTIME DESTINATION "${_bin_dest}"
)

install(DIRECTORY data/
	DESTINATION "data"
)
