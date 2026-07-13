CREATE DATABASE IF NOT EXISTS ape_auth
  CHARACTER SET utf8mb4
  COLLATE utf8mb4_unicode_ci;

USE ape_auth;

CREATE TABLE IF NOT EXISTS users (
  user_id VARCHAR(64) PRIMARY KEY,
  username VARCHAR(64) NOT NULL UNIQUE,
  nickname VARCHAR(64) NOT NULL,
  password_hash VARCHAR(128) NOT NULL,
  password_salt VARCHAR(64) NOT NULL,
  created_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  last_login_at TIMESTAMP NULL DEFAULT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT INTO users (
  user_id,
  username,
  nickname,
  password_hash,
  password_salt
) VALUES (
  'test-user-001',
  'test',
  'Test User',
  'ba618256204676edb9225874af0b4b332dcb81eba71a43b5339aef6005558446',
  '00112233445566778899aabbccddeeff'
) ON DUPLICATE KEY UPDATE
  nickname = VALUES(nickname),
  password_hash = VALUES(password_hash),
  password_salt = VALUES(password_salt);

-- ── 群组表 ──

CREATE TABLE IF NOT EXISTS `groups` (
    group_id VARCHAR(64) PRIMARY KEY,
    group_name VARCHAR(255) NOT NULL,
    notification TEXT,
    introduction TEXT,
    face_url VARCHAR(512),
    owner_user_id VARCHAR(64) NOT NULL,
    creator_user_id VARCHAR(64) NOT NULL,
    create_time BIGINT NOT NULL,
    member_count INT DEFAULT 0,
    status TINYINT DEFAULT 0,
    group_type INT DEFAULT 0,
    need_verification INT DEFAULT 0,
    look_member_info INT DEFAULT 0,
    apply_member_friend INT DEFAULT 0,
    notification_update_time BIGINT DEFAULT 0,
    notification_user_id VARCHAR(64),
    ex TEXT,
    INDEX idx_owner (owner_user_id),
    INDEX idx_status (status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ── 群成员表 ──

CREATE TABLE IF NOT EXISTS `group_members` (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    group_id VARCHAR(64) NOT NULL,
    user_id VARCHAR(64) NOT NULL,
    role_level TINYINT DEFAULT 1,
    join_time BIGINT NOT NULL,
    nickname VARCHAR(255),
    face_url VARCHAR(512),
    join_source INT DEFAULT 0,
    operator_user_id VARCHAR(64),
    inviter_user_id VARCHAR(64),
    mute_end_time BIGINT DEFAULT 0,
    ex TEXT,
    UNIQUE KEY uk_group_user (group_id, user_id),
    INDEX idx_user (user_id),
    INDEX idx_group (group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
