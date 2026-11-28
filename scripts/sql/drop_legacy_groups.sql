-- 群聊 v3 破坏性重置：删掉所有旧群表（mock 数据不迁移）。
-- 停服务后执行；随后启动进程会由 InitializeGroups() 重建新 schema。
-- 注意：会删除全部群与群消息索引数据；Mongo 群正文需另行清理：
--   db.msg.deleteMany({isGroupMsg: true})
DROP TABLE IF EXISTS im_group_send_staging;
DROP TABLE IF EXISTS im_group_messages;
DROP TABLE IF EXISTS im_group_requests;
DROP TABLE IF EXISTS im_group_applications;
DROP TABLE IF EXISTS im_group_numbers;
DROP TABLE IF EXISTS im_group_members;
DROP TABLE IF EXISTS im_group_stream;
DROP TABLE IF EXISTS im_groups;
-- 更早版本遗留的群表。
DROP TABLE IF EXISTS ape_auth.group_members;
DROP TABLE IF EXISTS ape_auth.groups;
