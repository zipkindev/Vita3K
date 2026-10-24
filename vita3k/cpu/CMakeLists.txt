set(SOURCE_LIST
include/cpu/state.h
include/cpu/jit_profile.h
include/cpu/jit_preparation.h
include/cpu/common.h
include/cpu/functions.h
include/cpu/impl/dynarmic_cpu.h
include/cpu/impl/interface.h
include/cpu/disasm/functions.h
include/cpu/disasm/state.h

src/disasm.cpp
src/cpu.cpp
src/jit_profile.cpp
src/jit_preparation.cpp
src/dynarmic_cpu.cpp
)

add_library(
cpu
STATIC
${SOURCE_LIST}
)

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})

target_include_directories(cpu PUBLIC include)
target_link_libraries(cpu PUBLIC mem util)
target_link_libraries(cpu PRIVATE dynarmic capstone merry::mcl)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
    add_executable(jit-profile-tests tests/jit_profile.cpp src/jit_profile.cpp)
    target_include_directories(jit-profile-tests PRIVATE include "${PROJECT_SOURCE_DIR}/external/dynarmic/tests/A32")
    target_link_libraries(jit-profile-tests PRIVATE dynarmic merry::mcl)
    add_test(NAME jit-profile COMMAND jit-profile-tests)
    add_executable(jit-preparation-tests tests/jit_preparation.cpp src/jit_preparation.cpp)
    target_include_directories(jit-preparation-tests PRIVATE include "${PROJECT_SOURCE_DIR}/external/dynarmic/tests/A32")
    target_link_libraries(jit-preparation-tests PRIVATE dynarmic merry::mcl)
    add_test(NAME jit-preparation COMMAND jit-preparation-tests)
endif()
