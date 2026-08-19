// MongoDB 副本集初始化脚本

// 等待所有节点启动
sleep(5000);

// 初始化副本集
rs.initiate({
    _id: "rs0",
    members: [
        { _id: 0, host: "mongodb-1:27017" },
        { _id: 1, host: "mongodb-2:27017" },
        { _id: 2, host: "mongodb-3:27017" }
    ]
});

// 等待副本集稳定
sleep(8000);

// 创建数据库、用户和集合
// 1. 切换到 admin 数据库（用于创建管理员用户）
db = db.getSiblingDB('admin');

// 2. 创建 root 管理员用户
db.createUser({
  user: "admin",
  pwd: "admin123",
  roles: [{ role: "root", db: "admin" }]
});

// 3. 延迟创建应用数据库
db = db.getSiblingDB('IM-System');

// 4. 创建集合
db.createCollection('msg');

// 5. 创建索引
db.msg.createIndex({serverMsgID:1}, {unique:true})

// 6. 创建应用数据库用户（仅有权限访问 IM-System）
db.createUser({
  user: "IM-user",
  pwd: "user123", 
  roles: [{ role: "readWrite", db: "IM-System" }]
});

print("MongoDB副本集初始化完成。");