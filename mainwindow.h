#ifndef MAINWINDOW_H
#define MAINWINDOW_H // 头文件保护：避免重复包含


#include <QMainWindow>
#include <QTimer>           // 定时器
#include <QFile>            // 文件读写
#include <QAudioFormat>     // 音频格式描述
#include <QIODevice>        // IO 抽象基类
#include <QAudioDevice>     // 音频设备信息
#include <QMediaDevices>    // 多媒体设备枚举
#include <QCloseEvent>      // 关闭事件
#include <QElapsedTimer>    // 高精度耗时统计
#include <QThread>          // 线程对象
#include <QWebSocket>       // WebSocket 信令通信
#include <QHash>            // 哈希表容器
#include <QListWidget>      // 列表控件
#include <QListWidgetItem>  // 列表项
#include <QDockWidget>      // 可停靠面板
#include <QPushButton>      // 按钮
#include <QToolButton>      // 工具按钮（可挂菜单）
#include <QPlainTextEdit>   // 纯文本编辑/日志显示
#include <QLabel>           // 标签显示
#include <QImage>           // 图像数据
#include <QFrame>           // 框架容器
#include <QStackedLayout>   // 页面切换布局
#include <QVector>          // 动态数组
#include <QSet>             // 集合（去重）
#include <QAction>          // 动作对象（菜单项）
#include <QTextEdit>        // 富文本编辑（此处主要用于聊天输入）
#include <QColor>           // 颜色值
#include <QComboBox>        // 下拉框
#include <QSpinBox>         // 数字选择框
#include <QSlider>          // 滑块
#include <QLine>            // 线段（白板笔迹）
#include <QLineEdit>        // 单行输入框（登录/房间号）
#include <QCheckBox>        // 复选框（记住账号）
#include <QQueue>           // 队列（最近日志缓存）
#include <atomic>           // 原子变量
#include <memory>           // 智能指针
#include "aidetectionclient.h"

// =========================
// 项目内部模块头文件
// =========================
#include "videocapture.h"   // 视频采集（摄像头/屏幕共享）
#include "audiocapture.h"   // 音频采集
#include "avrecorder.h"     // 本地录制
#include "avsender.h"       // UDP 发送（历史链路）
#include "avreceiver.h"     // UDP 接收（历史链路）
#include "rtmppusher.h"     // RTMP 推流
#include "avnetencoder.h"   // 视频编码（H264）
#include "rtmppuller.h"     // RTMP 拉流
#include "avaudioencoder.h" // 音频编码（AAC）

// =========================
// FFmpeg C API 头文件
// 使用 extern "C" 避免 C++ 名字改编
// =========================
extern "C" {
#include <libavcodec/avcodec.h>       // 编解码核心
#include <libavformat/avformat.h>     // 封装/解封装
#include <libavutil/opt.h>            // 参数设置工具
#include <libavdevice/avdevice.h>     // 设备输入输出
#include <libswscale/swscale.h>       // 视频像素格式转换
#include <libswresample/swresample.h> // 音频重采样
}

// Qt Designer 自动生成的 UI 前向声明
QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class AiAssistantDialog;
class AiSegmentationClient;

// =========================================================
// MainWindow：项目总控类
// 负责 UI 编排、会议生命周期、信令、推拉流、白板、登录等
// =========================================================
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr); // 构造：初始化 UI 与各子系统
    ~MainWindow();                         // 析构：触发完整停会与资源回收

    // 事件重写：统一拦截键盘/鼠标/控件事件
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;   // 窗口关闭时兜底停会
    void resizeEvent(QResizeEvent *event) override; // 窗口尺寸变化时同步布局

