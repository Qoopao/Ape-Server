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
