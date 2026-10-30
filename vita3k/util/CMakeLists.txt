add_library(
	util
	STATIC
	src/android_driver.cpp
	src/arm.cpp
	src/float_to_half.cpp
	src/fs_utils.cpp
	src/hash.cpp
	src/instrset_detect.cpp
	src/logging.cpp
	src/mem_snapshot.cpp
	src/net_utils.cpp
	src/jit_startup_profile.cpp
	src/pipeline_profile.cpp
	src/sync_wait_profile.cpp
	src/string_utils.cpp
	src/tracy.cpp
	src/vita_theme_utils.cpp
)

# vc_runtime_checker.cpp is directly added from the main CMakeList (for some reason adding it here doesn't work)

target_include_directories(util PUBLIC include)
target_link_libraries(util PUBLIC ${Boost_LIBRARIES} fmt spdlog http mem)
target_link_libraries(util PRIVATE libcurl crypto SDL3::SDL3)
if(ANDROID)
	target_link_libraries(util PUBLIC emuenv android xxHash::xxhash)
	target_link_libraries(util PRIVATE adrenotools)
endif()
target_compile_definitions(util PRIVATE $<$<CONFIG:Debug,RelWithDebInfo>:TRACY_ENABLE>)

find_package(Threads REQUIRED)
add_executable(sync-wait-profile-tests tests/sync_wait_profile.cpp src/sync_wait_profile.cpp)
target_include_directories(sync-wait-profile-tests PRIVATE include)
target_link_libraries(sync-wait-profile-tests PRIVATE Threads::Threads)
add_test(NAME sync-wait-profile COMMAND sync-wait-profile-tests)

add_executable(pipeline-profile-tests tests/pipeline_profile.cpp src/pipeline_profile.cpp)
target_include_directories(pipeline-profile-tests PRIVATE include)
target_link_libraries(pipeline-profile-tests PRIVATE Threads::Threads)
add_test(NAME pipeline-profile COMMAND pipeline-profile-tests)

add_executable(jit-startup-profile-tests tests/jit_startup_profile.cpp src/jit_startup_profile.cpp)
target_include_directories(jit-startup-profile-tests PRIVATE include)
target_link_libraries(jit-startup-profile-tests PRIVATE Threads::Threads)
add_test(NAME jit-startup-profile COMMAND jit-startup-profile-tests)