private slots:
    // ===== 主流程按钮槽函数 =====
    void on_startMeetingButton_clicked(); // 开始会议（采集+编码+推流）
    void on_captureImageButton_clicked(); // 拍照（优先焦点远端，否则本地）
    void on_startRecordButton_clicked();  // 开始录制（UI 入口）
    void on_stopRecordButton_clicked();   // 停止录制（UI 入口）

    // ===== 会话与登录控制 =====
    void on_startReceiveButton_clicked(); // 连接/断开信令
    void on_logoutButton_clicked();       // 退出登录
    void on_stopMeetingButton_clicked();  // 结束会议（全链路回收）

    // ===== WebSocket 信令事件回调 =====
    void onSignalConnected();                     // 信令连接成功
    void onSignalDisconnected();                  // 信令连接断开
    void onSignalTextMessage(const QString &msg); // 收到文本消息（JSON 协议）

    // ===== 成员列表交互 =====
    void onRoomUserDoubleClicked(QListWidgetItem *item); // 双击成员聚焦
    void onRoomListContextMenu(const QPoint &pos);       // 右键管理菜单
    void onSendChatClicked();                            // 发送聊天消息
    void onAskAiClicked();                               // 发送 AI 助手请求

    void applyResponsiveLayout();                       // 根据窗口宽度调整紧凑布局
    bool isCompactMeetingLayout() const;
    bool isUltraCompactMeetingLayout() const;
    void updateMeetingHeaderUi();
    void updatePrimaryActionTexts();
    void updateRoomPanelToggleText();
    void updateRoomDockPresentation();

    void loadAppConfig();
    QString resolveAppConfigPath() const;
    QString buildRtmpPublishUrl(const QString &stream) const;
    QString buildRtmpPlayUrl(const QString &stream) const;

