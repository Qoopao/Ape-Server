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

// 6. 创建 offline_msg 集合
db.createCollection('offline_msg');

// 7. 创建 TTL 索引（30 天自动过期）
// 基于 createdAt 字段，文档将在创建 30 天（2592000 秒）后自动删除
db.offline_msg.createIndex(
  { "createdAt": 1 },
  {
    expireAfterSeconds: 2592000,  // 30 * 24 * 60 * 60 = 2592000
    name: "idx_offline_msg_ttl"
  }
);

// 8. 创建复合查询索引（按用户拉取离线消息）
db.offline_msg.createIndex(
  { "recvID": 1, "seq": 1 },
  { name: "idx_offline_msg_recvid_seq" }
);

// 9. 创建 isPushed 过滤索引（用于查询未推送的消息）
db.offline_msg.createIndex(
  { "recvID": 1, "seq": 1, "isPushed": 1 },
  { name: "idx_offline_msg_recvid_seq_ispushed" }
);

print("MongoDB: IM-System offline_msg indexes created successfully");
