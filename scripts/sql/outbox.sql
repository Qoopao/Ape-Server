CREATE TABLE IF NOT EXISTS im_outbox (
        id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
        operation TINYINT UNSIGNED NOT NULL,
        dedupe_key VARCHAR(256) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        actor_account BIGINT UNSIGNED NOT NULL,
        peer_account BIGINT UNSIGNED NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        server_msg_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL DEFAULT '',
        seq BIGINT NOT NULL,
        payload MEDIUMTEXT NOT NULL,
        state TINYINT UNSIGNED NOT NULL DEFAULT 0,
        stage TINYINT UNSIGNED NOT NULL DEFAULT 0,
        dispatch_done TINYINT UNSIGNED NOT NULL DEFAULT 0,
        attempts INT UNSIGNED NOT NULL DEFAULT 0,
        available_at BIGINT NOT NULL DEFAULT 0,
        lease_until BIGINT NOT NULL DEFAULT 0,
        lease_token VARCHAR(64) CHARACTER SET ascii NOT NULL DEFAULT '',
        last_error VARCHAR(2048) NOT NULL DEFAULT '',
        created_at BIGINT NOT NULL,
        updated_at BIGINT NOT NULL,
        UNIQUE KEY outbox_dedupe(operation,dedupe_key),
        KEY outbox_ready(state,available_at,id),
        KEY outbox_lease(state,lease_until,id),
        KEY outbox_message(server_msg_id,operation),
        KEY outbox_pair(conversation_id,operation,seq)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS im_message_receipts (
        owner_account BIGINT UNSIGNED NOT NULL,
        server_msg_id VARCHAR(128) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        conversation_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
        seq BIGINT NOT NULL,
        delivered_at BIGINT NOT NULL,
        PRIMARY KEY(owner_account,server_msg_id),
        KEY receipt_conversation(owner_account,conversation_id,seq)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