private:
    // =====================================================
    // 成员状态模型（由服务端 members 快照驱动）
    // =====================================================
    struct MemberState {
        QString user;         // 用户名/账号
        QString stream;       // 流 ID（room_user 形式）
        bool audio = true;    // 麦克风状态
        bool video = true;    // 摄像头状态
        bool pub = false;     // 是否正在推流
        bool host = false;    // 是否主持人
        bool cohost = false;  // 是否联席主持人
        bool share = false;   // 是否正在共享屏幕
    };

    // ===== 信令与成员面板相关 =====
    void setupSignalUi();                          // 初始化信令面板 UI
    bool ensureRoomIdentity(bool askRoomIfEmpty); // 确保 room/user/stream 就绪
    void appendRoomEvent(const QString &text);    // 写入系统事件日志
    void refreshRoomUserList();                   // 刷新成员列表与状态图标
    void setupBottomMenus();                      // 初始化底部“美颜/更多”菜单
    void refreshSelfControlActions();             // 刷新“自控”按钮文本状态

    // ===== 信令消息发送 =====
    void sendSignalJoin();                                    // 发送 join
    void sendSignalLeave();                                   // 发送 leave
    void sendSignalUpdate();                                  // 发送 update（音视频/推流状态）
    void sendSignalCmd(const QString &toStream, const QString &action); // 发送管理命令
    void sendSignalChat(const QString &content);              // 发送聊天消息

    // 屏幕共享候选项（屏幕或窗口）
    struct ShareSourceCandidate
    {
        bool isWindow = false; // false=屏幕, true=窗口
        int screenIndex = 0;   // 屏幕索引
        quint64 windowId = 0;  // 窗口句柄（Windows）
        QString label;         // UI 显示名
    };
    QVector<ShareSourceCandidate> buildShareSourceCandidates() const; // 枚举共享源
    void chooseShareSource();                                          // 弹窗选择共享源

    // 追加聊天消息到聊天窗口（含去重与时间戳展示）
    void appendChatMessage(const QString &fromStream, const QString &content, qint64 tsMs, bool isSelf, const QString &msgId = QString());
    QString extractAiPrompt(const QString &content) const;                      // 从输入中提取 AI 提问正文
    void updateAiAssistantUi();                                                 // 刷新 AI 按钮状态
    void showAiAssistantDialog(const QString &prompt = QString(), bool autoSubmit = false); // 打开 AI 助手小窗

    // ===== 远端宫格/焦点渲染相关 =====
    void setupRemoteGridUi();                                 // 初始化远端宫格 UI
    void syncRemoteContainerGeometry();                       // 同步远端容器尺寸
    void refreshRemoteTiles();                                // 刷新宫格卡片
    void applyTileFrame(const QString &stream, const QImage &img); // 把画面贴到某个卡片
    void clearAllTileFrames();                                // 清空所有卡片画面
    void updateFocusStatusBadge();                            // 更新焦点状态浮标
    void syncGridPullers();                                   // 按成员状态同步拉流会话
    void ensurePullSession(const QString &stream);            // 确保某流有拉流会话
    void stopPullSession(const QString &stream, bool waitForQuit = true); // 停止单路拉流
    void stopAllPullSessions(bool waitForQuit = true);        // 停止全部拉流
    void applyFocusAudioRouting();                            // 仅焦点流输出音频
    QStringList currentDisplayStreams() const;                // 当前应显示的推流列表

    // ===== 弱网自适应与统计 =====
    void setupAdaptiveNetworkControl();                  // 初始化自适应恢复定时器
    void applyAdaptiveProfile(int level, bool restartPipeline); // 切换推流档位
    void handleNetworkIssue(const QString &reason, int weight = 1); // 记录网络问题并降档
    void tryRecoverAdaptiveProfile();                    // 网络恢复后尝试升档
    void setupMeetingStatsUi();                          // 初始化统计栏
    void updateMeetingStatsUi();                         // 刷新统计内容
    void exportRecentMeetingLogs();                      // 导出最近会中日志

    // ===== 拉流/采集控制 =====
    void startPullStream(const QString &stream);   // 聚焦某流并开始拉流
    void stopCurrentPull(bool waitForQuit = true); // 停止当前拉流
    void startAudioCapture();                      // 启动音频采集线程
    void stopAudioCapture();                       // 停止音频采集线程

    // ===== 本地视频源与美颜 =====
    void applyLocalVideoSource();                            // 应用摄像头/共享源切换
    void applyBeautyToWorker();                              // 同步美颜参数到采集线程
    void applyAiVirtualBackgroundToWorker();                 // 同步 A2 虚拟背景参数到采集线程
    void setBeautyMode(const QString &modeName, int level);  // 设置美颜模式与强度
    void setAiVirtualBackgroundMode(const QString &modeName); // 设置 A2 虚拟背景模式
    void setAiVirtualBackgroundColor(const QColor &color, const QString &displayName); // 设置 A2 纯色背景
    void chooseAiVirtualBackgroundImage();                   // 选择 A2 图片背景
    void saveAiVirtualBackgroundPrefs() const;               // 持久化 A2 虚拟背景配置
    void saveAiDetectionPrefs() const;                       // 持久化 A3 检测配置
    void setAiDetectionPreviewEnabled(bool enabled);         // 设置 A3 本地预览调试开关
    bool isAiVirtualBackgroundEffectActive() const;          // A2 当前是否处于实际生效状态
    void updateAiDetectionClientConfig();                    // 同步 A3 检测客户端配置
    void requestAiDetectionPreview(const QImage &frame);     // 请求 A3 检测（仅本地预览调试）
    QImage buildAiDetectionPreviewFrame(const QImage &frame) const; // 在本地预览上绘制 A3 检测框

    // ===== 白板相关 =====
    void setupWhiteboardUi();                     // 初始化白板控件
    void ensureWhiteboardCanvas();                // 确保白板画布尺寸有效
    void updateWhiteboardCanvasLabel();           // 刷新白板显示
    QPoint mapWhiteboardPoint(const QPoint &widgetPos) const; // 坐标映射：控件坐标 -> 画布坐标

    QColor whiteboardSelectedColor() const; // 当前选中的白板颜色
    int whiteboardSelectedWidth() const;    // 当前选中的画笔粗细
    bool canClearWhiteboard() const;        // 当前角色是否允许清空白板

    // 绘制白板线段；broadcast=true 时会发信令同步给其他端
    void drawWhiteboardLine(const QPoint &from, const QPoint &to, bool broadcast,
                            const QColor &color, int width, const QString &strokeId, const QString &ownerStream);
    void redrawWhiteboardFromStrokes(); // 按缓存笔迹重绘整张白板
    void removeStrokeById(const QString &strokeId, const QString &byStream = QString()); // 删除某条笔迹
    void undoLastLocalStroke(bool broadcast); // 撤销自己最后一笔

    void clearWhiteboard(bool broadcast, const QString &byStream = QString()); // 清空白板
    void sendSignalWhiteboardDraw(const QPoint &from, const QPoint &to,
                                  const QColor &color, int width, const QString &strokeId); // 发 draw
    void sendSignalWhiteboardClear();                    // 发 clear
    void sendSignalWhiteboardUndo(const QString &strokeId); // 发 undo

    bool canWriteWhiteboard() const;                  // 当前角色是否可写白板
    bool canManageWhiteboard() const;                 // 当前角色是否可锁/解锁白板
    void applyWhiteboardLockUi();                     // 按权限刷新白板工具状态
    void sendSignalWhiteboardLock(bool locked);       // 发 lock/unlock
    bool captureFocusedRemoteImage(QImage *outImage) const; // 抓取焦点远端帧用于拍照

    // ===== 登录/鉴权与信令连接管理 =====
    bool ensureSignalCredential();                      // 校验登录参数是否完整
    void openSignalConnection();                        // 打开 WebSocket 连接
    void resetSignalReconnectState();                   // 重置重连计数与定时器
    void scheduleSignalReconnect(const QString &reason = QString()); // 计划指数退避重连
    void sendSignalAuthLogin();                         // 发送登录请求
    void sendSignalAuthRegister();                      // 发送注册请求
    void setupLoginUi();                                // 创建登录遮罩层 UI
    void syncLoginOverlayGeometry();                    // 同步遮罩层几何
    void showLoginOverlay(bool show, const QString &hint = QString()); // 显示/隐藏登录遮罩
    void triggerLoginAction(bool registerFirst);        // 执行“登录或注册后登录”
    void loadLoginPrefs();                              // 读取本地登录偏好
    void saveLoginPrefs() const;                        // 保存本地登录偏好

