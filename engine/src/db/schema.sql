-- LUMINA 全库表结构（schema_version=2，M2 T2.2：新增 files 表 + photos.file_id）
-- 本文件是唯一事实源：由 CMake 在配置期生成 schema_sql.inc，运行时执行
-- 版本化增量（002/003/004）在 db/migrations.cpp，本文件只负责「全量幂等建表」。
PRAGMA journal_mode=WAL;
PRAGMA busy_timeout=5000;

CREATE TABLE IF NOT EXISTS schema_meta(key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE IF NOT EXISTS roots(
  id INTEGER PRIMARY KEY,
  path TEXT UNIQUE,
  label TEXT
);

CREATE TABLE IF NOT EXISTS folders(
  id INTEGER PRIMARY KEY,
  root_id INT,
  rel_path TEXT,
  name TEXT,
  missing INT DEFAULT 0,
  UNIQUE(root_id, rel_path)
);

-- 四级表 · 文件身份（M2 T2.2 / migration 002，Q1）
-- 身份 = (folder_id, name)：文件夹改名/移动只改 folders.rel_path，文件 id 不变。
-- 注意：photos 表**不重建**（它是 photos_fts 的外部内容表），photos.file_id 由
-- migrations.cpp 用 PRAGMA 守卫后 ALTER 追加。两处列定义必须保持一致。
CREATE TABLE IF NOT EXISTS files(
  id INTEGER PRIMARY KEY,
  folder_id INT NOT NULL REFERENCES folders(id),
  name TEXT NOT NULL,
  format TEXT,
  file_size INT,
  md5 TEXT,
  mtime INT,
  missing INT DEFAULT 0,
  health TEXT DEFAULT 'unchecked',
  UNIQUE(folder_id, name)
);
CREATE INDEX IF NOT EXISTS idx_files_folder ON files(folder_id, name);

CREATE TABLE IF NOT EXISTS photos(
  id INTEGER PRIMARY KEY,
  folder_id INT REFERENCES folders(id),
  root_id INT,
  file_name TEXT NOT NULL,
  file_path TEXT UNIQUE NOT NULL,
  format TEXT,
  file_size INT,
  md5 TEXT,
  width INT,
  height INT,
  captured_at INT,
  imported_at INT,
  camera TEXT,
  lens TEXT,
  iso INT,
  aperture TEXT,
  shutter TEXT,
  focal_length TEXT,
  white_balance TEXT,
  gps_lat REAL,
  gps_lng REAL,
  gps_alt REAL,
  location_id INT,
  rating INT DEFAULT 0,
  pick TEXT DEFAULT 'none',
  color_label TEXT,
  keywords TEXT,
  description TEXT,
  sidecar_path TEXT,
  sidecar_mtime INT,
  has_edits INT DEFAULT 0,
  is_missing INT DEFAULT 0,
  is_video INT DEFAULT 0,
  pair_kind TEXT DEFAULT 'single',
  health TEXT DEFAULT 'unchecked',
  deleted INT DEFAULT 0,
  seq_id INT,
  location TEXT,
  date TEXT
);

CREATE INDEX IF NOT EXISTS idx_photos_captured ON photos(captured_at) WHERE deleted=0;
CREATE INDEX IF NOT EXISTS idx_photos_folder   ON photos(folder_id);
CREATE INDEX IF NOT EXISTS idx_photos_root     ON photos(root_id);
CREATE INDEX IF NOT EXISTS idx_photos_rating   ON photos(rating);
CREATE INDEX IF NOT EXISTS idx_photos_seq      ON photos(seq_id);
CREATE INDEX IF NOT EXISTS idx_photos_missing  ON photos(is_missing);
CREATE INDEX IF NOT EXISTS idx_photos_deleted  ON photos(deleted);
CREATE INDEX IF NOT EXISTS idx_photos_camera   ON photos(camera);

-- 全文检索（FTS5 + unicode61，中文可用）
-- 外部内容表：列必须与 photos 同名列一致（file_name / location / date）
CREATE VIRTUAL TABLE IF NOT EXISTS photos_fts USING fts5(
  file_name, location, date, content='photos', content_rowid='id', tokenize='unicode61');

-- 外部内容表靠触发器与 photos 同步（FTS5 官方推荐写法）
CREATE TRIGGER IF NOT EXISTS photos_fts_ai AFTER INSERT ON photos BEGIN
  INSERT INTO photos_fts(rowid, file_name, location, date)
  VALUES (new.id, new.file_name, new.location, new.date);
END;
CREATE TRIGGER IF NOT EXISTS photos_fts_ad AFTER DELETE ON photos BEGIN
  INSERT INTO photos_fts(photos_fts, rowid, file_name, location, date)
  VALUES ('delete', old.id, old.file_name, old.location, old.date);
END;
CREATE TRIGGER IF NOT EXISTS photos_fts_au AFTER UPDATE ON photos BEGIN
  INSERT INTO photos_fts(photos_fts, rowid, file_name, location, date)
  VALUES ('delete', old.id, old.file_name, old.location, old.date);
  INSERT INTO photos_fts(rowid, file_name, location, date)
  VALUES (new.id, new.file_name, new.location, new.date);
END;

CREATE TABLE IF NOT EXISTS locations(
  id INTEGER PRIMARY KEY,
  name TEXT,
  lat REAL,
  lng REAL,
  alt REAL,
  favorite INT DEFAULT 0
);

CREATE TABLE IF NOT EXISTS sequences(
  id INTEGER PRIMARY KEY,
  name TEXT,
  kind TEXT,
  frame_count INT,
  state TEXT DEFAULT 'idle',
  created_at INT
);

CREATE TABLE IF NOT EXISTS sequence_frames(
  seq_id INT,
  frame_no INT,
  photo_id INT,
  PRIMARY KEY(seq_id, frame_no)
);

CREATE TABLE IF NOT EXISTS albums(
  id INTEGER PRIMARY KEY,
  name TEXT,
  kind TEXT,
  cover_photo_id INT,
  published_token TEXT,
  published_at INT,
  expires_at INT,
  created_at INT
);

CREATE TABLE IF NOT EXISTS album_items(
  album_id INT,
  photo_id INT,
  position INT,
  PRIMARY KEY(album_id, photo_id)
);

CREATE TABLE IF NOT EXISTS presets(
  id INTEGER PRIMARY KEY,
  name TEXT UNIQUE,
  params_json TEXT
);

CREATE TABLE IF NOT EXISTS export_presets(
  id INTEGER PRIMARY KEY,
  name TEXT,
  spec_json TEXT
);

CREATE TABLE IF NOT EXISTS health_issues(
  id INTEGER PRIMARY KEY,
  photo_id INT,
  kind TEXT,
  path TEXT,
  last_known TEXT,
  status TEXT DEFAULT 'open',
  detected_at INT,
  resolved_at INT
);
CREATE INDEX IF NOT EXISTS idx_health_photo ON health_issues(photo_id);
CREATE INDEX IF NOT EXISTS idx_health_status ON health_issues(status);

CREATE TABLE IF NOT EXISTS backups(
  id INTEGER PRIMARY KEY,
  photo_id INT,
  backup_path TEXT,
  verified_at INT,
  UNIQUE(photo_id)
);

CREATE TABLE IF NOT EXISTS virtual_copies(
  id INTEGER PRIMARY KEY,
  photo_id INT,
  name TEXT,
  params_json TEXT,
  created_at INT
);

CREATE TABLE IF NOT EXISTS snapshots(
  id INTEGER PRIMARY KEY,
  photo_id INT,
  name TEXT,
  params_json TEXT,
  created_at INT
);

CREATE TABLE IF NOT EXISTS sequence_ramps(
  seq_id INT PRIMARY KEY,
  keyframes_json TEXT,
  updated_at INT
);

CREATE TABLE IF NOT EXISTS jobs(
  id INTEGER PRIMARY KEY,
  kind TEXT,
  state TEXT,
  progress INT,
  detail TEXT
);

CREATE TABLE IF NOT EXISTS mock_meta(key TEXT PRIMARY KEY, value TEXT);

-- Web 客户画廊：发布元信息（标题/客户名/密码哈希）与客户收藏反馈。
-- 为什么单独建表：albums 没有放 title/customer_name/password_hash 的列；
-- 也不能用 photos.rating/pick 承载客户收藏 —— 那是摄影师自己的筛选结果，覆盖等于毁数据。
-- 注意：gallery_service.cpp 里保留了一份幂等 DDL（兼容本次升级前已建好的库），
-- 两处列定义必须保持一致，改动请同步。
CREATE TABLE IF NOT EXISTS gallery_meta(
  album_id INTEGER PRIMARY KEY,
  token TEXT,
  title TEXT,
  customer_name TEXT,
  password_hash TEXT,
  created_at INT,
  updated_at INT
);

CREATE TABLE IF NOT EXISTS gallery_feedback(
  album_id INT,
  photo_id INT,
  favorited INT DEFAULT 0,
  updated_at INT,
  PRIMARY KEY(album_id, photo_id)
);
