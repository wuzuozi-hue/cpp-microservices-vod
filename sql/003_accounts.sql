CREATE TABLE IF NOT EXISTS vod_users (
    user_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    email VARCHAR(254) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    nickname VARCHAR(64) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NULL,
    role_type TINYINT UNSIGNED NOT NULL DEFAULT 3,
    user_status TINYINT UNSIGNED NOT NULL DEFAULT 1,
    password_salt CHAR(32) CHARACTER SET ascii COLLATE ascii_bin NULL,
    password_hash CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    password_iterations INT UNSIGNED NULL,
    failed_attempts INT UNSIGNED NOT NULL DEFAULT 0,
    locked_until DATETIME NULL,
    PRIMARY KEY (user_id), UNIQUE KEY users_email (email), UNIQUE KEY users_nickname (nickname)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_follows (
    follower_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    target_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    created_at DATETIME NOT NULL,
    PRIMARY KEY (follower_id, target_id), INDEX follow_target (target_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_user_avatars (
    user_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (user_id), INDEX avatar_file (file_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_administrators (
    user_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    nickname VARCHAR(64) CHARACTER SET utf8mb4 NOT NULL,
    memo VARCHAR(500) CHARACTER SET utf8mb4 NOT NULL,
    enabled BOOLEAN NOT NULL DEFAULT TRUE,
    PRIMARY KEY (user_id), INDEX administrators_enabled (enabled)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_email_codes (
    code_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    session_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    email VARCHAR(254) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    digest CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    expires_at DATETIME NOT NULL,
    attempts TINYINT UNSIGNED NOT NULL DEFAULT 0,
    used BOOLEAN NOT NULL DEFAULT FALSE,
    PRIMARY KEY (code_id), INDEX codes_expiry (expires_at)
) ENGINE=InnoDB;
