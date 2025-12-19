cmake_minimum_required(VERSION 3.20)
project(im-system CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VCPKG_TRIPLET "x64-linux" CACHE STRING "vcpkg x64-linux")
set(CMAKE_PREFIX_PATH ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/${VCPKG_TRIPLET} ${CMAKE_PREFIX_PATH})

# 引入vcpkg依赖
find_package(Boost REQUIRED COMPONENTS asio)
find_package(Boost REQUIRED COMPONENTS beast)
find_package(spdlog REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(utf8proc REQUIRED)
find_package(mongocxx REQUIRED)
find_package(bsoncxx REQUIRED)

# 收集所有 src 下的 .cpp
file(GLOB_RECURSE ALL_CPP_FILE "src/*.cpp")
message(STATUS "Collected CPP files: ${ALL_CPP_FILE}")
list(REMOVE_ITEM ALL_CPP_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")  # 移除main.cpp

# 核心库
add_library(ape-server STATIC ${ALL_CPP_FILE})
target_include_directories(ape-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(ape-server PUBLIC 
Boost::asio Boost::beast 
spdlog::spdlog 
nlohmann_json::nlohmann_json 
mongo::mongocxx_static mongo::bsoncxx_static utf8proc::utf8proc
)

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

# 测试
# enable_testing()
# add_executable(unit-tests tests/unit/CoreTest.cpp tests/unit/NetworkTest.cpp)
# target_link_libraries(unit-tests im-core im-network GTest::gtest_main)
# add_test(NAME UnitTests COMMAND unit-tests)