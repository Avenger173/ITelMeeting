# SmartMeet（统一研发与部署手册）

基于 **Qt 6 + Golang+FFmpeg + OpenCV +OpenGL+ ZLMediaKit + WebSocket 信令** 的多人会议客户端（类腾讯会议）项目。  
当前主链路为 **RTMP 推拉 + 自研信令**，后续演进到 **WebRTC 主链路**。

---

## 文档信息

| 项目 | 内容 |
|---|---|
| 文档版本 | v0.7.0（S5 落地版） |
| 更新日期 | 2026-03-20 |
| 适用范围 | 开发 / 测试 / 部署 / 面试展示 |
| 代码仓库基线 | `SmartMeet` + `golangSignaling`（线上） + `signalTest`（历史实现） |
| 当前执行主线 | `S0 -> S3 -> S5 -> S6 -> S7`（`S4` 暂缓） |

---

## 目录

- [1. 项目定位与目标](#1-项目定位与目标)
- [2. 当前状态总览](#2-当前状态总览)
- [3. 系统架构](#3-系统架构)
- [4. 代码结构与核心文件](#4-代码结构与核心文件)
- [5. 功能模块清单（已做/在做/待做）](#5-功能模块清单已做在做待做)
- [6. 本地联调（Windows）](#6-本地联调windows)
- [7. 云端部署（Ubuntu + 阿里云）](#7-云端部署ubuntu--阿里云)
- [8. 信令协议说明（当前实现）](#8-信令协议说明当前实现)
- [9. 数据库与持久化（SQLite -> MySQL）](#9-数据库与持久化sqlite---mysql)
- [10. 主线路线图 S0~S7（唯一执行路线）](#10-主线路线图-s0s7唯一执行路线)
- [11. 回归与验收（S0 固化）](#11-回归与验收s0-固化)
- [12. 已知问题与优化池](#12-已知问题与优化池)
- [13. 研发与提交流程规范](#13-研发与提交流程规范)
- [14. 下一步立刻执行什么](#14-下一步立刻执行什么)

---

## 1. 项目定位与目标

### 1.1 项目定位
- 面向 Windows 普通用户的图形化会议客户端。
- 重点体现 C++/Qt 音视频工程能力与工程化落地能力。
- 当前目标是“稳定可演示 + 可云端部署 + 可持续演进”。

### 1.2 交付目标
- 可运行的 Windows 客户端（`exe`）。
- 云端可部署的一套服务（`ZLMediaKit + golangSignaling + MySQL`，SQLite 作为历史迁移源保留）。
- 完整回归清单、风险台账、部署文档与演示流程。

---

## 2. 当前状态总览

### 2.1 当前技术基线
- 媒体：RTMP（推流/拉流）+ FFmpeg 编解码。
- 信令：WebSocket（当前线上为 `golangSignaling`，本地/历史兼容 `signalTest`）。
- UI：Qt Widgets + `mainwindow.ui`（Designer 主导）。
- 数据：当前线上信令使用 Go + MySQL；`signalTest` 仍保留 `QSQLITE/QMYSQL` 双栈用于历史兼容与迁移。

### 2.2 已稳定的能力（主干）
- 多人房间成员列表与状态同步（`pub/audio/video/role/share`）。
- 并发拉流宫格 + 双击焦点放大 + 焦点音频输出。
- 主持/联席主持控制（静音、开关摄像头、踢出、转主持、联席管理）。
- 聊天、白板、屏幕共享、AV 录制、截图。
- 信令断线重连与恢复能力（手动断开不重连、异常断开可重连）。

### 2.3 仍需改进的重点
- 美颜效果和性能（自然度不足、部分参数表现不稳定）。
- 配置化（仍有服务地址硬编码残留，需 S2 全面收口）。
- 配置化收口仍有残余（客户端少量地址与构建路径仍需继续清理）。
- WebRTC 主链迁移（S4，暂缓）。
- OpenGL 渲染降载（S6）。
- 客户端与 Go 信令的长期稳定性、日志粒度、运维脚本仍需继续收口。

---

## 3. 系统架构

### 3.1 客户端架构（SmartMeet）
- 采集层：
  - `VideoCapture`：摄像头/屏幕采集、基础美颜处理。
  - `AudioCapture`：麦克风采集。
- 编码推流层：
  - `AvNetEncoder`（视频）、`AvAudioEncoder`（音频）。
  - `RtmpPusher` 推流至 ZLM。
- 拉流解码层：
  - `rtmppuller` 拉流、解码、视频回调、音频播放与录制喂入。
- 业务层：
  - `MainWindow`：房间、成员、主持控制、聊天、白板、共享、录制、UI路由。
- 录制层：
  - `AvRecorder`：本地 MP4 音画同步录制。

### 3.2 服务端架构（当前部署）
- ZLMediaKit：媒体收发中枢（RTMP）。
- `golangSignaling`（当前线上 WebSocket 信令）：
  - 房间状态管理。
  - 权限控制（host/cohost/member）。
  - 聊天、白板、主持控制协议转发。
  - 登录注册鉴权。
  - MySQL 持久化（`users`、`meeting_events`）。
- `signalTest`（Qt WebSocket 历史实现）：
  - 保留为协议对照、历史兼容与 SQLite 迁移工具。

### 3.3 典型链路
- 入会：客户端连接信令 -> `join` -> 收成员列表。
- 推流：客户端开始会议 -> RTMP publish。
- 拉流：收到成员 `pub=true` -> 创建拉流会话 -> 宫格显示。
- 控制：主持端发送 `cmd` -> 服务端校验权限 -> 定向下发。
- 结束：停拉流会话 -> 停采集/编码 -> 断信令。

---

## 4. 代码结构与核心文件

### 4.1 SmartMeet 关键文件
- `mainwindow.cpp/.h/.ui`：UI 与会议业务主逻辑。
- `videocapture.cpp/.h`：视频采集、共享源切换、美颜处理。
- `audiocapture.cpp/.h`：音频采集线程。
- `avnetencoder.cpp/.h`：视频编码。
- `avaudioencoder.cpp/.h`：音频编码。
- `rtmppusher.cpp/.h`：RTMP 推流。
- `rtmppuller.cpp/.h`：RTMP 拉流、解码、音频播放。
- `avrecorder.cpp/.h`：本地录制。
- `CMakeLists.txt`：构建配置（当前偏 Windows，本地绝对路径较多）。

### 4.2 信令服务关键文件（独立目录）
- `D:\QTcoding\SmartMeet\golangSignaling\cmd\signalserver\main.go`：Go 信令启动入口、HTTP/WebSocket 服务装配、优雅退出。
- `D:\QTcoding\SmartMeet\golangSignaling\internal\server\hub.go`：房间状态、成员广播、聊天、白板、主持控制。
- `D:\QTcoding\SmartMeet\golangSignaling\internal\storage\store.go`：MySQL 连接、自动建表、用户与事件存储。
- `D:\QTcoding\SmartMeet\golangSignaling\signalserver.ini`：Go 信令监听地址与数据库配置。
- `D:\QTcoding\signalTest\main.cpp`：历史 C++ 信令实现与 SQLite 迁移入口。
- `D:\QTcoding\signalTest\signalstorage.cpp/.h`：历史数据访问层、SQLite/MySQL 双驱动。

### 4.3 当前需重点技术债
- `mainwindow.h/.cpp` 仍有少量服务地址与行为逻辑耦合残留，需继续做 S2 收口。
- `CMakeLists.txt` 与第三方依赖路径仍偏本机化，需后续工程化收口。
- Go 信令当前已兼容现协议，但日志、配置分层、灰度回滚脚本还不够完整。

---

## 5. 功能模块清单（已做/在做/待做）

### 5.1 已完成（可演示）

1. 音视频链路
- 摄像头/麦克风采集。
- H264/AAC 编码。
- RTMP 推流 + RTMP 拉流。
- 焦点流音频输出控制。

2. 房间与成员
- 房间加入/离开。
- 成员状态同步。
- 多路宫格展示。
- 双击焦点放大。

3. 主持控制
- 单人：静音/恢复、关/开摄像头、踢出。
- 全体：静音/恢复、关/开摄像头（不含自己）。
- 角色：设置联席、取消联席、转移主持。

4. 协作能力
- 聊天消息。
- 白板绘制、撤销、清空、锁定。
- 屏幕共享（含共享源选择）。

5. 本地能力
- AV 录制（含远端焦点流录制路径）。
- 截图。

6. 账号与事件
- 注册/登录（当前线上 Go 信令 + MySQL，本地保留 signalTest 历史兼容）。
- `meeting_events` 写入。

### 5.2 在做（收口优化）
- UI 自适配与窗口行为细化。
- 美颜稳定性和自然度优化。
- 共享切换时延与卡顿控制。

### 5.3 待做（按 S 路线）
- S2：全面配置化（去硬编码）。
- S3：SQLite/MySQL 双栈 + 迁移脚本。
- S4：WebRTC 双栈迁移（RTMP 保底回退，当前暂缓）。
- S5：Go 信令替换（已完成并上线）。
- S6：OpenGL 渲染升级。
- S7：交付封版。

---

## 6. 本地联调（Windows）

### 6.1 启动顺序（最小闭环）
1. 启动 ZLMediaKit（确保 1935 可用）。
2. 启动 `signalTest`（默认 `ws://127.0.0.1:9001`）。
3. 启动两个 SmartMeet 实例，输入同一房间号。
4. 点击“连接信令”。
5. 至少一个实例点击“开始会议”。
6. 检查成员列表、宫格拉流、焦点音频、主持控制。

### 6.2 本地快速检查
- 推流端日志应出现：
  - `RtmpPusher started`
  - `header written`
- 拉流端日志应出现：
  - `RtmpPuller open_input ret=0`
  - `stream ready`
  - `first video frame received`

### 6.3 常见本地问题
- `open_input: Immediate exit requested`
  - 常见于会话被 stop 标记或重复切换冲突。
- `QThread destroyed while thread is still running`
  - 线程停机顺序异常，必须走统一 stop 回收路径。
- `QWindowsWindow::setGeometry...`
  - 多为输入框窗口尺寸警告，通常不影响核心链路。

---

## 7. 云端部署（Ubuntu + 阿里云）

### 7.1 推荐单机拓扑
- 1 台 ECS（2C4G，5Mbps 起）。
- 同机部署：
  - ZLMediaKit
  - `golangSignaling`
  - MySQL（线上）
  - `signalTest` 仅作为历史兼容与 SQLite 迁移工具保留，不再作为线上主服务

### 7.2 安全组端口建议
- TCP：22、1935、9001、8080（按实际 HTTP 端口）、可选 80/443。
- UDP：WebRTC 阶段再放行 RTC 端口（如 8000 或端口段）。

### 7.3 ECS 基础命令
```bash
apt update
apt install -y software-properties-common
add-apt-repository -y universe
apt update
apt install -y build-essential cmake git pkg-config libssl-dev zlib1g-dev libevent-dev \
  libgl1-mesa-dev libopengl-dev \
  qt6-base-dev qt6-base-dev-tools libqt6websockets6-dev libqt6sql6-sqlite libqt6sql6-mysql
```

### 7.4 编译与启动 ZLMediaKit
```bash
mkdir -p /opt
cd /opt
git clone --recursive https://github.com/ZLMediaKit/ZLMediaKit.git
cd /opt/ZLMediaKit
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j"$(nproc)"
find /opt/ZLMediaKit -type f -name MediaServer

mkdir -p /opt/smartmeet/zlm
cp /opt/ZLMediaKit/release/linux/Release/config.ini /opt/smartmeet/zlm/config.ini
```

`/etc/systemd/system/zlm.service`：
```ini
[Unit]
Description=ZLMediaKit MediaServer
After=network.target

[Service]
Type=simple
ExecStart=/opt/ZLMediaKit/release/linux/Release/MediaServer -c /opt/smartmeet/zlm/config.ini
WorkingDirectory=/opt/ZLMediaKit/release/linux/Release
Restart=always
RestartSec=2
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
```

启动：
```bash
systemctl daemon-reload
systemctl enable --now zlm
systemctl status zlm --no-pager
```

### 7.5 编译与启动 Go 信令（当前线上）
```bash
cd /opt/smartmeet/golangSignaling
go mod tidy
go build -o signalserver ./cmd/signalserver
./signalserver -config ./signalserver.ini
```

运行目录配置文件：
- `/opt/smartmeet/golangSignaling/signalserver.ini`

最小 MySQL 配置示例：
```ini
[server]
listen_host=0.0.0.0
listen_port=9001

[database]
driver=QMYSQL
database_name=smartmeet
host=127.0.0.1
port=3306
user=smartmeet
password=StrongPass_123!
connect_options=
```

`/etc/systemd/system/smartmeet-go-signal.service`：
```ini
[Unit]
Description=SmartMeet Go Signal Server
After=network.target mysql.service

[Service]
Type=simple
WorkingDirectory=/opt/smartmeet/golangSignaling
ExecStart=/opt/smartmeet/golangSignaling/signalserver -config /opt/smartmeet/golangSignaling/signalserver.ini
Restart=always
RestartSec=2
Environment=TZ=Asia/Shanghai

[Install]
WantedBy=multi-user.target
```

启动：
```bash
systemctl daemon-reload
systemctl enable --now smartmeet-go-signal
systemctl status smartmeet-go-signal --no-pager
```

### 7.6 云端 MySQL（已实测通过）
1. 安装 MySQL：
```bash
apt update
apt install -y mysql-server
systemctl enable --now mysql
```

2. 建库与业务账号：
```bash
mysql -u root -p
```

```sql
CREATE DATABASE IF NOT EXISTS smartmeet CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS 'smartmeet'@'127.0.0.1' IDENTIFIED BY 'StrongPass_123!';
CREATE USER IF NOT EXISTS 'smartmeet'@'localhost' IDENTIFIED BY 'StrongPass_123!';
GRANT ALL PRIVILEGES ON smartmeet.* TO 'smartmeet'@'127.0.0.1';
GRANT ALL PRIVILEGES ON smartmeet.* TO 'smartmeet'@'localhost';
FLUSH PRIVILEGES;
```

3. 修改 `/opt/smartmeet/golangSignaling/signalserver.ini` 为 MySQL 配置。

4. 先停止 systemd，前台验证一次：
```bash
systemctl stop smartmeet-go-signal
cd /opt/smartmeet/golangSignaling
./signalserver -config ./signalserver.ini
```

若日志出现 `Signal server listening on ws://0.0.0.0:9001`，说明连接与自动建表成功。

5. 再切回 systemd：
```bash
systemctl restart smartmeet-go-signal
systemctl reset-failed smartmeet-go-signal
systemctl status smartmeet-go-signal --no-pager
journalctl -u smartmeet-go-signal -n 50 --no-pager
```

6. 验证云端 MySQL：
```bash
mysql -u root -p
```

```sql
USE smartmeet;
SHOW TABLES;
SELECT COUNT(*) FROM users;
SELECT COUNT(*) FROM meeting_events;
```

### 7.7 历史 signalTest：SQLite -> MySQL 迁移
若旧云端实例曾使用 `signalTest + SQLite`，可直接使用历史迁移命令：

```bash
systemctl stop smartmeet-signal
cd /opt/smartmeet/signalTest/build
./signalTest --migrate-sqlite /opt/smartmeet/signalTest/build/smartmeet_auth.db
```

迁移完成后，由 Go 信令继续读取同一个 MySQL 库。

### 7.8 signalTest 退役与 Go 信令接管（已实测通过）
```bash
systemctl stop smartmeet-signal
systemctl disable smartmeet-signal
mv /etc/systemd/system/smartmeet-signal.service /etc/systemd/system/smartmeet-signal.service.bak
systemctl daemon-reload
systemctl mask smartmeet-signal

systemctl enable --now smartmeet-go-signal
systemctl status smartmeet-go-signal --no-pager
ss -lntp | grep 9001
```

说明：
- 旧 `smartmeet-signal` 当前已退役，不再作为线上服务使用。
- 当前线上信令统一由 `smartmeet-go-signal` 提供。

### 7.9 运维常用命令
```bash
systemctl status zlm --no-pager
systemctl status smartmeet-go-signal --no-pager
systemctl restart zlm
systemctl restart smartmeet-go-signal
journalctl -u zlm -f -n 100 -l
journalctl -u smartmeet-go-signal -f -n 100 -l
```

Windows 侧端口验证：
```powershell
Test-NetConnection <ECS公网IP> -Port 1935
Test-NetConnection <ECS公网IP> -Port 9001
Test-NetConnection <ECS公网IP> -Port 8080
```

---

## 8. 信令协议说明（当前实现）

### 8.1 核心消息类型
- `auth_register`：注册
- `auth_login`：登录
- `join`：加入房间
- `leave`：离开房间
- `update`：状态更新（`pub/audio/video/share/role`）
- `cmd`：主持控制指令
- `chat`：聊天消息
- `wb`：白板同步（`draw/undo/clear/lock`）
- `ping`：心跳

### 8.2 常见字段
- 公共字段：`type, room, user, stream, ts, msg_id`
- 状态字段：`pub, audio, video, share, role`
- 控制字段：`action, to`

### 8.3 权限原则
- Host 可管理全部成员。
- CoHost 可管理普通成员，不可管理 Host。
- Member 仅可控制自己状态。
- 服务端权限判定为准，客户端仅做 UI 层过滤。

---

## 9. 数据库与持久化（SQLite -> MySQL）

### 9.1 当前（已落地）
- 线上数据库在 Go 信令服务侧（`golangSignaling` -> MySQL）。
- 历史 C++ 信令 `signalTest` 已支持驱动：`QSQLITE / QMYSQL`。
- 当前线上默认配置：MySQL。
- 当前本地验证：Go 信令 + MySQL 已跑通；`signalTest` 的 SQLite / MySQL 双栈也已跑通。
- 当前云端验证：Go 信令 + MySQL 已跑通，`users` / `meeting_events` 可自动建表并正常写入。
- 已有核心表：
  - `users`
  - `meeting_events`

### 9.2 S3 目标
- 数据访问层抽象，支持 `QSQLITE/QMYSQL` 双驱动切换。
- 提供 MySQL 自动建表与 SQLite 迁移命令。
- 保留 SQLite 作为本地开发/回退方案。

### 9.3 迁移原则
- 先兼容，再切换。
- 同一业务接口在 SQLite/MySQL 行为一致。
- 切换过程可回退，不阻断现网演示。

### 9.4 当前实现细节
- Go 信令 schema 初始化入口：`golangSignaling/internal/storage/store.go` 中的 `InitSchema()`。
- 当前线上表：
  - `users`
  - `meeting_events`
- 历史 SQLite 迁移入口：`signalTest --migrate-sqlite <sqlite_db_path>`。
- 推荐使用方式：
  - 本地快速开发：Go 信令 + MySQL，或 `signalTest` + SQLite/MySQL。
  - 云端长期运行：Go 信令 + MySQL。

---

## 10. 主线路线图 S0~S7（当前 S4 暂缓）

| 阶段 | 目标 | 核心交付 | 退出标准 |
|---|---|---|---|
| S0 | 基线冻结与标准化 | 回归清单 + 风险台账 + 固化流程 | 30 分钟双实例稳定，无 P0/P1 未缓解问题 |
| S1 | 稳定性收口 | 崩溃/卡顿/切换抖动修复，UI适配收敛 | 共享切换 20 次稳定，音画同步可接受 |
| S2 | 配置化 | 去硬编码，`config.ini` + 设置页 | 仅改配置可切换本地/云端环境 |
| S3 | 数据库双栈 | SQLite/MySQL 双驱动 + 迁移脚本 | 登录/事件在双库均通过 |
| S4 | WebRTC 迁移 | 传输抽象 + RTMP/RTC 双栈 + PoC 闭环 | WebRTC 复现核心会议能力 |
| S5 | Go 信令替换 | Go 服务兼容现协议，灰度切换 | 客户端基本无感切换成功 |
| S6 | OpenGL 升级 | 渲染后端可切换，降低多路 CPU 占用 | 多路渲染帧稳定提升且 CPU 下降 |
| S7 | 封版交付 | 一键部署、监控、打包、演示材料 | 1 小时内可复现完整云端演示 |

### 10.1 强制执行规则
- 不跨阶段，不插队开发。
- 每阶段必须“开发 -> 回归 -> 冻结 -> 下一阶段”。
- 出现 P0/P1 稳定性问题，立即停新功能先修复。
- 每阶段结束必须更新 README（状态、风险、回归结果）。

### 10.2 当前执行说明
- 当前版本先不推进 S4。
- S4 保留在路线图中，作为后续协议升级项，不纳入当前交付主线。
- S5 已完成：Go 信令已替换云端 `signalTest` 上线。
- 当前实际顺序更新为：`S0 -> S1 -> S2 -> S3 -> S5(完成) -> S6 -> S7`。

---

## 11. 回归与验收（S0 固化）

### 11.1 模板文件
- 回归清单：`docs/S0_基线冻结回归清单.md`
- 风险台账：`docs/S0_风险台账.md`
- S6 前盘点清单：`docs/S6_进入前盘点清单.md`

### 11.2 执行频率
- S0：每次提交必跑。
- S1~S3：每天至少 1 次全量，改动后补跑受影响模块。

### 11.3 S0 退出标准
- 双实例连续 30 分钟无崩溃、无线程残留。
- 音视频/主持控制/共享/白板/录制/重连全通过。
- 风险台账无未缓解且可复现的 P0/P1。
- 至少保留 1 份完整回归记录（环境、版本、日志路径）。

---

## 12. 已知问题与优化池

### 12.1 当前已知问题
- 美颜效果存在不自然场景（特别是高强度瘦脸）。
- 共享与摄像头切换后偶发短时卡顿（比历史版本已改善）。
- 部分 UI 在分辨率变化时仍需进一步约束。

### 12.2 优化池（仅在当前阶段通过后插入）
- 美颜 V2：ROI + 参数防抖 + 弱网自动降级。
- Go 信令工程化继续收口：日志字段、监控、灰度回滚脚本。
- 视觉渲染优化：OpenGL 管线。
- 自动化回归脚本与日志采集工具。
- 配置中心化（dev/test/prod）。

---

## 13. 研发与提交流程规范

### 13.1 需求与文档
- 先更新 README 再改代码。
- 跨模块改动必须补充回归清单和风险台账。

### 13.2 提交规范
- `feat:` 新功能
- `fix:` 缺陷修复
- `refactor:` 重构
- `docs:` 文档更新

### 13.3 发布与回滚
- 阶段内小步提交，阶段末统一验收打 Tag。
- 出现阻断回归，先回退最近稳定 Tag，再做最小修复补丁。

---

## 14. 下一步立刻执行什么

### 当前建议（马上做）
1. 以当前 RTMP + Go 信令 + MySQL 基线为稳定版本，冻结现有协议字段。
2. 先完成 S6 前盘点，明确当前 UI/渲染/共享/美颜/录制的基线问题。
3. 盘点通过后，再进入 S6：OpenGL 渲染升级。

### 当前阶段的直接技术动作
- 固化当前 Go 信令线上配置、systemd 服务名与运维命令。
- 按 `docs/S6_进入前盘点清单.md` 跑完一轮基线检查。
- 明确 OpenGL 只替换渲染层，不动现有会议协议和业务模型。

---

## 附：常见排障速查

- `ssh` 读取本机配置报错：
```powershell
ssh -F NUL root@<ECS公网IP>
```

- Qt 缺 OpenGL 依赖：
```bash
apt install -y libgl1-mesa-dev libopengl-dev
```

- Go signal 服务异常退出定位：
```bash
journalctl -u smartmeet-go-signal -n 200 -l
```

- ZLM 服务日志跟踪：
```bash
journalctl -u zlm -f -n 100 -l
```
