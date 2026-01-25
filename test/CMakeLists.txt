cmake_minimum_required(VERSION 3.20)
project(unit-test CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VCPKG_TRIPLET "x64-linux" CACHE STRING "vcpkg x64-linux")
set(CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg_installed/${VCPKG_TRIPLET} ${CMAKE_PREFIX_PATH})
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")


# 引入vcpkg依赖
find_package(Boost REQUIRED COMPONENTS asio beast thread)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(RdKafka CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(etcd-cpp-api CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)


# 测试的文件
file(GLOB TEST_FILES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

file(GLOB MQ_FILES ${CMAKE_CURRENT_SOURCE_DIR}/../src/messagequeue/*.cpp)

file(GLOB PROTO_FILES ${CMAKE_CURRENT_SOURCE_DIR}/../src/proto/*.cc)
file(GLOB_RECURSE SERVICE_FILES ${CMAKE_CURRENT_SOURCE_DIR}/../src/services/*.cpp)

add_executable(tests ${TEST_FILES} ${MQ_FILES} ${PROTO_FILES} ${SERVICE_FILES})
target_include_directories(tests PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../src/proto)


# 链接核心库
target_link_libraries(tests PUBLIC 
# Boost组件
Boost::asio Boost::beast Boost::thread

# 日志spdlog
spdlog::spdlog 
nlohmann_json::nlohmann_json 

# MongoDB
mongo::mongocxx_static
mongo::bsoncxx_static
utf8proc::utf8proc

# Kafka
RdKafka::rdkafka RdKafka::rdkafka++

# gRPC
gRPC::grpc++

# etcd
etcd-cpp-api

# gtest
GTest::gtest_main

)



