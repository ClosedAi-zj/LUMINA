// LUMINA 引擎入口：组装服务 → 注册方法 → 握手 → 进入 stdio JSON-RPC 主循环
// 启动即握手、无残留状态（被 kill 后由外部重启，参数以 sidecar 为准天然可恢复）
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/job_manager.h"
#include "core/logger.h"
#include "core/str_util.h"
#include "core/thread_pool.h"
#include "db/album_dao.h"
#include "db/database.h"
#include "db/fts_indexer.h"
#include "db/health_dao.h"
#include "db/photo_dao.h"
#include "edit/edit_session.h"
#include "edit/virtual_copy.h"
#include "health/backup.h"
#include "health/db_backup.h"
#include "health/scanner.h"
#include "health/watcher.h"
#include "import/importer.h"
#include "jobs/rebuild_develop_settings.h"
#include "library/query_engine.h"
#include "mock/generator.h"
#include "raw/raw_decoder.h"
#include "rpc/methods.h"
#include "rpc/rpc_server.h"
#include "rpc/version.h"
#include "thumb/thumbnail_service.h"
#include "xmp/migrator.h"
#include "xmp/xmp_service.h"

using namespace lumina;

namespace {

void printUsage() {
  fprintf(stderr,
          "LUMINA engine %s (protocol %s)\n"
          "usage: lumina-engine [--home <dir>] [--help]\n"
          "  --home <dir>   运行时主目录（db/cache/logs 均在其下），默认 %%USERPROFILE%%/.lumina\n"
          "  --version      打印版本\n",
          kEngineVersion, kProtocolVersion);
}

}  // namespace

