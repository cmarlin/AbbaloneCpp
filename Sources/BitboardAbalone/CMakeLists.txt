cmake_minimum_required(VERSION 3.16)
project(BitboardAbalone CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(bb_core STATIC
  bb_tables.cpp
  bb_move.cpp
  bb_eval.cpp
  bb_search.cpp
  bb_perft.cpp
  bb_atp.cpp
  bb_tt.cpp
)
target_include_directories(bb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

enable_testing()

add_executable(test_bb tests/test_bb.cpp)
target_include_directories(test_bb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_bb PRIVATE bb_core)
add_test(NAME test_bb COMMAND test_bb)

add_executable(test_eval tests/test_eval.cpp)
target_include_directories(test_eval PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_eval PRIVATE bb_core)
add_test(NAME test_eval COMMAND test_eval)

add_executable(test_search tests/test_search.cpp)
target_include_directories(test_search PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_search PRIVATE bb_core)
add_test(NAME test_search COMMAND test_search)

add_executable(test_tt tests/test_tt.cpp)
target_include_directories(test_tt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_tt PRIVATE bb_core)
add_test(NAME test_tt COMMAND test_tt)

add_executable(test_id tests/test_id.cpp)
target_include_directories(test_id PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(test_id PRIVATE bb_core)
add_test(NAME test_id COMMAND test_id)

add_executable(test_atp tests/test_atp.cpp
  ../SpielAbalone/abalone_core.cpp
  ../SpielAbalone/abalone_boards.cpp
)
target_include_directories(test_atp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_BINARY_DIR}/gen
)
target_link_libraries(test_atp PRIVATE bb_core)
add_test(NAME test_atp COMMAND test_atp)

add_executable(atp_engine_bb atp_engine_bb.cpp)
target_link_libraries(atp_engine_bb PRIVATE bb_core)

add_executable(bb_bench bb_bench.cpp)
target_link_libraries(bb_bench PRIVATE bb_core)

# Cross-validation against the reference core (Sources/SpielAbalone).
# The reference sources include headers via the OpenSpiel layout; mirror
# them into the build directory so that path resolves.
set(AB_INCLUDE_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen/open_spiel/games/abalone)
file(MAKE_DIRECTORY ${AB_INCLUDE_DIR})
foreach(HEADER abalone_core.h abalone_boards.h)
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/../SpielAbalone/${HEADER}
                ${AB_INCLUDE_DIR}/${HEADER} COPYONLY)
endforeach()

add_executable(test_perft tests/test_perft.cpp
  ../SpielAbalone/abalone_core.cpp
  ../SpielAbalone/abalone_boards.cpp
)
target_include_directories(test_perft PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_BINARY_DIR}/gen
)
target_link_libraries(test_perft PRIVATE bb_core)
add_test(NAME test_perft COMMAND test_perft)
