// MongoDB 分片集群 mongos 初始化脚本

// 等待配置服务器启动
sleep(5000);

// 添加分片
sh.addShard("rs0/mongodb-1:27017,mongodb-2:27017,mongodb-3:27017");

// 启用分片数据库
sh.enableSharding("IM-System");

// 对集合进行分片
sh.shardCollection("IM-System.msg", { "conversation_id": 1 });

// 创建应用数据库用户（仅有权限访问 IM-System）
db.createUser({
  user: "IM-user",
  pwd: "user123", 
  roles: [{ role: "readWrite", db: "IM-System" }]
});

print("MongoDB 分片集群初始化完成");
