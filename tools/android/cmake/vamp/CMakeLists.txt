# The Vamp SDK ships a hand-written Makefile and an autoconf script that only
# probes for the example host's dependencies; this builds the two libraries
# Ardour uses (the plugin SDK and the host SDK) from the same source lists.
cmake_minimum_required(VERSION 3.10)
project(vamp-plugin-sdk C CXX)

add_library(vamp-sdk
	src/vamp-sdk/PluginAdapter.cpp
	src/vamp-sdk/RealTime.cpp
	src/vamp-sdk/FFT.cpp
	src/vamp-sdk/acsymbols.c)
target_include_directories(vamp-sdk PRIVATE . src)

add_library(vamp-hostsdk
	src/vamp-hostsdk/Files.cpp
	src/vamp-hostsdk/PluginHostAdapter.cpp
	src/vamp-hostsdk/RealTime.cpp
	src/vamp-hostsdk/PluginBufferingAdapter.cpp
	src/vamp-hostsdk/PluginChannelAdapter.cpp
	src/vamp-hostsdk/PluginInputDomainAdapter.cpp
	src/vamp-hostsdk/PluginLoader.cpp
	src/vamp-hostsdk/PluginSummarisingAdapter.cpp
	src/vamp-hostsdk/PluginWrapper.cpp
	src/vamp-hostsdk/host-c.cpp
	src/vamp-hostsdk/acsymbols.c)
target_include_directories(vamp-hostsdk PRIVATE . src)
target_link_libraries(vamp-hostsdk dl)

install(TARGETS vamp-sdk vamp-hostsdk LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(DIRECTORY vamp vamp-sdk vamp-hostsdk DESTINATION include)

foreach(pc vamp vamp-sdk vamp-hostsdk)
	file(READ pkgconfig/${pc}.pc.in PC)
	string(REPLACE "%PREFIX%" "${CMAKE_INSTALL_PREFIX}" PC "${PC}")
	file(WRITE ${CMAKE_BINARY_DIR}/${pc}.pc "${PC}")
	install(FILES ${CMAKE_BINARY_DIR}/${pc}.pc DESTINATION lib/pkgconfig)
endforeach()
