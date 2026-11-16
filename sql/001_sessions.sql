-- Run in a dedicated application database selected by the operator.
CREATE TABLE IF NOT EXISTS vod_sessions (
    session_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    user_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    expires_at DATETIME NOT NULL,
    last_code_at DATETIME NULL,
    PRIMARY KEY (session_id),
    INDEX sessions_expiry (expires_at)
) ENGINE=InnoDB;
