cmake_minimum_required(VERSION 3.20)
project(im-system CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(VCPKG_TRIPLET "x64-linux" CACHE STRING "vcpkg x64-linux")
set(CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TRIPLET} ${CMAKE_PREFIX_PATH})
set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

# 引入vcpkg依赖
find_package(Boost REQUIRED COMPONENTS asio beast thread uuid mysql)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(utf8proc CONFIG REQUIRED)
find_package(mongocxx CONFIG REQUIRED)
find_package(bsoncxx CONFIG REQUIRED)
find_package(RdKafka CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(etcd-cpp-api CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(opentelemetry-cpp CONFIG REQUIRED)

# 收集所有 src 下的 .cpp
file(GLOB_RECURSE ALL_IMPL_FILE CONFIGURE_DEPENDS "src/*.cpp")

# 收集src下protobuf生成的cc文件
file(GLOB_RECURSE PROTO_CC_FILE CONFIGURE_DEPENDS "src/proto/*.cc")

list(APPEND ALL_IMPL_FILE ${PROTO_CC_FILE})

message(STATUS "Collected CPP files: ${ALL_IMPL_FILE}")
list(REMOVE_ITEM ALL_IMPL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")  # 移除main.cpp

# 核心库
add_library(ape-server STATIC ${ALL_IMPL_FILE})
target_include_directories(ape-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/proto)
target_link_libraries(ape-server PUBLIC 
# Boost组件
Boost::asio Boost::beast Boost::thread Boost::mysql

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
${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libgrpc++_reflection.a

# etcd
etcd-cpp-api

# OpenSSL
OpenSSL::Crypto

# OpenTelemetry Trace (OTLP gRPC exporter + SDK)
opentelemetry-cpp::otlp_grpc_exporter

# OpenTelemetry Metrics (OTLP gRPC exporter + SDK)
opentelemetry-cpp::otlp_grpc_metrics_exporter

# OpenTelemetry Logs (OTLP gRPC exporter + SDK)
opentelemetry-cpp::otlp_grpc_log_record_exporter

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

# 压测工具
# add_executable(stress_test test/stress_test.cpp)
# target_include_directories(stress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/src/proto)
# target_link_libraries(stress_test PRIVATE ape-server)

option(APE_REGISTRATION_TESTS "Build isolated MySQL registration integration tests" OFF)
option(APE_GROUP_TESTS "Build isolated group chat integration tests" OFF)
option(APE_MONGO_INDEX_TESTS "Build MongoDB index compatibility tests" OFF)
option(APE_OFFLINE_LOGIN_TESTS "Build offline-login regression tests" OFF)
if(APE_OFFLINE_LOGIN_TESTS)
    add_executable(offline_disconnect_test test/offline_disconnect_test.cpp)
    target_link_libraries(offline_disconnect_test PRIVATE ape-server)
    add_executable(offline_login_test test/offline_login_test.cpp)
    target_link_libraries(offline_login_test PRIVATE ape-server)
endif()
if(APE_MONGO_INDEX_TESTS)
    add_executable(mongo_indexes_test test/mongo_indexes_test.cpp src/storage/mongoconnector.cpp)
    target_include_directories(mongo_indexes_test PRIVATE include)
    target_link_libraries(mongo_indexes_test PRIVATE mongo::mongocxx_static mongo::bsoncxx_static utf8proc::utf8proc Boost::asio Boost::thread spdlog::spdlog)
endif()
if(APE_GROUP_TESTS)
    add_executable(group_integration test/group_integration.cpp)
    target_link_libraries(group_integration PRIVATE ape-server)
endif()
if(APE_REGISTRATION_TESTS)
    add_executable(registration_integration test/registration_integration.cpp)
    target_link_libraries(registration_integration PRIVATE ape-server)
endif()
