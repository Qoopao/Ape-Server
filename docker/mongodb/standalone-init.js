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

print("MongoDB单机版初始化完成。");
