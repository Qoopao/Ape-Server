CREATE DATABASE IF NOT EXISTS ape_auth
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE ape_auth;

CREATE TABLE IF NOT EXISTS `users` (
    user_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    account        BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '对外10位账号',
    nickname       VARCHAR(64) NOT NULL,
    phone          VARCHAR(11) NOT NULL,
    password_hash  VARCHAR(255) NOT NULL,
    password_salt  VARCHAR(64) NOT NULL,
    created_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at     DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                   ON UPDATE CURRENT_TIMESTAMP COMMENT '记录更新时间',
    last_login_at  DATETIME DEFAULT NULL COMMENT '最后登录时间',
    UNIQUE KEY uk_account (account)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT INTO users (
    account,
    nickname,
    phone,
    password_hash,
    password_salt
) VALUES (
    1000000001,
    'Bob',
    '13800138000',
    'fa2f35eba15768650f7bd746c5d0a5717efe258c497c98ff6b6f7c6d23351fe8',
    '00112233445566778899aabbccddeeff'
);

-- ── 群组表 ──
CREATE TABLE IF NOT EXISTS `groups` (
    group_id VARCHAR(64) PRIMARY KEY,
    group_name VARCHAR(255) NOT NULL,
    notification TEXT,
    introduction TEXT,
    face_url VARCHAR(512),
    owner_user_id BIGINT UNSIGNED NOT NULL,
    creator_user_id BIGINT UNSIGNED NOT NULL,
    create_time BIGINT NOT NULL,
    member_count INT DEFAULT 0,
    status TINYINT DEFAULT 0,
    group_type INT DEFAULT 0,
    need_verification INT DEFAULT 0,
    look_member_info INT DEFAULT 0,
    apply_member_friend INT DEFAULT 0,
    notification_update_time BIGINT DEFAULT 0,
    notification_user_id BIGINT UNSIGNED,
    ex TEXT,
    INDEX idx_owner (owner_user_id),
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 群成员表 ──
CREATE TABLE IF NOT EXISTS `group_members` (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    group_id VARCHAR(64) NOT NULL,
    user_id BIGINT UNSIGNED NOT NULL,
    role_level TINYINT DEFAULT 1,
    join_time BIGINT NOT NULL,
    nickname VARCHAR(255),
    face_url VARCHAR(512),
    join_source INT DEFAULT 0,
    operator_user_id BIGINT UNSIGNED,
    inviter_user_id BIGINT UNSIGNED,
    mute_end_time BIGINT DEFAULT 0,
    ex TEXT,
    UNIQUE KEY uk_group_user (group_id, user_id),
    INDEX idx_user (user_id),
    INDEX idx_group (group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 号段游标表（单行，记录下一个可用号段起点）
CREATE TABLE IF NOT EXISTS account_pool_cursor (
    id            TINYINT PRIMARY KEY DEFAULT 1,
    next_start    BIGINT UNSIGNED NOT NULL COMMENT '下一个可用号段起点',
    updated_at    DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                  ON UPDATE CURRENT_TIMESTAMP,
    CONSTRAINT chk_single_row CHECK (id = 1)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 账号池表（候选账号）
CREATE TABLE IF NOT EXISTS account_pool (
    id          BIGINT AUTO_INCREMENT PRIMARY KEY,
    account     BIGINT UNSIGNED NOT NULL COMMENT '10位候选账号',
    status      TINYINT NOT NULL DEFAULT 0 COMMENT '0=可用, 1=已分配',
    created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
                ON UPDATE CURRENT_TIMESTAMP,
    UNIQUE KEY uk_account (account),
    KEY idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 补货锁表（数据库实现的分布式锁）
CREATE TABLE IF NOT EXISTS account_pool_refill_lock (
    lock_name   VARCHAR(64) PRIMARY KEY,
    holder      VARCHAR(128) NOT NULL COMMENT '持有者标识',
    expires_at  DATETIME NOT NULL COMMENT '锁过期时间',
    created_at  DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;


-- 初始化游标行（只插一次，用 INSERT IGNORE 保证幂等）
INSERT IGNORE INTO account_pool_cursor (id, next_start)
VALUES (1, 1000000000);
