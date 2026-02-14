# SmartMeet（中文需求说明）
基于 Qt 6 + FFmpeg + OpenCV + ZLMediaKit(WebRTC SFU) 的多平台智能视频会议与远程教学系统（类腾讯会议）。  
本文件用于保存项目初版需求与总体规划，便于后续对话与协作延续。

## 目标
- 交付 Windows .exe 客户端，支持 2-6 人远程视频会议。
- 使用 ZLMediaKit 作为 WebRTC SFU，实现低延迟互动通话。
- 模块化设计，低耦合高内聚，便于扩展。
- 作为面试级项目，覆盖音视频、网络、UI、数据库等关键技术点。

## 总体架构
客户端（Qt 6）：
- 采集：摄像头、麦克风、屏幕（FFmpeg/QMediaDevices/QScreen）。
- 处理：OpenCV 图像处理（美颜、背景虚化等）。
- 传输：WebRTC 推拉流 + 信令。
- UI：Qt Widgets + Qt Designer，多视频窗口布局。

服务器（云端）：
- ZLMediaKit：WebRTC SFU，负责多路音视频转发。
- STUN/TURN（coturn）：NAT 穿透。
- 可选：自建信令服务（房间/成员/权限管理）。

## 模块划分
1) 音视频采集与播放（FFmpeg + Qt + OpenCV）
- 摄像头视频与麦克风音频采集。
- 屏幕共享（QScreen/X11/QtMultimedia）。
- 本地预览（QImage/QLabel）。
- 本地录制（MP4，音画同步）。

2) 实时传输（WebRTC + ZLMediaKit）
- SFU 模式发布/订阅（WHIP/WHEP 或自定义信令）。
- 房间管理：加入/退出、成员列表、订阅管理。
- 音画同步（PTS/DTS + 时钟对齐）。
- 弱网自适应：码率、帧率、分辨率动态调整。

3) 视觉处理（OpenCV + DNN）
- 人脸检测、美颜（磨皮、亮度增强）。
- 虚拟背景：背景替换/模糊。
- 可扩展：表情识别、眨眼检测、防疲劳提醒。

4) 教学功能
- 白板（QPainter + 鼠标事件）。
- 文件共享（PDF/图片/PPT 预览）。
- 屏幕标注（画笔/橡皮/激光笔）。
- 课件播放（QtWebEngine 或 OpenCV 解图）。

5) 实时聊天
- 文本聊天（QTcpSocket/QUdpSocket 或 WebSocket）。
- 表情、时间戳、消息记录。
- 数据库保存（MySQL/SQLite）。

6) 登录与权限
- 注册/登录（密码哈希存储）。
- 角色：学生/老师/管理员。
- 会话记录、课程记录存档。

7) 多线程与性能
- 采集/编码/解码/网络/UI 分线程运行。
- 线程安全队列传输帧数据。
- 自定义线程池处理任务。

8) 跨平台
- Qt 6 + CMake。
- Windows/Linux/macOS 兼容。
- 编码器与采集后端抽象（DirectShow/AVFoundation/v4l2）。

## 技术栈
- Qt 6（Qt Creator + Qt Designer）
- FFmpeg 7.1.1（新 API：ch_layout）
- OpenCV 4.x
- WebRTC + ZLMediaKit（SFU）
- MySQL/SQLite
- QThread/自定义线程池
- QTcpSocket/QUdpSocket/WebSocket

## 关键约束
- FFmpeg 7.1.1 新版 API（ch_layout 替代 channel_layout）。
- 模块接口清晰、可替换、低耦合。
- CMake 构建，依赖分层管理。

## 网络方案（WebRTC 方向）
- 信令：WebSocket/HTTP（交换 SDP/ICE）。
- STUN/TURN：解决公网穿透与弱网问题。
- ZLMediaKit REST API：房间与流管理。
- SFU：一人推流，多人订阅，满足会议场景。

## 交付物
- Windows .exe 客户端（windeployqt 打包）。
- ZLMediaKit 部署文档（端口、配置、防火墙）。
- 演示视频 + 架构图 + README。

