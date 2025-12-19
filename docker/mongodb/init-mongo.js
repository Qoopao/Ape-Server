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

// 5. 可选：创建集合并插入初始数据
db.users.insertMany([
  { name: "Alice", email: "alice@example.com" },
  { name: "Bob", email: "bob@example.com" }
]);