private:
    // ===== UI 与通用对象 =====
    Ui::MainWindow *ui = nullptr; // Qt Designer 生成 UI 对象
    QTimer *timer = nullptr;      // 预留通用定时器（当前未作为核心流程）

    // ===== 视频采集线程 =====
    QThread *videoThread = nullptr;      // 视频线程
    VideoCapture *videoWorker = nullptr; // 视频采集 worker

    // ===== 音频采集线程 =====
    QThread *audioThread = nullptr;      // 音频线程
    AudioCapture *audioWorker = nullptr; // 音频采集 worker

    // ===== 信号连接句柄（便于动态断开）=====
    QMetaObject::Connection recordConn;    // 采集音频 -> 录制连接
    QMetaObject::Connection audioSendConn; // 采集音频 -> 音频编码连接
    QMetaObject::Connection videoSendConn; // 采集视频 -> 视频编码连接
    QMetaObject::Connection videoPushConn; // 视频编码 -> 推流连接
    QMetaObject::Connection audioPushConn; // 音频编码 -> 推流连接

    // ===== 录制状态 =====
    bool isRecording = false;       // 是否处于 AV 录制态
    AvRecorder *recorder = nullptr; // 录制器
    bool camRecording = false;      // 摄像头视频录制标志
    int recFps = 30;                // 录制帧率
    qint64 lastPushMs = 0;          // 上次推送录制时间（调试/节流）
    std::shared_ptr<std::atomic_int> videoQueueDepthToken = std::make_shared<std::atomic_int>(0); // 编码在途帧计数
    qint64 lastVideoDropProtectLogMs = 0; // 上次“丢帧保护”日志时间
    std::shared_ptr<std::atomic_int> videoPushQueueDepthToken = std::make_shared<std::atomic_int>(0); // 推流在途视频包计数

    // ===== UDP 历史链路（当前主线不是它）=====
    AVSender *sender = nullptr;     // UDP 发送器
    AVReceiver *receiver = nullptr; // UDP 接收器

    // ===== RTMP 主链路对象 =====
    RtmpPusher *pusher = nullptr;      // 推流器
    AvNetEncoder *netEnc = nullptr;    // 视频编码器
    AvAudioEncoder *audioEnc = nullptr; // 音频编码器

    // 单路拉流会话对象：一个 stream 对应一组 puller + thread + 重试状态
    struct PullSession {
        QString stream;            // 目标流 ID
        rtmppuller *puller = nullptr; // 拉流 worker
        QThread *thread = nullptr; // 拉流线程
        int retryCount = 0;        // 当前会话累计重试次数
        quint64 retryEpoch = 0;    // 重试代次（用于废弃过期重试任务）
    };
    QHash<QString, PullSession*> pullSessions; // stream -> PullSession

    // 推流档位配置（自适应降档/升档使用）
    struct PublishProfile {
        QString name;          // 档位名（高清/均衡/流畅）
        int width = 1280;      // 输出宽
        int height = 720;      // 输出高
        int fps = 30;          // 目标帧率
        int bitrate = 2200000; // 视频码率
        int shareMaxW = 1920;  // 共享模式最大宽
        int shareMaxH = 1080;  // 共享模式最大高
    };
    int adaptiveProfileLevel = 0;   // 当前档位等级
    int adaptiveStressScore = 0;    // 网络压力分
    qint64 lastAdaptiveIssueMs = 0; // 最近一次网络问题时间
    qint64 lastAdaptiveLogMs = 0;   // 最近一次网络问题日志时间
    QTimer *adaptiveRecoverTimer = nullptr; // 定时尝试升档
    QString currentPushUrl;         // 当前推流 URL

    // 编码/推流线程
    QThread *encThread = nullptr;      // 视频编码线程
    QThread *audioEncThread = nullptr; // 音频编码线程
    QThread *pushThread = nullptr;     // 推流线程

    // 会中停止状态
    bool audioStopped = false;   // 音频采集是否已停
    bool meetingStopped = false; // 会议是否已停

    // ===== 信令连接状态 =====
    QWebSocket *signalSocket = nullptr; // WebSocket 客户端
    bool signalConnected = false;       // 当前是否连接成功
    QString signalUrl = "ws://8.166.132.119:9001"; // 信令服务地址
    QString rtmpPublishBaseUrl = "rtmp://8.166.132.119/live"; // RTMP推流基地址
    QString rtmpPlayBaseUrl = "rtmp://8.166.132.119/live";    // RTMP拉流基地址
    bool aiAssistantEnabled = true;                            // 是否启用本地 AI 助手
    QString aiServiceBaseUrl = "http://127.0.0.1:18080";     // AI 服务基地址
    int aiTimeoutMs = 600000;                                  // AI 请求超时
    QString aiAssistantName = QStringLiteral("AI助手");       // AI 助手显示名
    AiSegmentationClient *aiSegmentationClient = nullptr;     // A2 分割客户端（P2，不接主链）
    AiDetectionClient *aiDetectionClient = nullptr;           // A3 检测客户端（P2/P3，仅本地预览）
    bool aiVirtualBackgroundEnabled = false;                  // A2 虚拟背景总开关
    QString aiVirtualBackgroundMode = QStringLiteral("off");  // A2 模式：off/blur
    QColor aiVirtualBackgroundColor = QColor(QStringLiteral("#ddebff")); // A2 纯色背景颜色
    QString aiVirtualBackgroundImagePath;                     // A2 图片背景路径
    QImage aiVirtualBackgroundImage;                          // A2 图片背景缓存
    int aiVirtualBackgroundBlurStrength = 45;                 // 背景虚化强度
    int aiSegmentationRequestInterval = 4;                    // 每 N 帧请求一次分割
    int aiSegmentationTimeoutMs = 1500;                       // 分割请求超时
    int aiSegmentationMaxInputSide = 512;                     // 分割请求最大输入边长
    bool aiDetectionEnabled = false;                          // A3 总开关
    bool aiDetectionPreviewEnabled = false;                   // A3 本地预览调试显示
    int aiDetectionRequestInterval = 12;                      // 每 N 帧请求一次检测
    int aiDetectionTimeoutMs = 1200;                          // A3 检测请求超时
    int aiDetectionMaxInputSide = 640;                        // A3 检测输入最大边长
    double aiDetectionConfThreshold = 0.35;                   // A3 置信度阈值
    double aiDetectionIouThreshold = 0.45;                    // A3 NMS IOU 阈值
    int aiDetectionMaxDetections = 12;                        // A3 最大检测数
    int aiDetectionRequestTick = 0;                           // A3 请求帧计数
    bool aiDetectionRequestPending = false;                   // A3 是否有在途请求
    AiDetectionList latestAiDetections;                       // A3 最近检测结果
    QSize latestAiDetectionImageSize;                         // A3 检测时的图像尺寸
    qint64 latestAiDetectionLatencyMs = 0;                    // A3 最近检测延迟
    qint64 latestAiDetectionUpdatedMs = 0;                    // A3 最近结果更新时间
    qint64 lastAiDetectionErrorLogMs = 0;                     // A3 错误日志节流
    QString activeDeployProfile = "cloud";                   // 当前部署配置名
    QString appConfigPath;                                    // 实际加载的配置文件路径
    QTimer *signalReconnectTimer = nullptr; // 重连计时器
    int signalReconnectAttempt = 0;         // 当前重连尝试次数
    int signalReconnectMaxAttempt = 12;     // 最大重连次数
    bool manualSignalDisconnect = false;    // 是否用户手动断开（手动断开不自动重连）
    QString roomId;         // 房间号
    QString userId;         // 用户 ID
    QString selfStream;     // 本端 stream ID（room_user）
    QString roomHostStream; // 当前房间主持人的 stream

    // ===== 本端发布状态 =====
    bool localAudioOn = true;  // 本端麦克风开关
    bool localVideoOn = true;  // 本端摄像头开关
    bool isPublishing = false; // 当前是否成功推流中
    bool shuttingDown = false; // 应用是否处于关闭流程

    // 成员状态与焦点状态
    QHash<QString, MemberState> memberStates; // stream -> 成员状态
    QString preferredRemoteStream;            // 偏好聚焦流
    QString currentRemoteStream;              // 当前播放主流
    bool stopMeetingInProgress = false;       // 停会重入保护

    // ===== 右侧 Dock 与聊天/成员控件 =====
    QDockWidget *roomDock = nullptr;             // 房间 Dock
    QPushButton *connectSignalButton = nullptr;  // 预留连接按钮（兼容旧 UI）
    QLabel *signalStateLabel = nullptr;          // 信令状态标签
    QLabel *roomCountLabel = nullptr;            // 在线人数标签
    QLabel *meetingStatsLabel = nullptr;         // 顶部会中统计标签
    QListWidget *roomUserList = nullptr;         // 成员列表
    QPlainTextEdit *roomEventLog = nullptr;      // 事件日志框
    QPlainTextEdit *chatMessageLog = nullptr;    // 聊天消息显示框
    QTextEdit *chatInputEdit = nullptr;          // 聊天输入框
    QPushButton *sendChatButton = nullptr;       // 聊天发送按钮
    QPushButton *askAiButton = nullptr;          // AI 助手按钮
    QSet<QString> seenChatMsgIds;                // 已显示聊天消息 ID（去重）
    quint64 chatLocalSeq = 0;                    // 本地聊天自增序号
    AiAssistantDialog *aiAssistantDialog = nullptr; // AI 助手弹窗
    QAction *selfMicToggleAction = nullptr;      // “静音我自己”菜单动作
    QAction *selfCamToggleAction = nullptr;      // “关闭我的摄像头”菜单动作
    QAction *selfShareToggleAction = nullptr;    // “开始/停止共享”菜单动作
    QAction *virtualBgOffAction = nullptr;       // “虚拟背景-关闭”
    QAction *virtualBgBlurAction = nullptr;      // “虚拟背景-背景虚化”
    QAction *virtualBgColorMintAction = nullptr; // “虚拟背景-纯色-薄荷绿”
    QAction *virtualBgColorSkyAction = nullptr;  // “虚拟背景-纯色-浅天蓝”
    QAction *virtualBgColorCreamAction = nullptr; // “虚拟背景-纯色-暖米白”
    QAction *virtualBgImageAction = nullptr;     // “虚拟背景-图片背景”
    QAction *aiDetectOffAction = nullptr;        // “A3 目标检测-关闭”
    QAction *aiDetectPreviewAction = nullptr;    // “A3 目标检测-本地预览调试”

    // 宫格单卡片数据结构
    struct RemoteTile {
        QFrame *frame = nullptr;       // 卡片外框
        QLabel *videoLabel = nullptr;  // 视频画面标签
        QLabel *nameLabel = nullptr;   // 名称标签
        QLabel *stateLabel = nullptr;  // 状态标签（未推流等）
        QLabel *cornerBadge = nullptr; // 角标（主持/麦克风/摄像头）
        QString stream;                // 该卡片绑定的 stream
        bool hasFrame = false;         // 是否已有视频帧
    };
    QWidget *remoteContainer = nullptr;          // 远端展示容器
    QWidget *remoteGridPage = nullptr;           // 宫格页面
    QStackedLayout *remoteStack = nullptr;       // 宫格/焦点页面栈
    QGridLayout *remoteGridLayout = nullptr;     // 宫格布局
    QLabel *focusStatusLabel = nullptr;          // 焦点状态浮标
    QVector<RemoteTile> remoteTiles;             // 宫格卡片数组
    QHash<QString, int> streamToTile;            // stream -> tile 下标映射
    QHash<QString, QImage> latestRemoteFrames;   // 缓存每路最近一帧
    QString focusedStream;                       // 当前焦点 stream
    bool focusMode = false;                      // 是否处于焦点模式
    bool focusPreviewFullScreen = false;         // 焦点是否全屏预览
    bool localScreenShareOn = false;             // 本端是否屏幕共享中
    int shareScreenIndex = 0;                    // 共享屏幕索引

    // ===== 美颜状态 =====
    int localBeautyLevel = 60; // 0~100，美颜强度
    // 注意：保留原字面值，不改业务逻辑；仅补注释说明
    QString localBeautyMode = QStringLiteral("关闭"); // 当前美颜模式名（默认“关闭”）
    int localBeautyStyle = 0; // 0关 1自然 2清晰 3柔和 4磨皮 5瘦脸 6祛皱
    QSlider *beautyStrengthSlider = nullptr; // 强度滑条
    QLabel *beautyStrengthValueLabel = nullptr; // 强度数值标签
    QToolButton *whiteboardLockButton = nullptr; // 白板锁定按钮（放在此处是因为更多菜单共用）
    bool whiteboardLocked = false; // 白板是否锁定

    // ===== 屏幕共享目标 =====
    quint64 shareWindowId = 0; // 共享窗口句柄（0 表示共享屏幕）
    // 注意：保留原字面值，不改业务逻辑；仅补注释说明
    QString shareSourceName = QStringLiteral("屏幕1"); // 当前共享源显示名

    // ===== 白板控件与状态 =====
    QLabel *whiteboardCanvasLabel = nullptr;      // 白板画布标签
    QPushButton *whiteboardClearButton = nullptr; // 清空白板按钮
    QToolButton *whiteboardPenButton = nullptr;   // 画笔开关按钮
    QImage whiteboardCanvas;                      // 白板位图缓冲
    bool whiteboardPenEnabled = true;             // 画笔是否启用
    bool whiteboardMouseDown = false;             // 当前是否按下鼠标绘制中
    QPoint whiteboardLastPoint;                   // 上一个绘制点
    QSet<QString> seenWhiteboardMsgIds;           // 已处理白板消息 ID（去重）
    quint64 whiteboardLocalSeq = 0;               // 本地白板消息序号

    QComboBox *whiteboardColorCombo = nullptr; // 白板颜色下拉框
    QSpinBox *whiteboardWidthSpin = nullptr;   // 白板粗细选择
    QPushButton *whiteboardUndoButton = nullptr; // 撤销按钮

    // 单笔白板轨迹：同一 stroke_id 下可能有多段线（拖动产生）
    struct WhiteboardStroke
    {
        QString strokeId;            // 轨迹唯一 ID（用于撤销/删除）
        QString ownerStream;         // 轨迹拥有者 stream
        QColor color = QColor("#e03131"); // 画笔颜色
        int width = 3;               // 画笔宽度
        QVector<QLine> segments;     // 该轨迹包含的线段集合
    };
    QVector<WhiteboardStroke> WhiteboardStrokes; // 白板全部轨迹缓存
    QString whiteboardActionStrokeId;            // 当前鼠标拖动画笔对应的轨迹 ID

    // ===== 登录与鉴权状态 =====
    bool signalAuthed = false;              // 是否完成 auth_ok
    bool authRegisterTried = false;         // 当前流程是否已尝试注册
    QString loginUser;                      // 登录账号
    QString loginPassword;                  // 登录密码（内存中临时保存）
    bool pendingAuthRegister = false;       // 连接后是否先走注册
    QFrame *loginOverlay = nullptr;         // 登录遮罩层
    QFrame *loginCard = nullptr;            // 登录卡片
    QLineEdit *loginUserEdit = nullptr;     // 账号输入
    QLineEdit *loginPasswordEdit = nullptr; // 密码输入
    QLineEdit *loginRoomEdit = nullptr;     // 房间号输入
    QPushButton *loginLoginButton = nullptr; // 登录按钮
    QPushButton *loginRegisterButton = nullptr; // 注册按钮
    QLabel *loginHintLabel = nullptr;       // 登录提示文案
    QCheckBox *rememberLoginCheck = nullptr; // 记住账号勾选框

    // ===== 会中观测数据 =====
    QQueue<QString> recentEventLogs;   // 最近事件日志（用于导出）
    int totalSignalReconnectCount = 0; // 信令累计重连次数
    int totalPullRetryCount = 0;       // 拉流累计重试次数
    QTimer *meetingStatsTimer = nullptr; // 会中统计刷新定时器
};

#endif // MAINWINDOW_H
