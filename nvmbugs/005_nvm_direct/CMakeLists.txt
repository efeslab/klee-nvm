cmake_minimum_required(VERSION 3.1)
 

file(GLOB ORIGINAL_SOURCES "original_lib/*.c")
file(GLOB ORIGINAL_HEADERS "original_lib/*.h")
set(ORIGINAL_SRC ${ORIGINAL_SOURCES} ${ORIGINAL_HEADERS})

file(GLOB LIB_SOURCES "lib/*.c")
file(GLOB LIB_HEADERS "lib/*.h")
set(LIB_SRC ${LIB_SOURCES} ${LIB_HEADERS})

file(GLOB NO_FC_SOURCES "no_fc_lib/*.c")
file(GLOB NO_FC_HEADERS "no_fc_lib/*.h")
set(NO_FC_SRC ${NO_FC_SOURCES} ${NO_FC_HEADERS})

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O1 -pthread -Xclang -disable-llvm-passes -D__NO_STRING_INLINES -D_FORTIFY_SOURCE=0 -U__OPTIMIZE__")

set(TINY_SRC ${LIB_SRC} tiny.c)
set(TINY_ORIGINAL ${ORIGINAL_SRC} tiny.c)
set(TINY_NO_FC ${NO_FC_SRC} tiny.c)
klee_add_test_executable(TARGET 005_Tiny SOURCES ${TINY_SRC})
klee_add_test_executable(TARGET 005_TinyOriginal SOURCES ${TINY_ORIGINAL})
klee_add_test_executable(TARGET 005_TinyNoFC SOURCES ${TINY_NO_FC})

set(SMALL_SRC ${LIB_SRC} small.c)
set(SMALL_ORIGINAL ${ORIGINAL_SRC} small.c)
set(SMALL_NO_FC ${NO_FC_SRC} small.c)
klee_add_test_executable(TARGET 005_Small SOURCES ${SMALL_SRC})
klee_add_test_executable(TARGET 005_SmallOriginal SOURCES ${SMALL_ORIGINAL})
klee_add_test_executable(TARGET 005_SmallNoFC SOURCES ${SMALL_NO_FC})

set(ALLOCS_SRC ${LIB_SRC} allocs.c)
set(ALLOCS_ORIGINAL ${ORIGINAL_SRC} allocs.c)
set(ALLOCS_NO_FC ${NO_FC_SRC} allocs.c)
klee_add_test_executable(TARGET 005_Allocs SOURCES ${ALLOCS_SRC})
klee_add_test_executable(TARGET 005_AllocsOriginal SOURCES ${ALLOCS_ORIGINAL})
klee_add_test_executable(TARGET 005_AllocsNoFC SOURCES ${ALLOCS_NO_FC})
