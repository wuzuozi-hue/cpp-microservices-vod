CREATE TABLE IF NOT EXISTS vod_files (
    file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    owner_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    size_bytes BIGINT UNSIGNED NOT NULL,
    is_public BOOLEAN NOT NULL DEFAULT FALSE,
    created_at DATETIME NOT NULL,
    PRIMARY KEY (file_id), INDEX files_owner (owner_id)
) ENGINE=InnoDB;
