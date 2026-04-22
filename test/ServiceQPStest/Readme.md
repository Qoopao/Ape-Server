# 查询示例

ghz --insecure \
    --proto ~/ape-server/proto/backbon.proto \
    --call backbon.BackbonService.RegisterService \
    -d '{"service":"myservice","ipport":["127.0.0.1:8080"],"methods":["Method1","Method2"]}' \
    127.0.0.1:50309