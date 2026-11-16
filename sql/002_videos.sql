CREATE TABLE IF NOT EXISTS vod_videos (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    owner_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    title VARCHAR(80) CHARACTER SET utf8mb4 NOT NULL,
    description VARCHAR(500) CHARACTER SET utf8mb4 NOT NULL,
    source_file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NULL,
    cover_file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NULL,
    playlist_file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NULL,
    completed_claim_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    reviewer_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    status TINYINT UNSIGNED NOT NULL,
    created_at DATETIME NOT NULL,
    PRIMARY KEY (video_id),
    INDEX videos_owner_status (owner_id, status),
    INDEX videos_status_created (status, created_at)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_video_likes (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    user_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (video_id, user_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_video_metadata (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    video_type INT UNSIGNED NOT NULL DEFAULT 0,
    duration_seconds INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (video_id), INDEX video_type_index (video_type)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_video_tags (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    tag_id INT UNSIGNED NOT NULL,
    PRIMARY KEY (video_id, tag_id), INDEX video_tag_index (tag_id, video_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_video_plays (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    play_count BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (video_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_barrage (
    barrage_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    user_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    content VARCHAR(200) CHARACTER SET utf8mb4 NOT NULL,
    position_seconds INT UNSIGNED NOT NULL,
    created_at DATETIME NOT NULL,
    PRIMARY KEY (barrage_id), INDEX barrage_video_time (video_id, position_seconds, barrage_id)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_transcode_jobs (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    source_file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    status TINYINT UNSIGNED NOT NULL DEFAULT 0,
    claim_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    lease_until DATETIME NULL,
    attempts INT UNSIGNED NOT NULL DEFAULT 0,
    created_at DATETIME NOT NULL,
    PRIMARY KEY (video_id), INDEX jobs_status (status, created_at)
) ENGINE=InnoDB;
CREATE TABLE IF NOT EXISTS vod_transcode_artifacts (
    video_id VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    claim_id CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    file_id VARCHAR(255) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (video_id, claim_id, file_id)
) ENGINE=InnoDB;
