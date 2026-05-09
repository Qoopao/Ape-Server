注意要用vcpkg提供的protoc工具生成相关文件

./vcpkg_installed/x64-linux/tools/protobuf/protoc -I=./proto --cpp_out=./src/proto --grpc_out=./src/proto --plugin=protoc-gen-grpc=./vcpkg_installed/x64-linux/tools/grpc/grpc_cpp_plugin ./proto/*.proto


-I .proto文件搜索路径
--cpp_out 基础protobuf文件输出路径
--grpc_out grpc文件输出路径
最后一项是需要编译的./proto文件