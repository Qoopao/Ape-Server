cmake_minimum_required(VERSION 3.20)
project(im-system CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VCPKG_TRIPLET "x64-linux" CACHE STRING "vcpkg x64-linux")
set(CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TRIPLET} ${CMAKE_PREFIX_PATH})
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

# 引入vcpkg依赖
find_package(Boost REQUIRED COMPONENTS asio beast thread uuid)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(RdKafka CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(etcd-cpp-api CONFIG REQUIRED)

# 收集所有 src 下的 .cpp
file(GLOB_RECURSE ALL_IMPL_FILE "src/*.cpp")

# 收集src下protobuf生成的cc文件
file(GLOB_RECURSE PROTO_CC_FILE "src/proto/*.cc")

list(APPEND ALL_IMPL_FILE ${PROTO_CC_FILE})

message(STATUS "Collected CPP files: ${ALL_IMPL_FILE}")
list(REMOVE_ITEM ALL_IMPL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")  # 移除main.cpp

# 核心库
add_library(ape-server STATIC ${ALL_IMPL_FILE})
target_include_directories(ape-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/proto)
target_link_libraries(ape-server PUBLIC 
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

#brpc
brpc
)

get_target_property(APE_SERVER_LINK_LIBS ape-server LINK_LIBRARIES)
message(STATUS "=== ape-server 链接的库 ===")
message(STATUS "LINK_LIBRARIES: ${APE_SERVER_LINK_LIBS}")

# 网络库
# add_library(im-network STATIC src/network/WebSocketServer.cpp src/network/Protocol.cpp)
# target_link_libraries(im-network im-core Boost::asio)

# 数据存储库
# add_library(im-storage STATIC src/storage/MongoDBHandler.cpp src/storage/KafkaProducer.cpp src/storage/KafkaConsumer.cpp)
# target_link_libraries(im-storage im-core mongo::mongocxx_shared rdkafka::rdkafka)

# 业务服务库
# add_library(im-service STATIC src/service/UserService.cpp src/service/MessageService.cpp src/service/GroupService.cpp)
# target_link_libraries(im-service im-core im-network im-storage)

# 主程序
add_executable(main src/main.cpp)
target_link_libraries(main PRIVATE ape-server)