int main(int argc, char** argv) {
  std::string home;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--help" || arg == "-h") {
      printUsage();
      return 0;
    }
    if (arg == "--version" || arg == "-v") {
      printf("lumina-engine %s (protocol %s)\n", kEngineVersion, kProtocolVersion);
      return 0;
    }
    if (arg == "--home" && i + 1 < argc) {
      home = argv[++i];
    } else if (arg.rfind("--home=", 0) == 0) {
      home = arg.substr(7);
    }
  }
  if (home.empty()) {
    const char* profile = getenv("USERPROFILE");
    home = profile ? std::string(profile) + "/.lumina" : std::string("./lumina-home");
  }
  home = normalizePath(home);

  // ---- 1. 配置与日志 ----
  core::Config config;
  std::string initErr;
  if (!config.init(home, &initErr)) {
    fprintf(stderr, "[engine] fatal: %s\n", initErr.c_str());
    return 2;
  }
  Logger::instance().init(config.logsDir(), LogLevel::kInfo);
  logInfo("main", "LUMINA engine " + std::string(kEngineVersion) + " starting, home=" + home);
  logInfo("main", "protocol " + std::string(kProtocolVersion) + ", sqlite " +
                      std::string(sqlite3_libversion()));

  // ---- 2. 单实例锁（同 home 只允许一个引擎） ----
  std::string lockErr;
  if (!config.acquireLock(&lockErr)) {
    logError("main", lockErr);
    return 3;
  }

  // ---- 3. 数据库（WP-A2：打开前自愈——主库损坏则从最新备份恢复） ----
  {
    bool recovered = false;
    std::string recErr;
    if (!health::DbBackupService::recoverIfCorrupt(
            config.dbPath(), config.backupsDir(), &recovered, &recErr)) {
      // 损坏且无备份：不吞，记日志后照常走 open，让 open 给出原始错误（零静默）
      logError("main", "db recovery failed: " + recErr);
    } else if (recovered) {
      logWarn("main", "db was corrupt and has been restored from backup, continuing startup");
    }
  }
  db::Database database;
  std::string dbErr;
  if (!database.open(config.dbPath(), &dbErr)) {
    logError("main", "database open failed: " + dbErr);
    config.releaseLock();
    return 4;
  }
  if (!database.runMigrations(&dbErr)) {
    logError("main", "schema migration failed: " + dbErr);
    database.close();
    config.releaseLock();
    return 4;
  }

  // ---- 4. 服务装配 ----
  core::ThreadPool pool(std::thread::hardware_concurrency() > 2
                            ? std::thread::hardware_concurrency() - 1
                            : 2);
  rpc::Dispatcher dispatcher;
  rpc::RpcServer server(&dispatcher);

  JobManager jobs(&pool, server.notifyFn());

  db::PhotoDao photos(&database);
  db::AlbumDao albums(&database);
  db::HealthDao health(&database);
  db::FtsIndexer fts(&database);
  library::QueryEngine query(&config, &database, &photos);
  raw::RawDecoder decoder;
  xmp::XmpService xmpService(&config, &photos, &database, &pool);
  xmp::XmpMigrator migrator(&config, &photos, &database, &xmpService);
  // M2 T2.2b：四级表身份层（relocate 走它重算 sidecar_path，见 image_dao.cpp N10）
  db::ImageDao imageDao(&database, &xmpService);
  edit::EditSessionManager sessions(&xmpService, &photos);
  edit::VirtualCopyService vcopy(&config, &database, &photos, &xmpService);
  health::HealthScanner healthScanner(&config, &photos, &health, server.notifyFn());
  health::BackupService backup(&config, &photos, &database, server.notifyFn());
  import::Importer importer(&config, &photos, &imageDao, &database, &fts, &decoder, &xmpService,
                            server.notifyFn());
  mock::MockGenerator mockGenerator(&config, &photos, &database, &fts, &health, &decoder,
                                    &xmpService);
  thumb::ThumbnailService thumbs(&config, &photos, &pool,
                                 [&server](int64_t photoId, int size, const std::string& path) {
                                   json::JsonValue params = json::JsonValue::objectValue();
                                   params.setItem("photoId", photoId);
                                   params.setItem("size", size);
                                   params.setItem("path", path);
                                   server.sendNotification("event.thumb.ready", params);
                                 });

  rpc::EngineServices services;
  services.config = &config;
  services.db = &database;
  services.photos = &photos;
  services.images = &imageDao;
  services.albums = &albums;
  services.health = &health;
  services.fts = &fts;
  services.query = &query;
  services.thumbs = &thumbs;
  services.decoder = &decoder;
  services.xmp = &xmpService;
  services.migrator = &migrator;
  services.sessions = &sessions;
  services.vcopy = &vcopy;
  services.importer = &importer;
  services.healthScanner = &healthScanner;
  services.backup = &backup;
  services.mock = &mockGenerator;  // 曾遗漏：mock.generate 解引用空指针导致 worker 线程崩溃
  services.jobs = &jobs;
  services.server = &server;
  services.requestShutdown = [&server]() { server.requestShutdown(); };

  // ---- 5. 文件系统实时监测（自动开启） ----
  health::HealthWatcher watcher(&config, &photos, &health, server.notifyFn());
  services.watcher = &watcher;
  {
    std::string watchErr;
    watcher.start(&watchErr);
  }

  // ---- 6. 预置数据 + 方法注册 ----
  rpc::seedPresetData(&services);
  rpc::registerMethods(&dispatcher, &services);

  // ---- 6.5 DB 每日自动备份（WP-A2）：启动即补当日快照 + 守护线程跨天补份 ----
  health::DbBackupService dbBackup(&config, &database);
  {
    std::string outPath;
    std::string bErr;
    if (dbBackup.backupNow(&outPath, &bErr)) {
      logInfo("main", "startup db backup: " + outPath);
    } else {
      logWarn("main", "startup db backup failed: " + bErr);
    }
    dbBackup.startDaily();
  }

  // ---- 7. 启动兜底：XMP 版本迁移（v1.1.1 起改为 engine ready 之后后台执行，见 7.5） ----
  //     左移前此处同步跑 migrateAll，全新安装首次启动迁移慢（Defender 扫描 684MB 文件 ~22s）
  //     超过 UI 握手超时 15s，被降级为 Mock。现改为后台线程，握手秒回。

  logInfo("main", "engine ready, " + std::to_string(dispatcher.methodNames().size()) +
                      " methods registered");
  fflush(stdout);

  // ---- 7.5 后台 XMP 版本迁移（v1.1.1 主修复） ----
  // 迁移只是补字段的兜底升级，读侧兼容旧格式；放后台保证 RPC server 先起来、握手秒回。
  // 复用 XmpService 的 per-photo 写锁（migrator.cpp 内部持有），与编辑/地图写入串行化。
  std::atomic<bool> migrationStop{false};
  std::thread migrationThread([&migrator, &migrationStop]() {
    logInfo("main", "xmp migration pass started (background)");
    int migrated = 0;
    std::string migrateErr;
    if (migrator.migrateAll(&migrated, &migrateErr, &migrationStop)) {
      logInfo("main", "xmp migration pass done, scanned=" + std::to_string(migrated));
    } else {
      logWarn("main", "xmp migration pass failed: " + migrateErr);
    }
  });

  // ---- 7.7 后台 files 表一致性重建（M2 T2.2b） ----
  // 迁移 002 只在版本升级时回填一次；此后 mock/HDR/异常路径都可能让 photos 与
  // files 表漂移（file_id 为空/悬空）。每次启动后台对齐一遍（幂等，全对齐时 0 写入），
  // 不阻塞握手；与 XMP 迁移线程共用 stop 信号。
  std::thread reconcileThread([&imageDao, &migrationStop]() {
    db::ImageDao::ReconcileReport rep;
    std::string recErr;
    if (imageDao.reconcile(&rep, &recErr, &migrationStop)) {
      logInfo("main", "files reconcile done: " + rep.detail);
    } else {
      logWarn("main", "files reconcile failed: " + recErr + " (" + rep.detail + ")");
    }
  });

  // ---- 7.8 启动回填 job（M2 T2.2b⑥）：develop_settings 派生缓存对齐 sidecar ----
  //     真空档收口：迁移 003 只建表不扫盘，升级库（有已编辑照片、缓存为空）此前
  //     永远不会回填。首跑（表空）全量重建，之后每次启动按 sidecar_mtime 增量；
  //     低优先级（批间让出）、可取消（jobs.shutdown 置位 + 批间检查，延迟 ≤ 一批）、
  //     有进度（event.job.progress）、失败不静默（failed/unavailable 均显式 warn）。
  //     与 7.5 XMP 迁移并行执行：即便读到迁移中途的 sidecar，mtime 增量下次启动自愈。
  jobs::submitRebuildDevelopSettings(&jobs, &database, &xmpService);

  // ---- 7.6 诊断看门狗：主循环状态变化/心跳落日志（挂死定位用） ----
  std::atomic<bool> watchdogStop{false};
  std::thread watchdog([&]() {
    int last = -1;
    int beats = 0;
    while (!watchdogStop.load()) {
      for (int i = 0; i < 30 && !watchdogStop.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (watchdogStop.load()) break;
      int st = server.loopState();
      ++beats;
      if (st != last || beats % 6 == 0) {
        logInfo("watchdog", "loopState=" + std::to_string(st) + " shutdown=" +
                                std::to_string(server.isShuttingDown() ? 1 : 0) +
                                " poolPending=" + std::to_string(pool.pendingCount()));
        last = st;
      }
    }
  });
  watchdog.detach();

  // ---- 8. 主循环 ----
  int exitCode = server.run();

  // ---- 9. 优雅退出：flush sidecar / 停止监测 ----
  watchdogStop.store(true);
  // 后台迁移优雅退出：置 stop 后处理完当前张即收尾，join 保证退出前迁移线程已结束
  // （迁移持 per-photo 写锁，须先于 sessions.shutdown 完成，避免与编辑会话写 sidecar 竞争）
  migrationStop.store(true);
  if (migrationThread.joinable()) migrationThread.join();
  if (reconcileThread.joinable()) reconcileThread.join();
  logInfo("main", "shutting down, pool pending=" + std::to_string(pool.pendingCount()));
  sessions.shutdown();
  logInfo("main", "cleanup: sessions flushed");
  jobs.shutdown();
  logInfo("main", "cleanup: jobs cancelled");
  // 退出前再补一份当日快照（WP-A2）：首启快照是空库，会话内新增数据必须落进当日备份，
  // 否则"损坏→恢复"会回退到空库（本轮 A2 验证抓到的真实缺陷）。
  {
    std::string outPath;
    std::string bErr;
    if (dbBackup.backupNow(&outPath, &bErr)) {
      logInfo("main", "shutdown db backup: " + outPath);
    } else {
      logWarn("main", "shutdown db backup failed: " + bErr);
    }
  }
  dbBackup.stopDaily();
  logInfo("main", "cleanup: db backup thread stopped");
  watcher.stop();
  logInfo("main", "cleanup: watcher stopped");
  pool.shutdown();
  logInfo("main", "cleanup: pool joined");
  database.close();
  logInfo("main", "cleanup: db closed");
  config.releaseLock();
  logInfo("main", "cleanup: lock released");
  Logger::instance().shutdown();
  return exitCode;
}
