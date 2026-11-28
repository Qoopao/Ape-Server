-- 群聊 v3 schema（与 docs/group/协议与数据模型.md §4 一致）。
-- 直接切新协议：不存在 im_group_messages，im_groups 没有 last_seq，成员没有 ack/read。
-- 老库请先执行 scripts/sql/drop_legacy_groups.sql（或由 InitializeGroups 检测后重建）。

CREATE TABLE IF NOT EXISTS im_groups (
 id BIGINT UNSIGNED NOT NULL PRIMARY KEY,
 name VARCHAR(256) NOT NULL,
 owner BIGINT UNSIGNED NOT NULL,
 member_count BIGINT UNSIGNED NOT NULL DEFAULT 1,
 dismissed TINYINT NOT NULL DEFAULT 0,
 created_time BIGINT NOT NULL DEFAULT 0,
 updated_at BIGINT NOT NULL DEFAULT 0,
 KEY group_owner(owner)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 每群一个稠密 seq：next_seq=已分配最大；published_seq=已发布（含 tombstone）最大。
CREATE TABLE IF NOT EXISTS im_group_stream (
 group_id BIGINT UNSIGNED PRIMARY KEY,
 next_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
 published_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
 last_msg_time BIGINT UNSIGNED NOT NULL DEFAULT 0,
 epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
 dismissed TINYINT NOT NULL DEFAULT 0,
 updated_at BIGINT NOT NULL,
 CONSTRAINT fk_stream_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS im_group_members (
 group_id BIGINT UNSIGNED NOT NULL,
 account BIGINT UNSIGNED NOT NULL,
 active TINYINT NOT NULL DEFAULT 1,
 epoch BIGINT UNSIGNED NOT NULL DEFAULT 1,
 join_seq BIGINT UNSIGNED NOT NULL DEFAULT 0,
 joined_time BIGINT NOT NULL DEFAULT 0,
 left_time BIGINT NOT NULL DEFAULT 0,
 PRIMARY KEY(group_id,account),
 KEY account_active(account,active,group_id),
 CONSTRAINT fk_member_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS im_group_applications (
 id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
 group_id BIGINT UNSIGNED NOT NULL,
 account BIGINT UNSIGNED NOT NULL,
 reason VARCHAR(512) NOT NULL,
 status TINYINT NOT NULL DEFAULT 0,
 created_time BIGINT NOT NULL,
 KEY group_page(group_id,id),
 KEY applicant(account,id),
 CONSTRAINT fk_application_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 正文暂存：与 seq/outbox 同一事务写入；worker 写 Mongo 成功后删除。
CREATE TABLE IF NOT EXISTS im_group_send_staging (
 server_msg_id VARCHAR(64) PRIMARY KEY,
 group_id BIGINT UNSIGNED NOT NULL,
 seq BIGINT UNSIGNED NOT NULL,
 payload MEDIUMTEXT NOT NULL,
 created_at BIGINT NOT NULL,
 updated_at BIGINT NOT NULL,
 KEY staging_retention(updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS im_group_requests (
 actor BIGINT UNSIGNED NOT NULL,
 client_id VARCHAR(64) NOT NULL,
 payload MEDIUMTEXT NOT NULL,
 reply MEDIUMTEXT NOT NULL,
 created_time BIGINT NOT NULL DEFAULT 0,
 updated_at BIGINT NOT NULL DEFAULT 0,
 PRIMARY KEY(actor,client_id),
 KEY request_retention(updated_at)
) ENGINE=InnoDB DEFAULT CHARSET=ascii;

CREATE TABLE IF NOT EXISTS im_group_numbers (
 group_id BIGINT UNSIGNED PRIMARY KEY,
 number BIGINT UNSIGNED NOT NULL UNIQUE,
 CONSTRAINT fk_number_group FOREIGN KEY(group_id) REFERENCES im_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS im_group_number_cursor (
 id TINYINT PRIMARY KEY,
 next_number BIGINT UNSIGNED NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
INSERT IGNORE INTO im_group_number_cursor VALUES(1,1000000000);
