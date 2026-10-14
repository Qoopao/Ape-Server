CREATE TABLE IF NOT EXISTS im_conversations (
        owner_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        peer_account BIGINT UNSIGNED NOT NULL,
        is_pinned TINYINT UNSIGNED NOT NULL DEFAULT 0,
        recv_msg_opt TINYINT UNSIGNED NOT NULL DEFAULT 0,
        is_hidden TINYINT UNSIGNED NOT NULL DEFAULT 0,
        min_seq BIGINT NOT NULL DEFAULT 0,
        max_seq BIGINT NOT NULL DEFAULT 0,
        read_seq BIGINT NOT NULL DEFAULT 0,
        unread_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
        latest_time BIGINT UNSIGNED NOT NULL DEFAULT 0,
        latest_message MEDIUMTEXT NOT NULL,
        PRIMARY KEY(owner_account,conversation_id),
        KEY conversation_pair(conversation_id),
        KEY conversation_order(owner_account,is_hidden,is_pinned,latest_time,conversation_id)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS im_conversation_unread (
        owner_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        seq BIGINT NOT NULL,
        PRIMARY KEY(owner_account,conversation_id,seq),
        CONSTRAINT conversation_unread_owner FOREIGN KEY(owner_account,conversation_id)
            REFERENCES im_conversations(owner_account,conversation_id) ON DELETE CASCADE
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
