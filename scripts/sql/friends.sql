CREATE TABLE IF NOT EXISTS friend_pairs (
    low_account BIGINT UNSIGNED NOT NULL,
    high_account BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (low_account, high_account)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friend_applications (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    from_account BIGINT UNSIGNED NOT NULL,
    to_account BIGINT UNSIGNED NOT NULL,
    message VARCHAR(512) NOT NULL DEFAULT '',
    status TINYINT NOT NULL DEFAULT 0,
    created_time BIGINT NOT NULL,
    handled_time BIGINT NOT NULL DEFAULT 0,
    PRIMARY KEY (id),
    KEY incoming (to_account, id),
    KEY outgoing (from_account, id),
    KEY pending (from_account, to_account, status)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friends (
    id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    owner_account BIGINT UNSIGNED NOT NULL,
    friend_account BIGINT UNSIGNED NOT NULL,
    remark VARCHAR(128) NOT NULL DEFAULT '',
    created_time BIGINT NOT NULL,
    PRIMARY KEY (id),
    UNIQUE KEY relationship (owner_account, friend_account),
    KEY owner_list (owner_account, id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friend_chat_deletions (
    owner_account BIGINT UNSIGNED NOT NULL,
    peer_account BIGINT UNSIGNED NOT NULL,
    deleted_time BIGINT NOT NULL,
    deleted_seq BIGINT NOT NULL,
    PRIMARY KEY (owner_account, peer_account)
) ENGINE=InnoDB;
