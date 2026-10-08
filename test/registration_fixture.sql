-- Run ONLY in the temporary ape-registration-test container.
USE ape_registration_test;
CREATE TABLE users (
 user_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
 account BIGINT NOT NULL, phone VARCHAR(11) NOT NULL, nickname VARCHAR(64) NOT NULL,
 password_hash VARCHAR(255) NOT NULL,password_salt VARCHAR(64) NOT NULL,
 UNIQUE KEY uk_account(account), UNIQUE KEY uk_phone(phone)
) ENGINE=InnoDB;
CREATE TABLE account_pool (
 id BIGINT AUTO_INCREMENT PRIMARY KEY,account BIGINT NOT NULL,status TINYINT NOT NULL DEFAULT 0,
 UNIQUE KEY uk_account(account),KEY idx_status_id(status,id)
) ENGINE=InnoDB;
CREATE TABLE account_pool_cursor(id TINYINT PRIMARY KEY,next_start BIGINT UNSIGNED NOT NULL) ENGINE=InnoDB;
INSERT INTO account_pool_cursor VALUES(1,1000000000);