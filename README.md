# 视频点播学习项目（开发中）

项目包含 C++ 服务、网关、FFmpeg 转码工作进程和 Qt 桌面客户端。它是学习实现，仍在完善，不应作为已经验收或可直接公网部署的系统。

## 当前功能

- 访客会话、邮箱验证码登录、昵称和密码登录、个人资料及退出。
- 已登录用户上传视频及可选封面，任务写入数据库并由工作进程转为 HLS；管理员预览并审核，审核通过后公开播放。作者或管理员可以下架，重新公开前需要再次审核。
- 用户关注与取消关注、点赞与取消、播放计数、保存和查看弹幕、按标题或简介搜索、分类和标签筛选及分页。弹幕目前以列表展示，尚未叠加到视频画面；视频时长由上传者输入。
- 头像上传与预览，超级管理员可管理已注册用户的管理权限；停用管理权限不影响其普通用户会话。
- etcd 服务注册发现、FastDFS 文件存储和 RabbitMQ 唤醒；RabbitMQ 不可用时仍由工作进程轮询数据库。
- 作者或管理员可以删除已处理的视频；当前是逻辑删除，视频从列表及公开访问中消失，底层文件暂不自动清除。
- Qt 客户端支持上述主要流程。现有自动化测试在 Linux 容器的无显示模式运行，Windows 客户端的原生打包和显示尚未验证。

## 构建

远程开发目录为 `/home/dev/workspace/vod-platform`。所需的 FastDFS 和消息队列脚手架代码已保存在本项目 `scaffold` 目录，原练习目录保留不动。

需要 C++17、CMake、MySQL 客户端、OpenSSL、FFmpeg、FastDFS、AMQP-CPP、libev；服务端还需要 etcd C++ SDK、BRPC、gflags、Protobuf、JsonCpp 和 cpp-httplib。客户端可选，需要 Qt 6 和 libmpv。

```sh
cd /home/dev/workspace/vod-platform
cmake -S . -B build -DVOD_BUILD_CLIENT=ON
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
```

测试会依据配置启用。`VOD_ETCD_BINARY` 指向 etcd 可执行文件，`VOD_MARIADB_BASEDIR` 指向 MariaDB 的 `usr` 目录，`VOD_TEST_FDFS=ON` 启用隔离的 FastDFS 测试。测试创建临时数据库、文件目录和本机服务，结束后清理；临时测试依赖可能在重启后消失。设置 `VOD_TEST_AMQP_URL` 可额外在集成测试中使用真实 RabbitMQ，测试仅使用并清理自己生成的临时队列和交换机。

配置并构建 `vod_session_mysql_integration` 后，可单独运行任务租约重领用例。

```sh
python3 tests/run_job_lease_reclaim_test.py build
```

用例复用集成测试的临时 MariaDB。它验证 A 租约过期且 B 接手后，A 迟到的失败和完成操作不会改变 B 的任务记录，B 可正常完成并将视频提交审核。2026-11-17 的隔离数据库实测通过。

## 数据和配置

实际运行前先准备一个专用数据库及其账号，设置 `VOD_DB_HOST`、`VOD_DB_PORT`、`VOD_DB_USER`、`VOD_DB_PASSWORD`、`VOD_DB_NAME`，再运行 `bash scripts/init-db.sh` 建表。脚本不会创建数据库本身；`CREATE TABLE IF NOT EXISTS` 不会为旧版表自动增加缺失列，升级旧数据库需单独迁移。若 MySQL 客户端不在 PATH，可用 `VOD_MYSQL_CLIENT` 指定可执行文件路径。邮箱验证码还需要 `VOD_CODE_SECRET` 和 `VOD_SMTP_URL`、`VOD_SMTP_USER`、`VOD_SMTP_PASSWORD`、`VOD_SMTP_FROM` 等配置。不要把真实密码提交进代码。

使用邮箱验证码先注册首位用户后，设置 `VOD_BOOTSTRAP_EMAIL` 为该用户的邮箱，执行 `bash scripts/bootstrap-admin.sh`，可将其初始化为超级管理员。该脚本只在数据库尚无超级管理员时生效，不能用它改动已有管理员；后续管理员在客户端的管理页添加，目标用户须已注册。

网关默认只监听本机 HTTP，远程访问应使用经正确配置的 TLS 代理或安全隧道；当前协议部分操作会将会话标识放入 URL。不要把默认配置直接暴露到公网。

数据库、etcd 和 FastDFS 等依赖服务就绪且数据库表已初始化后，可以在远程终端设置 `VOD_DB_USER`、`VOD_DB_NAME`、`VOD_DB_PASSWORD`、`VOD_FDFS_TRACKER`，再运行 `bash scripts/run-local.sh`。根据环境可设置 `VOD_DB_HOST`、`VOD_DB_PORT`、`VOD_ETCD_ADDRESS`、`VOD_AMQP_URL` 和邮件相关环境变量；脚本不会启动这些外部依赖。脚本在前台运行，Ctrl-C 会停止本次启动的服务，日志默认写入项目内的 `run-logs`。看到“Services launched”只表示进程已经启动，并不代表依赖检查或业务验收通过，应查看日志并验证功能。

使用此工作区配置好的独立数据库和基础服务时，运行 `bash scripts/run-qq.sh`，脚本会启动独立 MySQL、etcd、FastDFS，并初始化项目表。按提示输入已开启 SMTP 的 QQ 发件邮箱和专用授权码；授权码输入时不回显，也不写入脚本或命令历史。脚本每次启动生成新的验证码密钥，重启后之前未使用的验证码会失效。不要把授权码发到聊天或提交到代码。数据库和文件数据保存在 `runtime/`，其中 `runtime/mysql/credentials.env` 含数据库密码，勿上传或分享。以上脚本依赖已经准备好的本地二进制和 FastDFS 配置，并不是全新机器的一键安装器。

在当前 Windows 电脑上，可以双击项目目录里的 `打开视频客户端.cmd`，在弹出的窗口中输入 `dev-docker` 的 SSH 密码。它使用本项目旁的用户级 VcXsrv 和带认证的 SSH X11 转发显示 Linux Qt 客户端；关闭播放器后启动窗口会退出。此方式依赖虚拟机和远程服务正常运行，不是原生 Windows 客户端安装包。首次使用邮件登录后，可用 `scripts/bootstrap-admin.sh` 初始化首位超级管理员；当前工作环境已完成首次初始化。

## 尚未完成

管理员目前只能从已注册账号中添加，尚未提供完整的用户管理；首次超级管理员需要使用初始化脚本。客户端可用性、部署脚本、Windows 打包及更多异常和并发场景还需完善。文件上传后的个别异常窗口需要清理恢复机制，当前不保证生产级可靠性。分阶段学习路线见 `docs/学习路线.md`。