## 演示流程
登录 -> 加入会议 -> 打开摄像头 -> 通话 -> 白板 -> 聊天 -> 录制 -> 退出

## 里程碑建议
M0：本地采集 + 预览。
M1：WebRTC 推到 ZLM + 两端拉流显示。
M2：多用户房间 + 布局 + 共享屏幕。
M3：聊天 + 白板 + 录制。
M4：界面打磨 + 打包 + 演示资料。

## 可选进阶
- 智能降噪/回声消除（SpeexDSP）。
- OCR 识别白板内容生成讲义。
- 接入 RTC SDK（腾讯/阿里）。
- RTMP/OBS 旁路直播。

## 备注
RTMP 可作为旁路直播/录制通道，会议主链路以 WebRTC + SFU 为主。

## 本地优先路线（先跑通再上云）
目标：先在本机完成“推流 + 拉流 + 多端显示”的闭环，再部署到阿里云。
- 本地部署 ZLMediaKit（Windows 或 WSL/Linux），确认 WebRTC 能发布/订阅。
- Qt 客户端先做最小闭环：加入房间 -> 打开摄像头 -> 推流 -> 拉流 -> 显示远端画面。
- 先只做 2 端互通，再扩展到多人订阅与布局。
- 记录关键日志：发布成功、订阅成功、ICE/DTLS 成功。
- 本地稳定后再上云，避免排查网络/防火墙干扰。

## 服务器部署（建议阿里云）
建议配置（2-6 人会议）：
- 2 核 4G / 5Mbps 起步（人数多可提高带宽）。
- 系统：Linux（CentOS/Ubuntu 均可）。
- 固定公网 IP + 域名（可选但推荐）。

需要重点配置：
- ZLMediaKit 配置：
  - 打开 WebRTC/SFU。
  - 设置公网 IP（externIP）。
  - 配置 RTC 端口（UDP 端口或端口段，按 ZLM 配置为准）。
- 防火墙/安全组：
  - 放行 ZLM 使用的 TCP 端口（如 80/443/1935/554/8000 等，按实际配置）。
  - 放行 RTC 的 UDP 端口（rtc.port 或端口段）。
- STUN/TURN（推荐 coturn）：
  - 在公网环境下保证 NAT 穿透与弱网可用性。

## 阿里云部署步骤清单（简版）
1) 购买 ECS（建议 Ubuntu 20.04/22.04 或 CentOS 7/8）。
2) 安装依赖（编译工具、OpenSSL 等）。
3) 拉取 ZLMediaKit 源码并编译：
   - `git clone --recursive https://github.com/ZLMediaKit/ZLMediaKit`
   - `mkdir build && cd build`
   - `cmake ..`
   - `cmake --build . --config Release`
4) 启动 MediaServer：
   - `./MediaServer -c ../conf/config.ini`（以实际路径为准）
5) 配置防火墙/安全组端口并重启服务。
6) 本地客户端改为公网 IP/域名测试。

## ZLMediaKit 配置示例（按版本调整）
说明：不同版本配置项略有差异，请以你实际版本 `conf/config.ini` 为准。

示例（仅展示关键项思路）：
```
[http]
port=80

[rtmp]
port=1935

[rtc]
enable=1
port=8000
externIP=你的公网IP
```
如果你使用了 TURN，请在客户端里配置 TURN 服务器地址与账号。

## 端口与防火墙建议（按实际配置放行）
- TCP: 80/443/1935/554/8000（按 ZLM 配置启用情况）
- UDP: RTC 端口（如 8000 或端口段）
- TURN: 3478/5349（若部署 coturn）

## 自检清单
- ZLM 启动后能访问 HTTP 接口（或 Web 控制台）。
- 客户端能成功发布 WebRTC 流（publish 成功日志）。
- 第二个客户端能订阅并显示远端视频。
- 外网测试通过（非同一局域网）。

## 后续补充建议（本地完成后）
- 信令服务：房间管理、成员列表、权限控制。
- 客户端自动重连、弱网降级（分辨率/码率/帧率自适应）。
- 多人布局策略：网格/主讲模式切换。
