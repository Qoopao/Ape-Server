// init-mongo.js
// 1. 切换到 admin 数据库（用于创建管理员用户）
db = db.getSiblingDB('admin');

// 2. 创建 root 管理员用户
db.createUser({
  user: "admin",
  pwd: "admin123",  // 替换为实际密码
  roles: [{ role: "root", db: "admin" }]
});

// 3. 切换到应用数据库
db = db.getSiblingDB('myapp');

// 4. 创建应用数据库用户（仅有权限访问 myapp）
db.createUser({
  user: "myappuser",
  pwd: "myapp123",  // 替换为实际密码
  roles: [{ role: "readWrite", db: "myapp" }]
});

// 5. 切换到 IM-System 数据库（应用业务库）
db = db.getSiblingDB('IM-System');

// 创建唯一索引
db.msg.createIndex({serverMsgID:1}, {unique:true})

// 6. 创建 offline_msg 集合
db.createCollection('offline_msg');

// 7. 创建复合查询索引（按用户拉取离线消息）
db.offline_msg.createIndex(
  { "recvID": 1, "seq": 1 },
  { name: "idx_offline_msg_recvid_seq" }
);

// 9. 创建 isSent 过滤索引（用于查询未送达的消息）
db.offline_msg.createIndex(
  { "recvID": 1, "seq": 1, "isSent": 1 },
  { name: "idx_offline_msg_recvid_seq_issent" }
);

print("MongoDB: IM-System offline_msg indexes created successfully");
