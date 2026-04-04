# ITELMeeting-SmartMeet

基于 **Qt 6 + FFmpeg + OpenCV + Golang + Python + ZLMediaKit** 的多人视频会议客户端项目，定位为类腾讯会议的 Windows 桌面端工程化作品。

当前稳定基线：
- 客户端主链：`RTMP 推流/拉流`
- 线上信令：`golangSignaling`
- 线上数据库：`MySQL`
- 云端媒体服务：`ZLMediaKit`
- AI 助手：`pythonAiService`

当前不把 WebRTC 作为主开发线。WebRTC 保留在后续规划里，但当前交付与演示基线仍是 `RTMP + Go 信令 + MySQL`。

---

## 目录

- [1. 项目定位](#1-项目定位)
- [2. 当前真实状态](#2-当前真实状态)
- [3. 系统架构](#3-系统架构)
- [4. 仓库结构](#4-仓库结构)
- [5. 已实现功能](#5-已实现功能)
- [6. 本地开发与联调](#6-本地开发与联调)
- [7. 云端部署基线](#7-云端部署基线)
- [8. AI 模块现状](#8-ai-模块现状)
- [9. 路线图](#9-路线图)
- [10. 已知问题](#10-已知问题)
- [11. 相关文档](#11-相关文档)

---

## 1. 项目定位

### 1.1 目标

这个项目的目标不是做一个“只会本地预览”的 demo，而是做一套：
- 可运行的 Windows 视频会议客户端
- 可部署到云端的信令 + 媒体 + 数据存储链路
- 可作为面试项目讲清楚的音视频工程实践

### 1.2 当前交付方向

当前优先级是：
1. 稳定主链
2. 完善 UI/交互与工程化文档
3. 增加 AI 能力作为项目亮点
4. 最后再做渲染升级和更大架构演进

---

## 2. 当前真实状态

### 2.1 技术基线

| 模块 | 当前方案 |
|---|---|
| 客户端 UI | Qt Widgets + Qt Designer |
| 视频链路 | RTMP |
| 编解码 | FFmpeg |
| 本地图像处理 | OpenCV |
| 线上信令 | Go WebSocket 服务 |
| 线上数据库 | MySQL |
| 媒体服务 | ZLMediaKit |
| AI 服务 | Python FastAPI，本地 HTTP 调用 |

### 2.2 当前线上状态

当前线上已经完成：
- `golangSignaling` 替换旧 `signalTest` 成为线上主信令
- Go 信令通过 `systemd` 托管，服务名为 `smartmeet-go-signal`
- MySQL 已作为线上默认持久化数据库
- `meeting_events` 与 `users` 已正常建表和写入
- 旧 `smartmeet-signal` 已退役并 `mask`

### 2.3 当前开发状态

当前推荐开发主线：
- 继续基于 RTMP 主链开发
- 继续基于 Go 信令开发
- AI 线当前已完成 `AI 助手`，下一步是“轻量基线盘点后进入 `AI 虚拟背景`”
- OpenGL 暂未开始正式开发

---

## 3. 系统架构

### 3.1 客户端

客户端主流程：
- `VideoCapture`：摄像头/屏幕采集、本地图像处理、美颜、共享源切换
- `AudioCapture`：麦克风采集
- `AvNetEncoder` / `AvAudioEncoder`：视频/音频编码
- `RtmpPusher`：推流到 ZLM
- `rtmppuller`：拉流、解码、音频播放
- `AvRecorder`：本地录制
- `MainWindow`：房间、成员、主持控制、白板、聊天、共享、录制、AI 助手入口

### 3.2 服务端

当前线上部署组件：
- `ZLMediaKit`
  - 负责 RTMP 媒体流收发
- `golangSignaling`
  - 负责房间管理
  - 成员状态同步
  - 聊天、白板、主持控制协议转发
  - 登录注册
  - MySQL 写入
- `MySQL`
  - 当前已确认核心表：
    - `users`
    - `meeting_events`

### 3.3 AI 服务

当前 AI 服务是独立本地服务：
- 目录：`pythonAiService`
- 协议：`Qt/C++ -> HTTP(JSON) -> Python`
- 当前已实现：
  - `GET /healthz`
  - `POST /assistant`

---

## 4. 仓库结构

### 4.1 客户端主工程

- `mainwindow.cpp/.h/.ui`
- `videocapture.cpp/.h`
- `audiocapture.cpp/.h`
- `avnetencoder.cpp/.h`
- `avaudioencoder.cpp/.h`
- `rtmppusher.cpp/.h`
- `rtmppuller.cpp/.h`
- `avrecorder.cpp/.h`
- `aiassistantdialog.cpp/.h/.ui`
- `smartmeet.ini`
- `CMakeLists.txt`

### 4.2 Go 信令

目录：`golangSignaling`

关键文件：
- `golangSignaling/cmd/signalserver/main.go`
- `golangSignaling/internal/server/hub.go`
- `golangSignaling/internal/storage/store.go`
- `golangSignaling/internal/config/config.go`
- `golangSignaling/signalserver.ini`
- `golangSignaling/deploy/linux/smartmeet-go-signal.service`

### 4.3 Python AI 服务

目录：`pythonAiService`

关键文件：
- `pythonAiService/main.py`
- `pythonAiService/ai_service.ini`
- `pythonAiService/project_facts.md`
- `pythonAiService/requirements.txt`
- `pythonAiService/README.md`

### 4.4 历史兼容服务

目录：`D:\QTcoding\signalTest`

用途：
- 历史 C++ 信令实现
- 协议对照
- SQLite -> MySQL 迁移工具

---

## 5. 已实现功能

### 5.1 会议主链

- 摄像头/麦克风采集
- H264/AAC 编码
- RTMP 推流
- RTMP 拉流
- 宫格显示
- 双击焦点放大
- 焦点流音频输出

### 5.2 房间与权限

- 登录/注册
- 房间加入/离开
- 成员列表同步
- 成员状态同步：
  - `pub`
  - `audio`
  - `video`
  - `share`
  - `role`
- 主持人控制：
  - 单人静音/恢复
  - 单人关/开摄像头
  - 踢出成员
  - 全体静音/恢复
  - 全体关/开摄像头
  - 联席主持设置/取消
  - 转主持

### 5.3 协作能力

- 聊天
- 白板绘制
- 白板撤销
- 白板清空
- 白板锁定/解锁
- 屏幕共享
- 窗口共享

### 5.4 本地能力

- 录制
- 截图
- 基础美颜

### 5.5 AI 助手

当前已实现：
- 独立 AI 助手窗口
- 通用助手 / 项目助手双模式
- 最近几轮对话短期记忆
- 真实大模型优先
- 本地规则型回答兜底
- 项目事实知识源 `project_facts.md`

当前明确未实现：
- 不直接读取代码库
- 不直接查询数据库
- 不直接读取服务器实时状态
- 不广播到房间，默认仅当前客户端可见

---

## 6. 本地开发与联调

### 6.1 最小联调链路

1. 启动本地 `ZLMediaKit`
2. 启动本地信令
   - 可用 `signalTest`
   - 也可用本地 `golangSignaling`
3. 启动两个 SmartMeet 客户端实例
4. 两端进入同一房间
5. 至少一端开始会议
6. 检查音视频、成员同步、主持控制、白板、聊天、共享

### 6.2 客户端配置

配置文件：
- `smartmeet.ini`

主要项：
- `network.active_profile`
- `profile.local.signal_url`
- `profile.cloud.signal_url`
- `ai.enabled`
- `ai.service_url`
- `ai.timeout_ms`

### 6.3 AI 服务本地启动

```powershell
cd D:\QTcoding\SmartMeet\pythonAiService
python -m venv .venv
.\.venv\Scripts\activate
pip install -r requirements.txt
python -m uvicorn main:app --host 127.0.0.1 --port 18080
```

健康检查：

```powershell
curl http://127.0.0.1:18080/healthz
```

### 6.4 本地常见问题

- `QThread destroyed while thread is still running`
  - 多半是停止顺序不统一
- `QWindowsWindow::setGeometry...`
  - 一般是输入对话框尺寸警告，不一定影响主链
- 构建目录 `smartmeet.ini` 与仓库根目录 `smartmeet.ini` 不一致
  - 以运行目录配置为准

---

## 7. 云端部署基线

### 7.1 当前线上拓扑

单机部署：
- Ubuntu ECS
- ZLMediaKit
- `smartmeet-go-signal`
- MySQL

### 7.2 当前线上信令服务

服务名：
- `smartmeet-go-signal`

监听端口：
- `9001`

常用命令：

```bash
systemctl status smartmeet-go-signal --no-pager
systemctl restart smartmeet-go-signal
journalctl -u smartmeet-go-signal -f -n 100 -l
```

### 7.3 当前线上媒体服务

服务名：
- `zlm`

常用命令：

```bash
systemctl status zlm --no-pager
systemctl restart zlm
journalctl -u zlm -f -n 100 -l
```

### 7.4 当前线上数据库

当前线上数据库：
- MySQL

已确认表：
- `users`
- `meeting_events`

验证命令：

```sql
USE smartmeet;
SHOW TABLES;
SELECT COUNT(*) FROM users;
SELECT COUNT(*) FROM meeting_events;
```

### 7.5 安全组建议

- TCP: `22`
- TCP: `1935`
- TCP: `9001`
- TCP: `8080`
- 可选：`80` / `443`

---

## 8. AI 模块现状

### 8.1 当前 A0/A1 已落地

当前 AI 线已完成：
- Python AI 服务目录搭建
- Qt 侧 HTTP 调用链
- 独立 AI 助手窗口
- 双模式切换
- 短期记忆
- 真实大模型接入
- 本地兜底
- 项目事实 grounding V1

### 8.2 当前 AI 助手行为

通用助手：
- 普通问题可正常回答
- SmartMeet 问题也可回答

项目助手：
- 优先回答 SmartMeet 相关问题
- 对未确认实现细节应更保守

### 8.3 当前 AI 助手边界

它不会：
- 自动扫源码
- 自动查数据库
- 自动读线上服务状态

如果要让它对“项目真实细节”回答更准，当前应该更新：
- `pythonAiService/project_facts.md`

而不是继续把事实硬编码进提示词。

### 8.4 当前 A2/A3 状态

当前 AI 主线已经不是“准备进入 A2”，而是：
- A2 已完成第一阶段
- A3 已启动第一阶段

当前真实情况：
- A2：AI 虚拟背景已经落地到客户端，支持 `背景虚化 / 纯色背景 / 图片背景`
- A3：先从 `pythonAiService` 的 YOLO 检测服务端能力开始，不直接碰 Qt 主链

当前推荐顺序是：
1. A1 已收口
2. A2 已完成第一阶段并进入回归观察
3. A3 先做 `/detect` 与模型验证
4. A3 当前已经打通到 Qt 本地预览调试叠框，后续再决定是否升级为更正式的客户端功能
5. S6 仍保持未开始，A5 仍放在 S6 之后

也就是说：
- 当前主线已经从 “进入 A2” 切到 “推进 A3”
- S6 暂时仍不是当前立即开发项
- A5 仍然依赖 S6

---

## 9. 路线图

### 9.1 主线阶段

| 阶段 | 状态 | 说明 |
|---|---|---|
| S0 | 已建立 | 基线冻结、回归模板、风险台账 |
| S1 | 已做过一轮 | 稳定性与 UI 收口 |
| S2 | 进行中 | 配置化继续收口 |
| S3 | 已落地 | SQLite/MySQL 双栈与 MySQL 主用 |
| S4 | 暂缓 | WebRTC 不作为当前主线 |
| S5 | 已完成 | Go 信令替换上线 |
| S6 | 未开始 | OpenGL 渲染升级（A5 前置基础） |
| S7 | 未开始 | 封版交付 |

### 9.2 AI 线阶段

| 阶段 | 状态 | 说明 |
|---|---|---|
| A0 | 已完成第一阶段 | Python AI 服务基础设施 |
| A1 | 已完成第一阶段 | AI 助手 |
| A2 | 已完成第一阶段 | AI 虚拟背景：虚化 / 纯色 / 图片背景已接入客户端 |
| A3 | 已完成当前调试阶段 | YOLO 增强：`/detect` 与 Qt 本地预览调试叠框已打通 |
| A4 | 未开始 | 音频降噪 |
| A5 | 未开始 | AI + OpenGL 结果叠加（依赖 S6） |

### 9.3 当前顺序

1. A1 已完成并收口
2. A2 已完成第一阶段并已接入客户端
3. 当前优先进入 `A3 YOLO 检测` 的收口阶段
4. A3 当前定位是 AI 工程能力验证与后续扩展基础，不是会议主链刚需
5. `S6` 仍未开始，`A5` 仍放在 `S6` 之后

补充说明：
- `A2` 已不再是“未启动项”
- `A3` 当前已经验证了 `Qt -> Python /detect -> 本地预览叠框` 这条视觉 AI 链路
- `A3` 现在主要用于展示 `YOLO / ONNX Runtime / Python 服务 / Qt 异步回传` 这套工程能力
- `S6` 是渲染基础设施升级
- `A5` 是建立在 `S6` 之上的 AI 结果叠加阶段

---

## 10. 已知问题

- 美颜在部分强参数下仍然不自然
- 共享切换后偶发短时卡顿
- 局部 UI 在分辨率变化时还会挤压
- 客户端仍有少量路径和配置耦合残留
- 根 `README` 之外的一些历史文档仍有待继续同步

---

## 11. 相关文档

### 回归与风险

- [S0 基线冻结回归清单](./docs/S0_基线冻结回归清单.md)
- [S0 风险台账](./docs/S0_风险台账.md)
- [S6 进入前盘点清单](./docs/S6_进入前盘点清单.md)

### AI 服务

- [pythonAiService README](./pythonAiService/README.md)
- [AI 项目事实文件](./pythonAiService/project_facts.md)
- [A1 AI 助手验收清单](./docs/A1_AI助手验收清单.md)
- [A2 AI 虚拟背景开发顺序](./docs/A2_AI虚拟背景开发顺序.md)
- [A3 YOLO 增强开发顺序](./docs/A3_YOLO增强开发顺序.md)

---

## 维护说明

这份 README 只保留：
- 当前真实架构
- 当前真实部署状态
- 当前真实开发主线
- 当前真实功能边界

不再把所有阶段的细节都堆在根文档里。  
更细的执行清单、回归表、风险台账和 AI 说明，统一放到各自子文档维护。
