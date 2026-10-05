# Build du module Python abbalone_cpp._engine (pybind11) à partir du cœur
# bitboard existant — les sources C++ ne sont pas dupliquées.

cmake_minimum_required(VERSION 3.16)
project(abbalone_cpp LANGUAGES CXX)

find_package(pybind11 CONFIG REQUIRED)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../Sources/BitboardAbalone)

pybind11_add_module(_engine MODULE
  abbalone_cpp.cpp
  ${BB_DIR}/bb_tables.cpp
  ${BB_DIR}/bb_move.cpp
  ${BB_DIR}/bb_eval.cpp
  ${BB_DIR}/bb_search.cpp
  ${BB_DIR}/bb_perft.cpp
  ${BB_DIR}/bb_atp.cpp
  ${BB_DIR}/bb_tt.cpp
)
target_include_directories(_engine PRIVATE ${BB_DIR})

# scikit-build-core installe sous CMAKE_INSTALL_PREFIX (racine de la wheel).
install(TARGETS _engine LIBRARY DESTINATION abbalone_cpp)
