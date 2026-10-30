add_library(
    codec
    STATIC
    include/codec/state.h
    include/codec/types.h
    src/atrac9.cpp
    src/decoder.cpp
    src/aac.cpp
    src/h264.cpp
    src/mjpeg.cpp
    src/mp3.cpp
    src/pcm.cpp
    src/player.cpp
)

target_include_directories(codec PUBLIC include)
target_link_libraries(codec PRIVATE ffmpeg libatrac9 util) 

add_executable(atrac9-span-tests tests/atrac9_span.cpp)
target_link_libraries(atrac9-span-tests PRIVATE codec)
add_test(NAME atrac9-input-span COMMAND atrac9-span-tests)
