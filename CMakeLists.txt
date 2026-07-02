cmake_minimum_required(VERSION 3.16)
project(SkimNanoAOD LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ROOT REQUIRED COMPONENTS RIO Tree RDataFrame ROOTVecOps)
find_package(nlohmann_json REQUIRED)

add_executable(skim_nanoaod src/skim_nanoaod.cpp)
target_link_libraries(skim_nanoaod PRIVATE ROOT::RIO ROOT::Tree ROOT::RDataFrame ROOT::ROOTVecOps nlohmann_json::nlohmann_json)
