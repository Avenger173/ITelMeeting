#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QImage>
#include <QPixmap>
#include <QDateTime>
#include <QMessageBox>
#include <QDebug>
#include <QThread>
#include <QAudioFormat>
#include <QPainter>
#include <QApplication>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QLineEdit>
#include <QUrl>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QToolButton>
#include <QSlider>
#include <QSignalBlocker>
#include <QSettings>
#include <QFileDialog>
#include <QFileInfo>
#include <QTextStream>
#include <QStringConverter>
#include <QDir>
#include <QStatusBar>
#include <QSizePolicy>
#include <algorithm>
#include<QHBoxLayout>
#include<QTabWidget>
#include<QGuiApplication>
#include<QScreen>
#include<QActionGroup>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include<Windows.h>
#endif

namespace {
static bool stopThreadAndDelete(QThread *&thr, const char *tag, int quitWaitMs = 3000) {
    if (!thr) return true;  //线程为空，直接返回成功
    thr->quit();    //发送退出信号
    //等待线程退出（超出时间默认3秒）
    if (!thr->wait(quitWaitMs)) {
        //超时处理，避免主线程卡死
        qWarning() << "[Mainwindow]" << tag << "停止超时，分离";
        thr->requestInterruption(); //标记线程中断
        thr->setParent(nullptr);    //解除父对象关联
        QObject::connect(thr, &QThread::finished, thr, &QObject::deleteLater, Qt::UniqueConnection);    //延迟销毁
        thr = nullptr;
        return false;
    }
    //正常退出，直接销毁线程
    delete thr;
    thr = nullptr;
    return true;
}
//把传入的 URL 基础地址「洗干净」
static QString normalizedUrlBase(QString value) {
    value = value.trimmed();
    while (value.endsWith('/')) {
        value.chop(1);
    }
    return value;
}
//安全拼接「基础 URL + 流地址」
static QString buildStreamUrl(const QString &base, const QString &stream) {
    const QString cleanBase = normalizedUrlBase(base);
    const QString cleanStream = stream.trimmed();
    if (cleanBase.isEmpty() || cleanStream.isEmpty()) return QString();
    return QString("%1/%2").arg(cleanBase, cleanStream);
}
}
#ifdef Q_OS_WIN
namespace{//存储窗口句柄和标题的结构体
struct  WinShareEntry
{
    quint64 hwnd=0; //窗口句柄
    QString title;  //窗口标题
};
//windows窗口枚举回调函数：遍历系统可见窗口，收集它们的窗口句柄和标题，存储到QVector容器中
static BOOL CALLBACK enumShareWindowProc(HWND hwnd,LPARAM lParam){
    //将lParam（通用参数类型）强制转换为QVector<WinShareEntry>*类型，指向存储窗口信息的容器。
    auto *out=reinterpret_cast<QVector<WinShareEntry>*>(lParam);
    if(!out) return TRUE;
    //过滤条件 1：跳过不可见窗口（IsWindowVisible(hwnd)返回 false）或最小化窗口（IsIconic(hwnd)返回 true），继续枚举。
    if(!IsWindowVisible(hwnd)||IsIconic(hwnd)) return TRUE;
    //获取窗口的扩展样式（GWL_EXSTYLE），判断是否包含WS_EX_TOOLWINDOW（工具窗口样式，通常是小的辅助窗口，如工具栏、弹窗等）。
    const LONG exStyle=GetWindowLong(hwnd,GWL_EXSTYLE);
    if(exStyle & WS_EX_TOOLWINDOW) return TRUE;

    wchar_t titleBuf[512]={0};
    const int len=GetWindowTextW(hwnd,titleBuf,511);    //GetWindowTextW获取窗口的宽字符标题，最多读取 511 个字符（留 1 个位置给结束符）
    if(len<=0) return TRUE;
    //宽字符标题转为QSTring，trimmed()：去除标题前后的空格（避免空标题或纯空格标题）
    const QString title=QString::fromWCharArray(titleBuf,len).trimmed();
    if(title.isEmpty()) return TRUE;

    WinShareEntry e;
    e.hwnd=static_cast<quint64>(reinterpret_cast<quintptr>(hwnd));//hwnd 是指针类型，先转换为 quintptr（Qt 的无符号整型指针类型），再转换为 quint64（64 位无符号整数），方便存储；
    e.title=title;
    out->push_back(e);
    return TRUE;
}
}
#endif

// 主窗口构造：初始化 UI、信令、宫格、登录和基础状态
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , timer(new QTimer(this))
{
    ui->setupUi(this);
    loadAppConfig();
    loadLoginPrefs();
    qInfo() << "[Build] SmartMeet" << __DATE__ << __TIME__;

    setWindowTitle("SmartMeet视频会议系统");
    //音视频设备检测与下拉框填充
    for (int i = 0; i < 5; ++i) {
        cv::VideoCapture temp(i);
        if (temp.isOpened()) {
            ui->cameraDevicecomboBox->addItem("摄像头" + QString::number(i), i);
            temp.release();
        }
    }
    for (const auto &device : QMediaDevices::audioInputs()) {
        ui->audioDevicecomboBox->addItem(device.description());
    }

    recorder = new AvRecorder(this);
    connect(recorder, &AvRecorder::videoPacketReady, this, [](const QByteArray &pkt, quint32 pts){
        qDebug() << "[Recorder] 视频包" << pkt.size() << "pts" << pts;
    });
    connect(recorder, &AvRecorder::audioPacketReady, this, [](const QByteArray &pkt, quint32 pts){
        qDebug() << "[Recorder] 音频包" << pkt.size() << "pts" << pts;
    });
    //初始化UI
    setupSignalUi();
    setupBottomMenus();
    setupRemoteGridUi();
    refreshRemoteTiles();
    setupMeetingStatsUi();
    setupLoginUi();
    appendRoomEvent(QString("部署配置已加载：%1 | 信令=%2 | RTMP=%3")
                        .arg(activeDeployProfile, signalUrl, rtmpPublishBaseUrl));
    //信令自动重连定时器
    signalReconnectTimer = new QTimer(this);
    signalReconnectTimer->setSingleShot(true);  //单次触发定时器（触发后自动停止）
    connect(signalReconnectTimer, &QTimer::timeout, this, [this]() {
        if (shuttingDown || meetingStopped || manualSignalDisconnect) return;// 各种状态判断：如果正在关闭/会议已停止/手动断开，直接返回
        if (signalConnected) return;    //已经连接上了，无需重连
        if (!ensureSignalCredential()) return;  // 确保信令认证信息有效
        if (!ensureRoomIdentity(false)) return;  // 确保房间信息有效
        openSignalConnection(); // 尝试重新建立信令连接
    });
    setupAdaptiveNetworkControl();  // 初始化自适应网络控制（根据网络调整码率/帧率）
    // 显示登录遮罩层，提示用户输入账号密码
    showLoginOverlay(true, "请输入账号、密码和房间号后登录");
}

MainWindow::~MainWindow()
{
    qDebug()<<"[MainWindow] 析构";
    shuttingDown = true;    //标志位

    on_stopMeetingButton_clicked();
    if(signalSocket){
        disconnect(signalSocket, nullptr, this, nullptr);
        signalSocket->close();
        delete signalSocket;
        signalSocket=nullptr;
    }
    // 移除所有尚未处理的、发送给当前窗口的事件，Qt 事件队列中可能还有未处理的事件，析构后处理会崩溃，这里主动清理
    QCoreApplication::removePostedEvents(this);
    //安全释放UI资源
    auto tmp=ui;
    ui=nullptr;
    delete tmp;
}

//窗口关闭事件：统一触发停会收尾
void MainWindow::closeEvent(QCloseEvent *event)
{
    if (shuttingDown) {
        event->accept();
        return;
    }
    shuttingDown = true;
    on_stopMeetingButton_clicked();
    QMainWindow::closeEvent(event); // 4. 调用父类（QMainWindow）的默认关闭事件处理，完成窗口关闭
}

// 窗口尺寸变化事件：同步远端容器和登录遮罩布局
void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    applyResponsiveLayout();
    syncRemoteContainerGeometry();
    syncLoginOverlayGeometry();
}

QString MainWindow::resolveAppConfigPath() const
{
    const QString envPath = qEnvironmentVariable("SMARTMEET_CONFIG").trimmed();
    if (!envPath.isEmpty() && QFileInfo::exists(envPath)) {
        return QDir::fromNativeSeparators(QFileInfo(envPath).absoluteFilePath());
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        QDir(appDir).filePath("smartmeet.ini"),
        QDir::current().filePath("smartmeet.ini"),
        QDir(QDir(appDir).absoluteFilePath("..")).filePath("smartmeet.ini"),
        QDir(QDir(appDir).absoluteFilePath("../..")).filePath("smartmeet.ini")
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            return QDir::fromNativeSeparators(QFileInfo(candidate).absoluteFilePath());
        }
    }
    return QDir::fromNativeSeparators(QDir(appDir).filePath("smartmeet.ini"));
}

void MainWindow::loadAppConfig()
{
    appConfigPath = resolveAppConfigPath();
    if (!QFileInfo::exists(appConfigPath)) {
        qWarning() << "[Config] smartmeet.ini not found, using built-in defaults:" << appConfigPath;
        signalUrl = signalUrl.trimmed();
        rtmpPublishBaseUrl = normalizedUrlBase(rtmpPublishBaseUrl);
        rtmpPlayBaseUrl = normalizedUrlBase(rtmpPlayBaseUrl);
        return;
    }

    QSettings settings(appConfigPath, QSettings::IniFormat);
    const QString envProfile = qEnvironmentVariable("SMARTMEET_PROFILE").trimmed();
    QString profile = envProfile.isEmpty()
                          ? settings.value("network/active_profile", activeDeployProfile).toString().trimmed()
                          : envProfile;
    if (profile.isEmpty()) {
        profile = activeDeployProfile;
    }

    const QString profilePrefix = QString("profile.%1/").arg(profile);
    const QString loadedSignal = settings.value(profilePrefix + "signal_url",
                                                settings.value("network/signal_url", signalUrl)).toString().trimmed();
    const QString loadedPublish = settings.value(profilePrefix + "rtmp_publish_base",
                                                 settings.value("network/rtmp_publish_base", rtmpPublishBaseUrl)).toString().trimmed();
    const QString loadedPlay = settings.value(profilePrefix + "rtmp_play_base",
                                              settings.value("network/rtmp_play_base", rtmpPlayBaseUrl)).toString().trimmed();

    if (!loadedSignal.isEmpty()) signalUrl = loadedSignal;
    if (!loadedPublish.isEmpty()) rtmpPublishBaseUrl = loadedPublish;
    if (!loadedPlay.isEmpty()) rtmpPlayBaseUrl = loadedPlay;
    activeDeployProfile = profile;

    signalUrl = signalUrl.trimmed();
    rtmpPublishBaseUrl = normalizedUrlBase(rtmpPublishBaseUrl);
    rtmpPlayBaseUrl = normalizedUrlBase(rtmpPlayBaseUrl);

    qInfo() << "[Config] loaded profile=" << activeDeployProfile
            << "path=" << appConfigPath
            << "signal=" << signalUrl
            << "rtmpPublish=" << rtmpPublishBaseUrl
            << "rtmpPlay=" << rtmpPlayBaseUrl;
}

QString MainWindow::buildRtmpPublishUrl(const QString &stream) const
{
    return buildStreamUrl(rtmpPublishBaseUrl, stream);
}

QString MainWindow::buildRtmpPlayUrl(const QString &stream) const
{
    return buildStreamUrl(rtmpPlayBaseUrl, stream);
}

bool MainWindow::isCompactMeetingLayout() const
{
    return width() < 1420;
}

bool MainWindow::isUltraCompactMeetingLayout() const
{
    return width() < 1240;
}

void MainWindow::updateMeetingHeaderUi()
{
    if (!ui) return;

    const bool compact = isCompactMeetingLayout();
    const bool connecting = signalSocket
                            && (signalSocket->state() == QAbstractSocket::ConnectingState
                                || signalSocket->state() == QAbstractSocket::HostLookupState);

    if (auto *meetingTitleLabel = findChild<QLabel*>("meetingTitleLabel")) {
        meetingTitleLabel->setVisible(!compact);
    }

    if (auto *meetingCodeLabel = findChild<QLabel*>("meetingCodeLabel")) {
        QString codeText = compact ? QStringLiteral("房: --") : QStringLiteral("房间: --");
        if (!roomId.isEmpty()) {
            codeText = compact
                           ? QStringLiteral("房: %1").arg(roomId)
                           : QStringLiteral("房间: %1").arg(roomId);
            if (!compact && !selfStream.isEmpty()) {
                codeText += QStringLiteral("  我: %1").arg(selfStream);
            }
        }
        meetingCodeLabel->setText(codeText);
        meetingCodeLabel->setToolTip(selfStream.isEmpty()
                                         ? codeText
                                         : QStringLiteral("房间: %1\n我的流: %2").arg(roomId, selfStream));
    }

    if (auto *signalBadgeLabel = findChild<QLabel*>("signalBadgeLabel")) {
        QString signalText;
        if (signalConnected) signalText = compact ? QStringLiteral("在线") : QStringLiteral("信令在线");
        else if (connecting) signalText = compact ? QStringLiteral("连接中") : QStringLiteral("信令连接中");
        else signalText = compact ? QStringLiteral("离线") : QStringLiteral("信令离线");
        signalBadgeLabel->setText(signalText);
        signalBadgeLabel->setToolTip(signalText);
    }

    if (auto *shareBadgeLabel = findChild<QLabel*>("shareBadgeLabel")) {
        QString shareText;
        if (localScreenShareOn) {
            shareText = compact
                            ? QStringLiteral("共享中")
                            : QStringLiteral("共享:%1").arg(shareSourceName.isEmpty() ? QStringLiteral("屏幕") : shareSourceName);
        } else {
            shareText = compact ? QStringLiteral("未共享") : QStringLiteral("共享:关");
        }
        shareBadgeLabel->setText(shareText);
        shareBadgeLabel->setToolTip(localScreenShareOn
                                        ? QStringLiteral("当前共享源: %1").arg(shareSourceName.isEmpty() ? QStringLiteral("屏幕") : shareSourceName)
                                        : QStringLiteral("当前未开启屏幕共享"));
    }
}

void MainWindow::updatePrimaryActionTexts()
{
    if (!ui) return;

    const bool compact = isCompactMeetingLayout();
    const bool connecting = signalSocket
                            && (signalSocket->state() == QAbstractSocket::ConnectingState
                                || signalSocket->state() == QAbstractSocket::HostLookupState);

    if (ui->startReceiveButton) {
        QString text;
        if (signalConnected) text = compact ? QStringLiteral("断开") : QStringLiteral("断开信令");
        else if (connecting) text = compact ? QStringLiteral("连接中") : QStringLiteral("连接中...");
        else text = compact ? QStringLiteral("连接") : QStringLiteral("连接信令");
        ui->startReceiveButton->setText(text);
        ui->startReceiveButton->setToolTip(signalConnected
                                               ? QStringLiteral("断开信令连接")
                                               : QStringLiteral("连接信令服务器"));
    }
    if (ui->logoutButton) {
        ui->logoutButton->setText(compact ? QStringLiteral("退出") : QStringLiteral("退出登录"));
        ui->logoutButton->setToolTip(QStringLiteral("退出当前登录账号"));
    }
    if (ui->startMeetingButton) {
        ui->startMeetingButton->setText(compact ? QStringLiteral("开始") : QStringLiteral("开始会议"));
        ui->startMeetingButton->setToolTip(QStringLiteral("启动采集、编码与推流"));
    }
    if (ui->stopMeetingButton) {
        ui->stopMeetingButton->setText(compact ? QStringLiteral("结束") : QStringLiteral("结束会议"));
        ui->stopMeetingButton->setToolTip(QStringLiteral("结束会议并释放音视频资源"));
    }
    if (ui->captureImageButton) {
        ui->captureImageButton->setToolTip(QStringLiteral("保存当前焦点画面或本地画面"));
    }
    if (ui->startRecordButton) {
        ui->startRecordButton->setText(compact ? QStringLiteral("录制") : QStringLiteral("开始AV录制"));
        ui->startRecordButton->setToolTip(QStringLiteral("开始录制当前会议音视频"));
    }
    if (ui->stopRecordButton) {
        ui->stopRecordButton->setText(compact ? QStringLiteral("停录") : QStringLiteral("停止AV录制"));
        ui->stopRecordButton->setToolTip(QStringLiteral("停止录制并保存文件"));
    }
}

void MainWindow::updateRoomPanelToggleText()
{
    if (auto *roomPanelToggleButton = findChild<QToolButton*>("roomPanelToggleButton")) {
        const bool visible = roomDock && roomDock->isVisible();
        const bool compact = isCompactMeetingLayout();
        roomPanelToggleButton->setText(compact
                                           ? QStringLiteral("侧栏")
                                           : (visible ? QStringLiteral("收起侧栏") : QStringLiteral("展开侧栏")));
        roomPanelToggleButton->setToolTip(visible
                                              ? QStringLiteral("收起右侧成员/聊天/白板侧栏")
                                              : QStringLiteral("展开右侧成员/聊天/白板侧栏"));
    }
}

void MainWindow::updateRoomDockPresentation()
{
    if (!roomDock) return;

    auto *tabs = findChild<QTabWidget*>("roomTabWidget");
    const bool compact = isCompactMeetingLayout();
    const bool ultraCompact = isUltraCompactMeetingLayout();

    int minDock = ultraCompact ? 248 : (compact ? 270 : 300);
    int maxDock = ultraCompact ? 320 : (compact ? 350 : 380);

    if (tabs) {
        QWidget *current = tabs->currentWidget();
        const QString name = current ? current->objectName() : QString();
        if (name == QStringLiteral("chatTab")) {
            minDock = ultraCompact ? 268 : (compact ? 290 : 320);
            maxDock = ultraCompact ? 340 : (compact ? 370 : 400);
            roomDock->setWindowTitle(QStringLiteral("聊天与消息"));
        } else if (name == QStringLiteral("whiteboardTab")) {
            minDock = ultraCompact ? 300 : (compact ? 330 : 360);
            maxDock = ultraCompact ? 380 : (compact ? 410 : 440);
            roomDock->setWindowTitle(QStringLiteral("协作白板"));
        } else if (name == QStringLiteral("eventTab")) {
            minDock = ultraCompact ? 252 : (compact ? 274 : 300);
            maxDock = ultraCompact ? 326 : (compact ? 352 : 380);
            roomDock->setWindowTitle(QStringLiteral("事件与成员"));
        } else {
            roomDock->setWindowTitle(QStringLiteral("房间成员"));
        }
    }

    roomDock->setMinimumWidth(minDock);
    roomDock->setMaximumWidth(maxDock);
    if (roomDock->isVisible() && !roomDock->isFloating()) {
        const int targetWidth = qBound(minDock, roomDock->width(), maxDock);
        roomDock->resize(targetWidth, roomDock->height());
        resizeDocks({roomDock}, {targetWidth}, Qt::Horizontal);
    }
}

void MainWindow::applyResponsiveLayout()
{
    if (!ui) return;

    const bool compact = isCompactMeetingLayout();
    const bool ultraCompact = isUltraCompactMeetingLayout();

    updateRoomDockPresentation();

    if (QWidget *localSide = findChild<QWidget*>("localSideFrame")) {
        localSide->setMinimumWidth(compact ? 148 : 172);
        localSide->setMaximumWidth(ultraCompact ? 210 : 320);
    }
    if (QWidget *localHint = findChild<QWidget*>("localHintFrame")) {
        localHint->setVisible(!compact);
    }

    if (QWidget *deviceFrame = findChild<QWidget*>("deviceEdgeFrame")) {
        deviceFrame->setMinimumWidth(compact ? 150 : 188);
        deviceFrame->setMaximumWidth(compact ? 210 : 260);
    }
    if (QWidget *deviceTitle = findChild<QWidget*>("deviceEdgeTitle")) {
        deviceTitle->setVisible(!compact);
    }
    if (QWidget *ctrlSep1 = findChild<QWidget*>("ctrlSep1")) {
        ctrlSep1->setVisible(!ultraCompact);
    }
    if (QWidget *ctrlSep2 = findChild<QWidget*>("ctrlSep2")) {
        ctrlSep2->setVisible(!ultraCompact);
    }

    if (QWidget *beautyStrengthFrame = findChild<QWidget*>("beautyStrengthFrame")) {
        beautyStrengthFrame->setVisible(!ultraCompact);
    }

    updateMeetingHeaderUi();
    updatePrimaryActionTexts();
    updateRoomPanelToggleText();
    updateMeetingStatsUi();
}

// 开始会议主流程：启动采集、编码、推流并同步状态
void MainWindow::on_startMeetingButton_clicked()
{
    localScreenShareOn=false;
    meetingStopped = false;
    audioStopped = false;
    localAudioOn = true;
    localVideoOn = true;
    // 设为未初始化，保证本次会议首次应用策略时也会输出档位日志
    adaptiveProfileLevel = -1;
    adaptiveStressScore = 0;
    lastAdaptiveIssueMs = 0;
    lastAdaptiveLogMs = 0;
    totalSignalReconnectCount = 0;
    totalPullRetryCount = 0;
    recentEventLogs.clear();
    updateMeetingStatsUi();
    refreshSelfControlActions();

    if (videoWorker || videoThread) {
        qInfo() << "[Mainwindow] 会议已启动（重复启动忽略）";
        return;
    }

    if (!ensureRoomIdentity(false)) {
        qWarning() << "[Room] 房间身份初始化失败";
        return;
    }
    appendRoomEvent(QString("房间: %1 用户流: %2").arg(roomId, selfStream));
    currentPushUrl = buildRtmpPublishUrl(selfStream);

    if (!videoWorker) {
        videoWorker = new VideoCapture;
        videoThread = new QThread(this);
        videoThread->setObjectName("videoThread");
        videoWorker->moveToThread(videoThread);

        connect(videoThread, &QThread::started, videoWorker, &VideoCapture::captureLoop);
        connect(videoThread, &QThread::finished, videoWorker, &QObject::deleteLater, Qt::UniqueConnection);
        //QPointer 是 Qt 提供的智能弱指针，专门用于管理 QObject 子类对象，当指向的对象被销毁时，QPointer 会自动置空，避免野指针访问。
        QPointer<QLabel> localLabel = ui->localVideolabel;
        QPointer<MainWindow> self(this);    //self 指向当前 MainWindow 实例，用 QPointer 包裹是为了在 lambda 表达式中安全访问。
        disconnect(videoWorker, &VideoCapture::frameCaptured, this, nullptr);
        connect(videoWorker, &VideoCapture::frameCaptured, this, [self,localLabel](const QImage &img){
            if(!self) return;
            if(self->meetingStopped) return;
            if(!localLabel) return;
            localLabel->setPixmap(QPixmap::fromImage(img).scaled(
                localLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
            if (self->camRecording && self->recorder && self->recorder->isOpen()) {
                self->recorder->pushVideoFrame(img);
            }
        }, Qt::QueuedConnection);

        int camIndex = 0;
        if (ui && ui->cameraDevicecomboBox) {
            camIndex = ui->cameraDevicecomboBox->currentData().toInt();
        }
        if (!videoWorker->open(camIndex)) {
            QMessageBox::warning(this, "错误", "无法打开摄像头");
            videoWorker->deleteLater();
            videoWorker = nullptr;
            videoThread->deleteLater();
            videoThread = nullptr;
            return;
        }

        videoThread->start(QThread::HighPriority);  //高优先级启动线程
        applyLocalVideoSource();
        applyBeautyToWorker();
    }
    //编码/推流线程初始化，均以高优先级启动
    if (!encThread) {
        encThread = new QThread(this);
        encThread->setObjectName("encThread");
        encThread->start(QThread::HighPriority);
    }
    if (!pushThread) {
        pushThread = new QThread(this);
        pushThread->setObjectName("pushThread");
        pushThread->start(QThread::HighPriority);
    }
    if (!audioEncThread) {
        audioEncThread = new QThread(this);
        audioEncThread->setObjectName("audioEncThread");
        audioEncThread->start(QThread::HighPriority);
    }
    //编码/推流对象初始化
    if (!pusher) {//RTMP推流器
        pusher = new RtmpPusher(nullptr);
        pusher->moveToThread(pushThread);
        connect(pushThread, &QThread::finished, pusher, &QObject::deleteLater, Qt::UniqueConnection);
        connect(pusher, &RtmpPusher::writeError, this, [this](const QString &err, bool videoPacket) {
            const int w = videoPacket ? 2 : 1;
            handleNetworkIssue(QString("推流写包异常: %1").arg(err), w);
        });
    }
    //视频编码器
    if (!netEnc) {
        netEnc = new AvNetEncoder(nullptr);
        netEnc->moveToThread(encThread);
        connect(encThread, &QThread::finished, netEnc, &QObject::deleteLater, Qt::UniqueConnection);
    }
    if (!audioEnc) {
        audioEnc = new AvAudioEncoder(nullptr);
        audioEnc->moveToThread(audioEncThread);
        connect(audioEncThread, &QThread::finished, audioEnc, &QObject::deleteLater, Qt::UniqueConnection);
    }

    if (!audioWorker) startAudioCapture();

    // 启动会议时先应用默认档位（高清）
    //videoQueueDepthToken用于追踪视频队列的深度（可以理解为待处理的视频数据量），原子类型atomic_int保证多线程环境下读写不会出现数据竞争，是音视频开发中处理并发的常用方式。
    applyAdaptiveProfile(0, false);//调用自适应配置应用函数，参数0代表默认的高清档位，false通常表示不执行额外的特殊逻辑（比如强制重启、立即生效等），核心作用是先把会议的视频参数初始化为高清配置。
    videoQueueDepthToken = std::make_shared<std::atomic_int>(0);// 视频队列深度（原子变量）
    lastVideoDropProtectLogMs = 0;//初始化视频丢包保护日志的最后记录时间（单位毫秒），用于控制日志打印频率，避免频繁输出日志占用资源。

    int targetW = 1280;
    int targetH = 720;
    int targetFps = 30;
    int targetBitrate = 2200000;
    if (adaptiveProfileLevel == 1) {
        targetW = 960;
        targetH = 540;
        targetFps = 24;
        targetBitrate = 1400000;
    } else if (adaptiveProfileLevel >= 2) {
        targetW = 640;
        targetH = 360;
        targetFps = 20;
        targetBitrate = 900000;
    }
    // 打开音频编码器（44100采样率，单声道）
    bool aok = false;
    QMetaObject::invokeMethod(audioEnc, [&]() {
        aok = audioEnc->open(44100, 1);
    }, Qt::BlockingQueuedConnection);//阻塞式排队调用—— 当前线程会等待audioEnc所在线程执行完open方法后，才继续执行后续代码，确保结果能正确返回；
    if (!aok) {
        qWarning() << "[Mainwindow] 音频编码器打开失败";
    }
    // 打开视频编码器
    bool encOk = false;
    QMetaObject::invokeMethod(netEnc, [&]() {
        encOk = netEnc->openVideo(targetW, targetH, targetFps, targetBitrate);
    }, Qt::BlockingQueuedConnection);
    if (!encOk) {
        qWarning() << "[Mainwindow] 视频编码器打开失败";
        return;
    }
    //编码/推流链路连接
    disconnect(netEnc, &AvNetEncoder::videoPacketReady, pusher, nullptr);
    connect(netEnc, &AvNetEncoder::videoPacketReady, pusher, &RtmpPusher::pushEncodeVideo, Qt::QueuedConnection);
    disconnect(audioEnc, &AvAudioEncoder::audioPacketReady, pusher, nullptr);
    connect(audioEnc, &AvAudioEncoder::audioPacketReady, pusher, &RtmpPusher::pushEncodeAudio, Qt::QueuedConnection);

    if (videoSendConn) disconnect(videoSendConn);
    videoSendConn = connect(videoWorker, &VideoCapture::frameCaptured, this, [this](const QImage &img) {
        //前置校验
        if (!netEnc) return;    //编码实例不存在，直接返回
        auto queueDepthToken = videoQueueDepthToken;
        if (!queueDepthToken) return;   //队列深度计数器不存在，直接返回
        //本地视频开关处理（黑屏替换）
        QImage out = img;
        if (!localVideoOn) {//localVideoOn 控制是否开启本地视频：关闭时不发送真实画面，改用同尺寸 / 格式的黑屏帧；
            static QImage blackFrame;
            if (blackFrame.size() != img.size() || blackFrame.format() != QImage::Format_RGB888) {
                blackFrame = QImage(img.size(), QImage::Format_RGB888);
                blackFrame.fill(Qt::black);
            }
            out = blackFrame;
        }
        // 背压保护：编码跟不上时只保留极少在途帧，避免共享后切回摄像头出现长时间慢动作。
        if (queueDepthToken->load(std::memory_order_relaxed) >= 2) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - lastVideoDropProtectLogMs > 1500) {
                appendRoomEvent("视频编码繁忙：已启用丢帧保护");
                lastVideoDropProtectLogMs = now;
            }
            return;
        }
        //异步提交编码任务，队列深度+1（原子操作，线程安全）
        queueDepthToken->fetch_add(1, std::memory_order_relaxed);   //队列计数+1
        QPointer<AvNetEncoder> encPtr(netEnc);  //用QPointer包装编码实例，防止编码实例被销毁后野指针访问
        const bool queued = QMetaObject::invokeMethod(//异步调用编码实例的pushVideoFrame方法
            netEnc,
            [encPtr, queueDepthToken, out]() {
                if (encPtr) {   //检查编码实例是否还存活
                    encPtr->pushVideoFrame(out);    //提交帧到编码器
                }
                queueDepthToken->fetch_sub(1, std::memory_order_relaxed);//无论编码是否成功，计数-1（释放队列位置）
            },
            Qt::QueuedConnection);//异步执行，不阻塞当前线程
        if (!queued) {  //如果任务提交失败，立即将计数-1，避免计数错误
            queueDepthToken->fetch_sub(1, std::memory_order_relaxed);
        }
    }, Qt::QueuedConnection);
    //音频帧推送
    if (audioWorker) {
        if (audioSendConn) disconnect(audioSendConn);
        audioSendConn = connect(audioWorker, &AudioCapture::audioFrameReady, this, [this](const QByteArray &pcm) {
            if (!audioEnc) return;
            QByteArray toSend = pcm;    //复制原始PCM音频数据到待发送变量
            if (!localAudioOn) {    //如果本地音频被关闭（静音），将音频数据填充为全0（静音）
                toSend.fill('\0');
            }
            //异步调用音频编码器的pushPcm方法，将PCM数据推入编码器
            //Q_ARG(QByteArray, toSend)：传递给 pushPcm 方法的参数，指定参数类型为 QByteArray，参数值为 toSend（即把 toSend 这个音频 PCM 数据传给 pushPcm 方法）。
            QMetaObject::invokeMethod(audioEnc, "pushPcm", Qt::QueuedConnection, Q_ARG(QByteArray, toSend));
        }, Qt::QueuedConnection);
    }
    //启动RTMP推流
    bool rtmpOk = false;
    QMetaObject::invokeMethod(pusher, [&]() {
        pusher->setVideoParams(targetW, targetH, targetFps);
        pusher->setAudioParams(44100, 1);
        rtmpOk = pusher->start(currentPushUrl, targetFps, 44100);//启动RTMP推流，结果赋值给rtmpOk
    },Qt::BlockingQueuedConnection);

    if (!rtmpOk) {
        qWarning() << "[Mainwindow] RTMP推流启动失败";
        isPublishing = false;
    } else {
        qDebug() << "[Mainwindow] RTMP推流已启动";
        isPublishing = true;
    }
    updateMeetingStatsUi();

    appendRoomEvent(QString("开始会议，推流: %1").arg(currentPushUrl));
    if (signalConnected) {
        sendSignalUpdate();
    }
}

// 结束会议主流程：停止拉流/推流并按顺序回收线程与资源
void MainWindow::on_stopMeetingButton_clicked()
{
    if (stopMeetingInProgress) {
        qDebug() << "[Mainwindow] 会议停止流程执行中（重复调用忽略）";
        return;
    }
    stopMeetingInProgress = true;

    if (meetingStopped) {
        qDebug() << "[Mainwindow] 会议已停止（重复调用忽略）";
        stopMeetingInProgress = false;
        return;
    }
    meetingStopped = true;
    manualSignalDisconnect = true;
    resetSignalReconnectState();
    isPublishing = false;
    currentPushUrl.clear();
    adaptiveStressScore = 0;
    localScreenShareOn=false;
    qInfo() << "[Mainwindow] 开始停止会议";

    if (signalConnected) {
        sendSignalUpdate();
    }

    stopCurrentPull();//停止拉流
    qInfo() << "[Mainwindow] stop pull done";

    if (videoSendConn) disconnect(videoSendConn);
    videoQueueDepthToken = std::make_shared<std::atomic_int>(0);
    if (audioSendConn) disconnect(audioSendConn);
    if (netEnc && pusher) disconnect(netEnc, &AvNetEncoder::videoPacketReady, pusher, nullptr);
    if (audioEnc && pusher) disconnect(audioEnc, &AvAudioEncoder::audioPacketReady, pusher, nullptr);

    if (receiver) disconnect(receiver, nullptr, this, nullptr);

    stopAudioCapture();
    qInfo() << "[Mainwindow] stop audio done";
    //销毁拉流/推流/编码资源
    if (receiver) {
        disconnect(receiver, nullptr, this, nullptr);
        receiver->stop();
        delete receiver;
        receiver = nullptr;
    }

    if (sender) {
        sender->stop();
        delete sender;
        sender = nullptr;
    }

    if (pusher) {
        if (pushThread && pushThread->isRunning()) {
            QMetaObject::invokeMethod(pusher, [&]() {
                pusher->stop();
            }, Qt::BlockingQueuedConnection);
        } else {
            pusher->stop();
        }
    }

    stopThreadAndDelete(pushThread, "pushThread", 3000);
    qInfo() << "[Mainwindow] stop pushThread done";
    pusher = nullptr;

    if (audioEnc) {
        if (audioEncThread && audioEncThread->isRunning()) {
            QMetaObject::invokeMethod(audioEnc, [&]() {
                audioEnc->close();
            }, Qt::BlockingQueuedConnection);
        } else {
            audioEnc->close();
        }
    }
    stopThreadAndDelete(audioEncThread, "audioEncThread", 3000);
    qInfo() << "[Mainwindow] stop audioEncThread done";
    audioEnc = nullptr;

    if (netEnc) {
        if (encThread && encThread->isRunning()) {
            QMetaObject::invokeMethod(netEnc, [&]() {
                netEnc->close();
            }, Qt::BlockingQueuedConnection);
        } else {
            netEnc->close();
        }
    }
    stopThreadAndDelete(encThread, "encThread", 3000);
    qInfo() << "[Mainwindow] stop encThread done";
    netEnc = nullptr;

    if (videoWorker) {
        videoWorker->stop();
    }
    stopThreadAndDelete(videoThread, "videoThread", 3000);
    qInfo() << "[Mainwindow] stop videoThread done";
    videoWorker = nullptr;
    //断开信令连接
    if (signalSocket) {
        disconnect(signalSocket, nullptr, this, nullptr);
        sendSignalLeave();
        signalSocket->close();
        appendRoomEvent("已断开信令");
    }
    signalConnected = false;
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
    //重置会议状态
    memberStates.clear();
    roomHostStream.clear();
    preferredRemoteStream.clear();
    currentRemoteStream.clear();
    focusedStream.clear();
    focusMode = false;
    if (focusPreviewFullScreen) {
        focusPreviewFullScreen = false;
        showNormal();
        if (roomDock) roomDock->show();
        if (auto *topBar = findChild<QWidget*>("topBarFrame")) topBar->show();
        if (auto *bottomBar = findChild<QWidget*>("bottomControlFrame")) bottomBar->show();
        if (auto *localSide = findChild<QWidget*>("localSideFrame")) localSide->show();
        if (menuBar()) menuBar()->show();
        if (statusBar()) statusBar()->show();
        syncRemoteContainerGeometry();
    }
    refreshRoomUserList();
    clearAllTileFrames();
    latestRemoteFrames.clear();
    if (remoteStack && remoteGridPage) {
        remoteStack->setCurrentWidget(remoteGridPage);
    }
    if (ui && ui->remoteVideolabel) {
        ui->remoteVideolabel->clear();
    }
    refreshSelfControlActions();
    updatePrimaryActionTexts();
    updateRoomPanelToggleText();
    updateMeetingStatsUi();

    qDebug() << "[Mainwindow] 会议已结束";
    stopMeetingInProgress = false;
}

// 拍照：优先保存焦点远端画面，其次保存本地画面
void MainWindow::on_captureImageButton_clicked()
{
    const QString filename = QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss") + ".jpg";

    QImage remoteImg;
    if (captureFocusedRemoteImage(&remoteImg)) {
        if (remoteImg.save(filename, "JPG")) {
            QMessageBox::information(this, "提示", "已保存照片：" + filename);
        } else {
            QMessageBox::warning(this, "错误", "保存照片失败：" + filename);
        }
        return;
    }

    if (videoWorker) {
        videoWorker->capturePhoto(filename);
        QMessageBox::information(this, "提示", "已保存照片：" + filename);
        return;
    }

    QMessageBox::warning(this, "提示", "当前没有可用画面可拍照");
}



// 启动音频采集线程并连接录制/发送链路
void MainWindow::startAudioCapture()
{
    if (!audioWorker) {
        audioWorker = new AudioCapture;
        audioThread = new QThread(this);
        audioThread->setObjectName("audioThread");
        audioWorker->moveToThread(audioThread);
        connect(audioThread, &QThread::finished, audioWorker, &QObject::deleteLater, Qt::UniqueConnection);

        connect(audioThread, &QThread::started, audioWorker, &AudioCapture::captureLoop);
        connect(audioWorker, &AudioCapture::logMessage, this, [](const QString &msg){
            qDebug() << "AudioLog:" << msg;
        });

        const QString selectedMic = (ui && ui->audioDevicecomboBox) ? ui->audioDevicecomboBox->currentText() : QString();
        QString device = "audio=" + selectedMic;
        qDebug() << "FFmpeg 采集设备名:" << device;

        // 采集端参数固定: 44100 Hz / 单声道 / Int16 (仅采集，不本地播放)
        QAudioFormat fmt;
        fmt.setSampleRate(44100);
        fmt.setChannelCount(1);
        fmt.setSampleFormat(QAudioFormat::Int16);

        if (!audioWorker->startCapture(device, false, false, fmt)) {
            QMessageBox::warning(this, "错误", "音频采集启动失败");
            return;
        }

        audioThread->start(QThread::HighPriority);
    }

    if (recordConn) disconnect(recordConn);
    QPointer<MainWindow> self(this);
    recordConn = connect(audioWorker, &AudioCapture::audioFrameReady, this,
            [self](const QByteArray &data){
                if(!self) return;
                if(self->meetingStopped||self->audioStopped) return;

                if(self->recorder&&self->recorder->isOpen()){
                    // 计算样本数
                    int nb_samples=data.size()/2;//S16 mono
                    self->recorder->pushAudioPCM(reinterpret_cast<const uint8_t*>(data.constData()),nb_samples);
                }

            },
            Qt::QueuedConnection
            );
}

// 停止音频采集并清理音频线程资源
void MainWindow::stopAudioCapture()
{

    // 如果已经停过一次，直接返回，避免重复释放
    if(audioStopped){
        qDebug()<<"[Mainwindow] 音频采集已停止(重复调用忽略)";
        return;
    }
    audioStopped=true;
    if (audioWorker) {
        audioWorker->stop();
        disconnect(audioWorker, nullptr, this, nullptr); // 断开所有信号
        recordConn = QMetaObject::Connection();
        audioSendConn = QMetaObject::Connection();
    }
    stopThreadAndDelete(audioThread, "audioThread", 5000);
    audioWorker = nullptr;

    qDebug()<<"[Mainwindow] 音频采集已停止";
}

// 应用本地视频源（摄像头/屏幕共享）与目标帧率
void MainWindow::applyLocalVideoSource()
{
    if(!videoWorker||!videoThread||!videoThread->isRunning()) return;
    const int mode=localScreenShareOn?1:0;
    int baseFps = 30;
    if (adaptiveProfileLevel == 1) baseFps = 24;
    else if (adaptiveProfileLevel >= 2) baseFps = 20;
    // 共享模式下适当降帧，减小 CPU 压力并降低编码堆积风险。
    const int targetFps = localScreenShareOn ? qMin(baseFps, 24) : baseFps;

    if(localScreenShareOn){
        videoWorker->setShareTarget(shareScreenIndex,shareWindowId);
    }

    // 切换源时重置背压计数，避免共享阶段遗留状态影响切回摄像头后的发送节奏。
    videoQueueDepthToken = std::make_shared<std::atomic_int>(0);

    //不能走QueuedConnection:captureLoop常驻会导致槽不执行
    videoWorker->setTargetFps(targetFps);
    videoWorker->setCaptureMode(mode);
    qInfo()<<"[Share] local source="<<(mode==1?shareSourceName:"camera")<<"fps="<<targetFps;
}

// 将美颜参数同步到视频采集 worker
void MainWindow::applyBeautyToWorker()
{
    if(!videoWorker) return;

    const int level=qBound(0,localBeautyLevel,100);
    const int style=localBeautyStyle;
    // VideoCapture::captureLoop 常驻，无事件循环，QueuedConnection 可能不生效。
    // 这些 setter 内部都有 mutex，直接调用即可线程安全。
    videoWorker->setBeautyStyle(style);
    videoWorker->setBeautyLevel(level);
    videoWorker->setBeautyEnabled(level>0&&style>0);
}

// 设置美颜模式与强度并刷新 UI 与日志
void MainWindow::setBeautyMode(const QString &modeName, int level)
{
    localBeautyMode=modeName;
    int sliderLevel=localBeautyLevel;
    if(beautyStrengthSlider){
        sliderLevel=qBound(0,beautyStrengthSlider->value(),100);
    }
    int finalLevel=(level>=0)?qBound(0,level,100):sliderLevel;

    if(modeName=="关闭"){
        localBeautyStyle=0;
        localBeautyLevel=0;
    }else{
        if(modeName=="自然") localBeautyStyle=1;
        else if(modeName=="清晰") localBeautyStyle=2;
        else if(modeName=="柔和") localBeautyStyle=3;
        else if(modeName=="磨皮") localBeautyStyle=4;
        else if(modeName=="瘦脸") localBeautyStyle=5;
        else if(modeName=="祛皱") localBeautyStyle=6;
        else localBeautyStyle=1;

        localBeautyLevel=qBound(1,finalLevel,100);
        if(beautyStrengthSlider && beautyStrengthSlider->value()!=localBeautyLevel){
            beautyStrengthSlider->setValue(localBeautyLevel);
        }
    }
    if(beautyStrengthValueLabel){
        beautyStrengthValueLabel->setText(QString::number(localBeautyLevel));
    }
    applyBeautyToWorker();
    if(localBeautyStyle<=0){
        appendRoomEvent("美颜已关闭");
    }else{
        appendRoomEvent(QString("美颜模式：%1（强度%2）").arg(localBeautyMode).arg(localBeautyLevel));
    }
}

// 初始化白板工具栏、权限按钮与交互绑定
void MainWindow::setupWhiteboardUi()
{
    if(whiteboardCanvasLabel){
        whiteboardCanvasLabel->installEventFilter(this);
        whiteboardCanvasLabel->setMouseTracking(true);
    }
    if(whiteboardColorCombo){
        if(whiteboardColorCombo->count()>=4){
            whiteboardColorCombo->setItemData(0,"#e03131",Qt::UserRole);//红
            whiteboardColorCombo->setItemData(1,"#1971c2",Qt::UserRole);//蓝
            whiteboardColorCombo->setItemData(2,"#2b8a3e",Qt::UserRole);//绿
            whiteboardColorCombo->setItemData(3,"#111111",Qt::UserRole);//黑
        }
    }
    if(whiteboardWidthSpin){
        whiteboardWidthSpin->setRange(1,12);
        if(whiteboardWidthSpin->value()<=0){
            whiteboardWidthSpin->setValue(3);
        }
    }
    if(whiteboardPenButton){
        disconnect(whiteboardPenButton,&QToolButton::toggled,this,nullptr);
        connect(whiteboardPenButton,&QToolButton::toggled,this,[this](bool on){
            whiteboardPenEnabled=on;
            appendRoomEvent(on?"白板画笔已开启":"白板画笔已关闭");
        });
    }
    if(whiteboardUndoButton){
        disconnect(whiteboardUndoButton,&QPushButton::clicked,this,nullptr);
        connect(whiteboardUndoButton,&QPushButton::clicked,this,[this](){
            if(!canWriteWhiteboard()){
                appendRoomEvent("白板已锁定，仅主持人/联席主持人可撤销");
                return;
            }
            undoLastLocalStroke(true);
        });
    }
    if(whiteboardClearButton){
        disconnect(whiteboardClearButton,&QPushButton::clicked,this,nullptr);
        connect(whiteboardClearButton,&QPushButton::clicked,this,[this](){
            if(!canClearWhiteboard()){
                appendRoomEvent("仅主持人/联席主持人可清空白板");
                return;
            }
            clearWhiteboard(true,selfStream);
        });
        whiteboardClearButton->setEnabled(canClearWhiteboard());
    }
    if(whiteboardLockButton){
        disconnect(whiteboardLockButton,&QToolButton::toggled,this,nullptr);
        connect(whiteboardLockButton,&QToolButton::toggled,this,[this](bool on){
            const bool canManage=canManageWhiteboard();
            if(!canManage){
                QSignalBlocker guard(whiteboardLockButton);
                whiteboardLockButton->setChecked(whiteboardLocked);
                appendRoomEvent("仅主持人/联席主持人可锁定白板");
                return;
            }
            whiteboardLocked=on;
            applyWhiteboardLockUi();
            sendSignalWhiteboardLock(on);
            appendRoomEvent(on ? "白板已锁定(仅主持人/联席主持人可写)":"白板已解锁(全员可写)");
        });
    }
    applyWhiteboardLockUi();
    ensureWhiteboardCanvas();
}

// 确保白板画布尺寸与容器匹配并完成重建
void MainWindow::ensureWhiteboardCanvas()
{
    if(!whiteboardCanvasLabel) return;
    QSize target=whiteboardCanvasLabel->size();
    if(target.width()<64||target.height()<64) target=QSize(960,540);

    if(whiteboardCanvas.size()==target&&!whiteboardCanvas.isNull()) return;

    QImage newCanvas(target,QImage::Format_ARGB32_Premultiplied);
    newCanvas.fill(Qt::white);

    if(!whiteboardCanvas.isNull()){
        QPainter p(&newCanvas);
        p.setRenderHint(QPainter::SmoothPixmapTransform,true);
        p.drawImage(QRect(QPoint(0,0),target),whiteboardCanvas);
    }
    whiteboardCanvas=newCanvas;
    updateWhiteboardCanvasLabel();
}

// 刷新白板画布显示
void MainWindow::updateWhiteboardCanvasLabel()
{
    if(!whiteboardCanvasLabel||whiteboardCanvas.isNull()) return;
    whiteboardCanvasLabel->setPixmap(QPixmap::fromImage(whiteboardCanvas));
}

// 将控件坐标映射到白板画布坐标
QPoint MainWindow::mapWhiteboardPoint(const QPoint &widgetPos) const
{
    if(!whiteboardCanvasLabel||whiteboardCanvas.isNull()) return QPoint();
    const int w=qMax(1,whiteboardCanvasLabel->width()-1);
    const int h=qMax(1,whiteboardCanvasLabel->height()-1);
    const int x=qBound(0,widgetPos.x(),w);
    const int y=qBound(0,widgetPos.y(),h);

    const int cx=(x*qMax(1,whiteboardCanvas.width()-1))/w;
    const int cy=(y*qMax(1,whiteboardCanvas.height()-1))/h;
    return QPoint(cx,cy);
}

// 读取当前白板颜色选择
QColor MainWindow::whiteboardSelectedColor() const
{
    if(whiteboardColorCombo){
        const QString hex=whiteboardColorCombo->currentData(Qt::UserRole).toString();
        if(!hex.isEmpty()) return QColor(hex);
    }
    return QColor("#e03131");
}

// 读取当前白板画笔粗细
int MainWindow::whiteboardSelectedWidth() const
{
    if(whiteboardWidthSpin) return qBound(1,whiteboardWidthSpin->value(),12);
    return 3;
}

// 判断当前用户是否有清空白板权限
bool MainWindow::canClearWhiteboard() const
{
    if(!signalConnected) return true;
    if(memberStates.isEmpty()) return true;
    return canManageWhiteboard();
}

// 根据缓存笔迹全量重绘白板
void MainWindow::redrawWhiteboardFromStrokes()
{
    ensureWhiteboardCanvas();
    if(whiteboardCanvas.isNull()) return;

    whiteboardCanvas.fill(Qt::white);
    QPainter p(&whiteboardCanvas);
    p.setRenderHint(QPainter::Antialiasing,true);

    for(const auto &stroke : WhiteboardStrokes){
        QPen pen(stroke.color,stroke.width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
        p.setPen(pen);
        for(const QLine &seg : stroke.segments){
            p.drawLine(seg);
        }
    }
    updateWhiteboardCanvasLabel();
}

// 按轨迹 ID 删除白板笔迹并刷新
void MainWindow::removeStrokeById(const QString &strokeId, const QString &byStream)
{
    if(strokeId.isEmpty()) return;

    QString owner;
    bool removed=false;
    for(int i=WhiteboardStrokes.size()-1;i>=0;--i){
        if(WhiteboardStrokes[i].strokeId==strokeId){
            owner=WhiteboardStrokes[i].ownerStream;
            WhiteboardStrokes.removeAt(i);
            removed=true;
            break;
        }
    }
    if(!removed) return;

    redrawWhiteboardFromStrokes();

    if(!byStream.isEmpty()){
        if(byStream==selfStream){
            appendRoomEvent("你撤销了一笔白板");
        }else{
            appendRoomEvent(QString("%1 撤销了一笔白板").arg(byStream));
        }
    }
}

// 撤销自己最近一笔白板轨迹
void MainWindow::undoLastLocalStroke(bool broadcast)
{
    for(int i=WhiteboardStrokes.size()-1;i>=0;--i){
        if(WhiteboardStrokes[i].ownerStream==selfStream){
            const QString sid=WhiteboardStrokes[i].strokeId;
            removeStrokeById(sid,selfStream);
            if(broadcast) sendSignalWhiteboardUndo(sid);
            return;
        }
    }
    appendRoomEvent("没有可撤销的本地笔迹");
}

// 绘制白板线段并按需广播信令
void MainWindow::drawWhiteboardLine(const QPoint &from,const QPoint &to,bool broadcast,
                                    const QColor &color,int width,const QString &strokeId,const QString &ownerStream)
{
    ensureWhiteboardCanvas();
    if(whiteboardCanvas.isNull()) return;

    const QString sid=strokeId.isEmpty()
        ? QString("st_%1_%2").arg(ownerStream.isEmpty()? selfStream : ownerStream)
                            .arg(QDateTime::currentMSecsSinceEpoch())
        :strokeId;
    const QString owner=ownerStream.isEmpty()?selfStream:ownerStream;
    const QColor penColor=color.isValid() ? color : QColor("#e03131");
    const int penWidth=qBound(1,width,12);

    int idx=-1;
    for(int i=WhiteboardStrokes.size()-1;i>=0;--i){
        if(WhiteboardStrokes[i].strokeId==sid){
            idx=i;
            break;
        }
    }
    if(idx<0){
        WhiteboardStroke stroke;
        stroke.strokeId=sid;
        stroke.ownerStream=owner;
        stroke.color=penColor;
        stroke.width=penWidth;
        WhiteboardStrokes.push_back(stroke);
        idx=WhiteboardStrokes.size()-1;
    }

    WhiteboardStrokes[idx].segments.push_back(QLine(from,to));

    QPainter p(&whiteboardCanvas);
    p.setRenderHint(QPainter::Antialiasing,true);
    QPen pen(WhiteboardStrokes[idx].color,WhiteboardStrokes[idx].width,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin);
    p.setPen(pen);
    p.drawLine(from,to);

    updateWhiteboardCanvasLabel();

    if(broadcast){
        sendSignalWhiteboardDraw(from,to,WhiteboardStrokes[idx].color,WhiteboardStrokes[idx].width,sid);
    }
}

// 清空白板并按需广播清空消息
void MainWindow::clearWhiteboard(bool broadcast, const QString &byStream)
{
    ensureWhiteboardCanvas();
    if(whiteboardCanvas.isNull()) return;

    WhiteboardStrokes.clear();
    whiteboardActionStrokeId.clear();

    whiteboardCanvas.fill(Qt::white);
    updateWhiteboardCanvasLabel();

    if(broadcast){
        sendSignalWhiteboardClear();
    }

    if(!byStream.isEmpty()){
        appendRoomEvent(byStream==selfStream?"你已清空白板"
                                            :QString("%1 已清空白板").arg(byStream));
    }
}

// 发送白板绘制信令
void MainWindow::sendSignalWhiteboardDraw(const QPoint &from,const QPoint &to,
                                          const QColor &color,int width,const QString &strokeId)
{
    if(!signalSocket||!signalConnected||whiteboardCanvas.isNull()) return;

    const int w=qMax(1,whiteboardCanvas.width()-1);
    const int h=qMax(1,whiteboardCanvas.height()-1);

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]="draw";
    obj["room"]=roomId;
    obj["stream"]=selfStream;
    obj["x1n"]=(from.x()*10000)/w;
    obj["y1n"]=(from.y()*10000)/h;
    obj["x2n"]=(to.x()*10000)/w;
    obj["y2n"]=(to.y()*10000)/h;
    obj["color"]=color.name(QColor::HexRgb);
    obj["pw"]=qBound(1,width,12);
    obj["stroke_id"]=strokeId;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 发送白板清空信令
void MainWindow::sendSignalWhiteboardClear()
{
    if(!signalSocket||!signalConnected) return;

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]="clear";
    obj["room"]=roomId;
    obj["stream"]=selfStream;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 发送白板撤销信令
void MainWindow::sendSignalWhiteboardUndo(const QString &strokeId)
{
    if(!signalSocket||!signalConnected||strokeId.isEmpty()) return;

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]="undo";
    obj["room"]=roomId;
    obj["stream"]=selfStream;
    obj["stroke_id"]=strokeId;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 判断当前用户是否可写白板
bool MainWindow::canWriteWhiteboard() const
{
    if(!whiteboardLocked) return true;
    if(!signalConnected) return true;
    if(!roomHostStream.isEmpty()&&selfStream==roomHostStream) return true;
    const MemberState st=memberStates.value(selfStream);
    return st.host||st.cohost;
}

// 判断当前用户是否可管理白板锁
bool MainWindow::canManageWhiteboard() const
{
    if(!signalConnected) return true;
    if(!roomHostStream.isEmpty()&&selfStream==roomHostStream) return true;
    if(!memberStates.contains(selfStream)) return false;
    const MemberState st=memberStates.value(selfStream);
    return st.host||st.cohost;
}

// 按权限与锁状态更新白板工具 UI
void MainWindow::applyWhiteboardLockUi()
{
    const bool canManage=canManageWhiteboard();

    if(whiteboardLockButton){
        QSignalBlocker guard(whiteboardLockButton);
        whiteboardLockButton->setChecked(whiteboardLocked);
        whiteboardLockButton->setEnabled(canManage);
        whiteboardLockButton->setText(whiteboardLocked ? "白板已锁":"白板解锁");
    }
    const bool canWrite = canWriteWhiteboard();
    if(whiteboardPenButton){
        whiteboardPenButton->setEnabled(canWrite);
    }
    if(whiteboardUndoButton){
        whiteboardUndoButton->setEnabled(canWrite);
    }
}

// 抓取焦点远端最近帧用于截图
bool MainWindow::captureFocusedRemoteImage(QImage *outImage) const
{
    if(!outImage) return false;
    QString target=focusedStream;
    if(target.isEmpty()) target=currentRemoteStream;
    if(target.isEmpty()) return false;
    const auto it=latestRemoteFrames.constFind(target);
    if(it==latestRemoteFrames.constEnd()||it->isNull()) return false;
    *outImage=*it;
    return !outImage->isNull();
}

// 发送白板锁定/解锁信令
void MainWindow::sendSignalWhiteboardLock(bool locked)
{
    if(!signalSocket||!signalConnected) return;

    QJsonObject obj;
    obj["type"]="wb";
    obj["op"]=locked ? "lock":"unlock";
    obj["room"]=roomId;
    obj["stream"]=selfStream;
    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["ts"]=nowMs;
    obj["msg_id"]=QString("wb_%1_%2_%3").arg(selfStream).arg(nowMs).arg(++whiteboardLocalSeq);

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 加载本地登录偏好（账号/房间）
void MainWindow::loadLoginPrefs()
{
    //创建QSettings对象，用于读写本地配置
    QSettings settings("SmartMeet", "SmartMeet");
    const bool remember = settings.value("auth/remember_user", true).toBool();
    const QString savedUser = settings.value("auth/last_user").toString().trimmed();
    const QString savedRoom = settings.value("auth/last_room").toString().trimmed();
    //如果开启了"记住用户"且保存的账号不为空，就把保存的账号赋值给当前登录用户变量
    if (remember && !savedUser.isEmpty()) {
        loginUser = savedUser;
    }
    if (!savedRoom.isEmpty()) {
        roomId = savedRoom;
    }
}

// 保存本地登录偏好（账号/房间）
void MainWindow::saveLoginPrefs() const
{
    QSettings settings("SmartMeet", "SmartMeet");
    const bool remember = rememberLoginCheck ? rememberLoginCheck->isChecked() : true;
    settings.setValue("auth/remember_user", remember);
    if (remember) {
        settings.setValue("auth/last_user", loginUser);
    } else {
        settings.remove("auth/last_user");
    }
    settings.setValue("auth/last_room", roomId);
}

// 创建并初始化登录遮罩层界面
void MainWindow::setupLoginUi()
{
    if (!ui || !ui->centralwidget || loginOverlay) return;

    loginOverlay = new QFrame(ui->centralwidget);
    loginOverlay->setObjectName("loginOverlay");
    loginOverlay->setStyleSheet(
        "QFrame#loginOverlay{background:rgba(9,12,19,205);}"
        "QFrame#loginCard{background:#1b2332;border:1px solid #34415a;border-radius:12px;}"
        "QLabel#loginTitleLabel{color:#f0f4ff;font-size:20px;font-weight:700;}"
        "QLabel#loginSubTitleLabel{color:#b8c4dc;font-size:13px;}"
        "QLabel#loginHintLabel{color:#ffd27a;font-size:12px;}"
        "QLineEdit#loginUserEdit,QLineEdit#loginPasswordEdit,QLineEdit#loginRoomEdit{"
        "background:#111826;color:#eaf0ff;border:1px solid #3d4e6b;border-radius:6px;padding:7px 10px;min-height:30px;}"
        "QPushButton#loginLoginButton{background:#2e86de;color:white;border:1px solid #4a9eee;border-radius:6px;padding:7px 14px;font-weight:700;}"
        "QPushButton#loginRegisterButton{background:#2d3a52;color:#dce7ff;border:1px solid #4a5f84;border-radius:6px;padding:7px 14px;font-weight:700;}"
        "QPushButton#loginLoginButton:hover{background:#3b92ea;}"
        "QPushButton#loginRegisterButton:hover{background:#364765;}"
    );
    loginOverlay->setFrameShape(QFrame::NoFrame);
    loginOverlay->setAttribute(Qt::WA_StyledBackground, true);

    loginCard = new QFrame(loginOverlay);
    loginCard->setObjectName("loginCard");
    loginCard->setFrameShape(QFrame::StyledPanel);
    loginCard->setAttribute(Qt::WA_StyledBackground, true);

    auto *cardLayout = new QVBoxLayout(loginCard);
    cardLayout->setContentsMargins(22, 22, 22, 22);
    cardLayout->setSpacing(12);

    auto *titleLabel = new QLabel("欢迎使用 SmartMeet", loginCard);
    titleLabel->setObjectName("loginTitleLabel");
    auto *subTitleLabel = new QLabel("登录后进入会议主界面", loginCard);
    subTitleLabel->setObjectName("loginSubTitleLabel");

    loginHintLabel = new QLabel(loginCard);
    loginHintLabel->setObjectName("loginHintLabel");
    loginHintLabel->setWordWrap(true);
    loginHintLabel->setText("请输入账号、密码和房间号");

    loginUserEdit = new QLineEdit(loginCard);
    loginUserEdit->setObjectName("loginUserEdit");
    loginUserEdit->setPlaceholderText("账号（例如：u1001）");
    loginUserEdit->setClearButtonEnabled(true);

    loginPasswordEdit = new QLineEdit(loginCard);
    loginPasswordEdit->setObjectName("loginPasswordEdit");
    loginPasswordEdit->setPlaceholderText("密码");
    loginPasswordEdit->setEchoMode(QLineEdit::Password);
    loginPasswordEdit->setClearButtonEnabled(true);

    loginRoomEdit = new QLineEdit(loginCard);
    loginRoomEdit->setObjectName("loginRoomEdit");
    loginRoomEdit->setPlaceholderText("房间号（6位数字）");
    loginRoomEdit->setClearButtonEnabled(true);

    rememberLoginCheck = new QCheckBox("记住账号", loginCard);
    const bool rememberUser = QSettings("SmartMeet", "SmartMeet").value("auth/remember_user", true).toBool();
    rememberLoginCheck->setChecked(rememberUser);
    rememberLoginCheck->setStyleSheet("QCheckBox{color:#c8d5ee;}");

    if (loginUser.isEmpty()) {
        loginUser = QString("u%1").arg(QRandomGenerator::global()->bounded(1000, 9999));
    }
    if (roomId.isEmpty()) {
        roomId = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
    }
    loginUserEdit->setText(loginUser);
    loginRoomEdit->setText(roomId);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(10);
    loginLoginButton = new QPushButton("登录并进入", loginCard);
    loginLoginButton->setObjectName("loginLoginButton");
    loginRegisterButton = new QPushButton("注册并进入", loginCard);
    loginRegisterButton->setObjectName("loginRegisterButton");
    buttonRow->addWidget(loginLoginButton);
    buttonRow->addWidget(loginRegisterButton);

    cardLayout->addWidget(titleLabel);
    cardLayout->addWidget(subTitleLabel);
    cardLayout->addWidget(loginHintLabel);
    cardLayout->addWidget(loginUserEdit);
    cardLayout->addWidget(loginPasswordEdit);
    cardLayout->addWidget(loginRoomEdit);
    cardLayout->addWidget(rememberLoginCheck);
    cardLayout->addLayout(buttonRow);

    connect(loginLoginButton, &QPushButton::clicked, this, [this]() {
        triggerLoginAction(false);
    });
    connect(loginRegisterButton, &QPushButton::clicked, this, [this]() {
        triggerLoginAction(true);
    });
    connect(loginUserEdit, &QLineEdit::returnPressed, this, [this]() {
        triggerLoginAction(false);
    });
    connect(loginPasswordEdit, &QLineEdit::returnPressed, this, [this]() {
        triggerLoginAction(false);
    });
    connect(loginRoomEdit, &QLineEdit::returnPressed, this, [this]() {
        triggerLoginAction(false);
    });
    connect(rememberLoginCheck, &QCheckBox::toggled, this, [this](bool){
        saveLoginPrefs();
    });

    syncLoginOverlayGeometry();
    loginOverlay->hide();
}

// 同步登录遮罩层几何位置
void MainWindow::syncLoginOverlayGeometry()
{
    if (!ui || !ui->centralwidget || !loginOverlay || !loginCard) return;

    loginOverlay->setGeometry(ui->centralwidget->rect());

    const int overlayW = loginOverlay->width();
    const int overlayH = loginOverlay->height();
    const int cardW = qBound(340, overlayW - 40, 460);
    loginCard->setFixedWidth(cardW);
    loginCard->adjustSize();

    const int x = (overlayW - loginCard->width()) / 2;
    const int y = qMax(20, (overlayH - loginCard->height()) / 2);
    loginCard->move(x, y);

    loginOverlay->raise();
}

// 显示或隐藏登录遮罩并更新提示
void MainWindow::showLoginOverlay(bool show, const QString &hint)
{
    if (!loginOverlay) {
        setupLoginUi();
    }
    if (!loginOverlay) return;

    if (loginHintLabel) {
        if (!hint.isEmpty()) loginHintLabel->setText(hint);
    }
    if (loginUserEdit && !loginUser.isEmpty()) {
        loginUserEdit->setText(loginUser);
    }
    if (loginRoomEdit && !roomId.isEmpty()) {
        loginRoomEdit->setText(roomId);
    }

    loginOverlay->setVisible(show);
    if (show) {
        syncLoginOverlayGeometry();
        if (loginPasswordEdit && loginPasswordEdit->text().isEmpty()) {
            loginPasswordEdit->setFocus();
        } else if (loginUserEdit) {
            loginUserEdit->setFocus();
            loginUserEdit->selectAll();
        }
    }
}

// 触发登录/注册流程并发起信令连接
void MainWindow::triggerLoginAction(bool registerFirst)
{
    if (!loginUserEdit || !loginPasswordEdit || !loginRoomEdit) return;

    const QString user = loginUserEdit->text().trimmed();
    const QString password = loginPasswordEdit->text();
    const QString room = loginRoomEdit->text().trimmed();

    if (user.isEmpty() || password.isEmpty() || room.isEmpty()) {
        showLoginOverlay(true, "账号、密码、房间号都不能为空");
        return;
    }

    loginUser = user;
    loginPassword = password;
    roomId = room;
    userId = loginUser;
    saveLoginPrefs();
    pendingAuthRegister = registerFirst;
    signalAuthed = false;
    authRegisterTried = registerFirst;

    if (signalConnected) {
        showLoginOverlay(true, registerFirst ? "正在注册并登录..." : "正在登录...");
        if (registerFirst) sendSignalAuthRegister();
        else sendSignalAuthLogin();
        return;
    }

    showLoginOverlay(true, registerFirst ? "正在连接并注册..." : "正在连接并登录...");
    on_startReceiveButton_clicked();
}

// 退出登录：断开信令并清理登录态
void MainWindow::on_logoutButton_clicked()
{
    appendRoomEvent("手动退出登录");
    manualSignalDisconnect = true;
    resetSignalReconnectState();

    if (signalSocket &&
        (signalSocket->state() == QAbstractSocket::ConnectedState
         || signalSocket->state() == QAbstractSocket::ConnectingState)) {
        if (signalConnected) {
            sendSignalLeave();
        }
        signalSocket->close();
    }

    signalAuthed = false;
    pendingAuthRegister = false;
    authRegisterTried = false;
    signalConnected = false;
    loginPassword.clear();
    userId.clear();
    selfStream.clear();

    if (rememberLoginCheck && !rememberLoginCheck->isChecked()) {
        loginUser.clear();
    }
    saveLoginPrefs();

    if (loginPasswordEdit) loginPasswordEdit->clear();
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
    updateMeetingHeaderUi();
    updatePrimaryActionTexts();

    showLoginOverlay(true, "已退出登录，请重新登录");
}

// 校验并补齐信令登录所需凭据
bool MainWindow::ensureSignalCredential()
{
    if (loginUser.isEmpty() && loginUserEdit) {
        loginUser = loginUserEdit->text().trimmed();
    }
    if (loginPassword.isEmpty() && loginPasswordEdit) {
        loginPassword = loginPasswordEdit->text();
    }
    if (roomId.isEmpty() && loginRoomEdit) {
        roomId = loginRoomEdit->text().trimmed();
    }

    if (loginUser.isEmpty() || loginPassword.isEmpty()) {
        showLoginOverlay(true, "请先登录后再连接信令");
        return false;
    }
    if (roomId.isEmpty()) {
        showLoginOverlay(true, "请填写房间号");
        return false;
    }

    if (userId.isEmpty()) userId = loginUser;
    return true;
}

// 发送 auth_login 认证消息
void MainWindow::sendSignalAuthLogin()
{
    if (!signalSocket || !signalConnected) return;
    QJsonObject obj;
    obj["type"] = "auth_login";
    obj["user"] = loginUser;
    obj["password"] = loginPassword;
    obj["ver"] = 1;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 发送 auth_register 注册消息
void MainWindow::sendSignalAuthRegister()
{
    if (!signalSocket || !signalConnected) return;
    QJsonObject obj;
    obj["type"] = "auth_register";
    obj["user"] = loginUser;
    obj["password"] = loginPassword;
    obj["ver"] = 1;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 录制按钮入口：开始 AV 录制
void MainWindow::on_startRecordButton_clicked()
{
    if(recorder&&recorder->isOpen()){
        recorder->close();
    }
    if(!recorder){
        recorder=new AvRecorder(this);
    }

    const int W=640,H=480,FPS=30,SR=44100;
    const QString filename=QDateTime::currentDateTime().toString("'record_av_'yyyyMMdd_hhmmss'.mp4'");

    if(!recorder->openAV(filename,W,H,FPS,SR)){
        QMessageBox::warning(this,"错误","openAV失败");
        return;
    }

    camRecording=true;// 视频线程开始push
    isRecording=true;// 标记音频线程也能push

    QMessageBox::information(this,"提示",QString("开始AV录制: %1").arg(filename));
}

// 录制按钮入口：停止 AV 录制
void MainWindow::on_stopRecordButton_clicked()
{
    if(recorder&&recorder->isOpen()){
        recorder->close();
        QMessageBox::information(this,"提示","AV录制已停止并保存");
    }
    camRecording=false;
    isRecording=false;
}


// 初始化弱网自适应恢复定时器
void MainWindow::setupAdaptiveNetworkControl()
{
    if (adaptiveRecoverTimer) return;
    adaptiveRecoverTimer = new QTimer(this);
    adaptiveRecoverTimer->setInterval(5000);
    connect(adaptiveRecoverTimer, &QTimer::timeout, this, [this]() {
        tryRecoverAdaptiveProfile();
    });
    adaptiveRecoverTimer->start();
}

// 应用网络自适应档位并按需重建推流链路
void MainWindow::applyAdaptiveProfile(int level, bool restartPipeline)
{
    static const PublishProfile kProfiles[] = {
        {QStringLiteral("高清"), 1280, 720, 30, 2200000, 1920, 1080},
        {QStringLiteral("均衡"), 960, 540, 24, 1400000, 1600, 900},
        {QStringLiteral("流畅"), 640, 360, 20, 900000, 1280, 720}
    };

    const int targetLevel = qBound(0, level, 2);
    const bool changed = (adaptiveProfileLevel != targetLevel);
    adaptiveProfileLevel = targetLevel;
    const PublishProfile &p = kProfiles[adaptiveProfileLevel];

    if (videoWorker) {
        // VideoCapture::captureLoop 常驻，无事件循环，QueuedConnection 可能不生效。
        videoWorker->setTargetFps(p.fps);
        videoWorker->setShareMaxSize(p.shareMaxW, p.shareMaxH);
    }

    if (changed) {
        appendRoomEvent(QString("网络策略切换：%1（%2x%3@%4, %5kbps）")
                            .arg(p.name).arg(p.width).arg(p.height).arg(p.fps).arg(p.bitrate / 1000));
        updateMeetingStatsUi();
    }

    if (!restartPipeline) return;
    if (meetingStopped || !isPublishing) return;
    if (!netEnc || !pusher || !encThread || !pushThread) return;
    if (!encThread->isRunning() || !pushThread->isRunning()) return;

    if (currentPushUrl.isEmpty() && !selfStream.isEmpty()) {
        currentPushUrl = buildRtmpPublishUrl(selfStream);
    }
    if (currentPushUrl.isEmpty()) return;

    bool encOk = false;
    QMetaObject::invokeMethod(netEnc, [&]() {
        netEnc->close();
        encOk = netEnc->openVideo(p.width, p.height, p.fps, p.bitrate);
    }, Qt::BlockingQueuedConnection);

    bool pushOk = false;
    QMetaObject::invokeMethod(pusher, [&]() {
        pusher->stop();
        pusher->setVideoParams(p.width, p.height, p.fps);
        pusher->setAudioParams(44100, 1);
        pushOk = pusher->start(currentPushUrl, p.fps, 44100);
    }, Qt::BlockingQueuedConnection);

    if (!encOk || !pushOk) {
        appendRoomEvent("网络策略切换失败：推流重建失败");
        qWarning() << "[Adaptive] profile rebuild failed encOk=" << encOk << " pushOk=" << pushOk;
        isPublishing = false;
        updateMeetingStatsUi();
        return;
    }

    qInfo() << "[Adaptive] profile applied" << p.name << p.width << p.height << p.fps << p.bitrate;
    updateMeetingStatsUi();
}

// 记录网络异常并触发降档策略
void MainWindow::handleNetworkIssue(const QString &reason, int weight)
{
    if (meetingStopped || !isPublishing) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    lastAdaptiveIssueMs = now;
    adaptiveStressScore = qMin(12, adaptiveStressScore + qMax(1, weight));

    if (now - lastAdaptiveLogMs > 1500) {
        appendRoomEvent(QString("网络波动: %1").arg(reason));
        lastAdaptiveLogMs = now;
    }

    if (adaptiveStressScore >= 3 && adaptiveProfileLevel < 2) {
        adaptiveStressScore = 0;
        applyAdaptiveProfile(adaptiveProfileLevel + 1, true);
    }
}

// 网络稳定后尝试逐级恢复档位
void MainWindow::tryRecoverAdaptiveProfile()
{
    if (meetingStopped || !isPublishing) return;
    if (adaptiveProfileLevel <= 0) return;
    if (lastAdaptiveIssueMs <= 0) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - lastAdaptiveIssueMs < 25000) return;

    adaptiveStressScore = 0;
    lastAdaptiveIssueMs = now;
    applyAdaptiveProfile(adaptiveProfileLevel - 1, true);
}

// 初始化会中统计栏与刷新定时器
void MainWindow::setupMeetingStatsUi()
{
    meetingStatsLabel = findChild<QLabel*>("meetingStatsLabel");
    if (!meetingStatsLabel && statusBar()) {
        meetingStatsLabel = new QLabel(this);
        meetingStatsLabel->setObjectName("meetingStatsLabelFallback");
        statusBar()->addPermanentWidget(meetingStatsLabel, 1);
    }
    if (!meetingStatsTimer) {
        meetingStatsTimer = new QTimer(this);
        meetingStatsTimer->setInterval(1000);
        connect(meetingStatsTimer, &QTimer::timeout, this, &MainWindow::updateMeetingStatsUi);
        meetingStatsTimer->start();
    }
    updateMeetingStatsUi();
}

// 刷新会中统计文本
void MainWindow::updateMeetingStatsUi()
{
    struct ProfileView {
        const char *name;
        int w;
        int h;
        int fps;
        int bitrate;
    };
    static const ProfileView kProfiles[] = {
        {"高清", 1280, 720, 30, 2200000},
        {"均衡", 960, 540, 24, 1400000},
        {"流畅", 640, 360, 20, 900000}
    };

    const bool compact = isCompactMeetingLayout();
    const bool ultraCompact = isUltraCompactMeetingLayout();
    const QString stageText = isPublishing ? "推流中" : "未推流";
    const QString stageShortText = isPublishing ? "推流" : "待机";
    QString profileText = "--";
    QString avText = "--";
    QString brText = "--";
    if (adaptiveProfileLevel >= 0 && adaptiveProfileLevel <= 2) {
        const ProfileView &p = kProfiles[adaptiveProfileLevel];
        profileText = QString::fromUtf8(p.name);
        avText = QString("%1x%2@%3").arg(p.w).arg(p.h).arg(p.fps);
        brText = QString("%1kbps").arg(p.bitrate / 1000);
    }

    const QString fullText = QString("%1 | 档位:%2 | %3 | %4 | 重连:信令%5 拉流%6")
                                 .arg(stageText)
                                 .arg(profileText)
                                 .arg(avText)
                                 .arg(brText)
                                 .arg(totalSignalReconnectCount)
                                 .arg(totalPullRetryCount);
    QString text = fullText;
    if (ultraCompact) {
        text = QString("%1 | %2 | %3/%4")
                   .arg(stageShortText)
                   .arg(profileText)
                   .arg(totalSignalReconnectCount)
                   .arg(totalPullRetryCount);
    } else if (compact) {
        text = QString("%1 | %2 | %3 | %4 | 重连%5/%6")
                   .arg(stageText)
                   .arg(profileText)
                   .arg(avText)
                   .arg(brText == "--" ? brText : brText.replace("kbps", "k"))
                   .arg(totalSignalReconnectCount)
                   .arg(totalPullRetryCount);
    }
    if (meetingStatsLabel) {
        meetingStatsLabel->setText(text);
        meetingStatsLabel->setToolTip(fullText);
    }
}

// 导出最近会中事件日志到文本文件
void MainWindow::exportRecentMeetingLogs()
{
    if (recentEventLogs.isEmpty()) {
        QMessageBox::information(this, "提示", "当前没有可导出的会议日志");
        return;
    }

    const QString defaultName = QString("meeting_log_%1.txt")
                                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));
    const QString defaultPath = QDir::current().absoluteFilePath(defaultName);
    const QString filePath = QFileDialog::getSaveFileName(
        this, "导出会中日志", defaultPath, "文本文件 (*.txt);;所有文件 (*)");
    if (filePath.isEmpty()) return;

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "导出失败", QString("无法写入文件：%1").arg(filePath));
        return;
    }

    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << "SmartMeet 会中日志导出\n";
    out << "导出时间: " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << "\n";
    out << "房间: " << roomId << "  用户流: " << selfStream << "\n";
    out << "------------------------------\n";
    for (const QString &line : recentEventLogs) {
        out << line << '\n';
    }
    f.close();

    appendRoomEvent(QString("会中日志已导出：%1").arg(filePath));
}

// 重置信令重连计数和计时器状态
void MainWindow::resetSignalReconnectState()
{
    signalReconnectAttempt = 0;
    if (signalReconnectTimer && signalReconnectTimer->isActive()) {
        signalReconnectTimer->stop();
    }
}

// 按指数退避策略安排信令重连
void MainWindow::scheduleSignalReconnect(const QString &reason)
{
    if (manualSignalDisconnect || meetingStopped || shuttingDown) return;
    if (signalConnected) return;
    if (!signalReconnectTimer) return;
    if (signalReconnectTimer->isActive()) return;

    if (signalReconnectAttempt >= signalReconnectMaxAttempt) {
        appendRoomEvent(QString("信令重连已停止（超过 %1 次）").arg(signalReconnectMaxAttempt));
        return;
    }

    ++signalReconnectAttempt;
    const int exp = qMin(signalReconnectAttempt - 1, 5);
    const int delayMs = qMin(15000, 600 * (1 << exp));

    if (!reason.isEmpty()) {
        appendRoomEvent(QString("%1，准备重连（%2/%3）")
                            .arg(reason)
                            .arg(signalReconnectAttempt)
                            .arg(signalReconnectMaxAttempt));
    }
    appendRoomEvent(QString("将在 %1 ms 后重连信令").arg(delayMs));
    signalReconnectTimer->start(delayMs);
}

// 建立 WebSocket 信令连接并绑定回调
void MainWindow::openSignalConnection()
{
    if (!signalSocket) {
        signalSocket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
        connect(signalSocket, &QWebSocket::connected, this, &MainWindow::onSignalConnected);
        connect(signalSocket, &QWebSocket::disconnected, this, &MainWindow::onSignalDisconnected);
        connect(signalSocket, &QWebSocket::textMessageReceived, this, &MainWindow::onSignalTextMessage);
        connect(signalSocket, &QWebSocket::errorOccurred, this, [this](QAbstractSocket::SocketError){
            if (!signalSocket) return;
            appendRoomEvent(QString("信令错误: %1").arg(signalSocket->errorString()));
        });
    }

    if (signalSocket->state() == QAbstractSocket::ConnectedState
        || signalSocket->state() == QAbstractSocket::ConnectingState) {
        appendRoomEvent("信令连接进行中");
        return;
    }

    if (signalStateLabel) signalStateLabel->setText("信令: 连接中");
    appendRoomEvent(QString("连接信令服务器: %1").arg(signalUrl));
    signalSocket->open(QUrl(signalUrl));
    updateMeetingHeaderUi();
    updatePrimaryActionTexts();
}


// 连接信令按钮逻辑：连接或手动断开
void MainWindow::on_startReceiveButton_clicked()
{
    const bool connectedOrConnecting =
        signalSocket && (signalSocket->state() == QAbstractSocket::ConnectedState
                         || signalSocket->state() == QAbstractSocket::ConnectingState);

    if (signalConnected || connectedOrConnecting) {
        manualSignalDisconnect = true;
        resetSignalReconnectState();
        appendRoomEvent("手动断开信令");
        if (signalSocket) {
            if (signalConnected) sendSignalLeave();
            signalSocket->close();
        }
        return;
    }

    manualSignalDisconnect = false;
    resetSignalReconnectState();

    if (this->QObject::sender() == ui->startReceiveButton) {
        pendingAuthRegister = false;
    }
    if (!ensureSignalCredential()) {
        return;
    }
    if (!ensureRoomIdentity(true)) {
        return;
    }

    openSignalConnection();
    qInfo() << "[Mainwindow] 接收端已启动(信令)";
}

// 初始化房间成员/事件/聊天等信令面板控件
void MainWindow::setupSignalUi()
{
    if (roomDock && roomUserList && roomEventLog && signalStateLabel && roomCountLabel &&
        chatMessageLog && chatInputEdit && sendChatButton) return;

    // 优先使用 UI(XML) 中已有控件。
    roomDock = findChild<QDockWidget*>("roomDock");
    signalStateLabel = findChild<QLabel*>("signalStateLabel");
    roomCountLabel = findChild<QLabel*>("roomCountLabel");
    roomUserList = findChild<QListWidget*>("roomUserList");
    roomEventLog = findChild<QPlainTextEdit*>("roomEventLog");
    chatMessageLog=findChild<QPlainTextEdit*>("chatMessageLog");
    chatInputEdit=findChild<QTextEdit*>("chatInputEdit");
    sendChatButton=findChild<QPushButton*>("sendChatButton");
    whiteboardCanvasLabel=findChild<QLabel*>("whiteboardCanvasLabel");
    whiteboardClearButton=findChild<QPushButton*>("whiteboardClearButton");
    whiteboardPenButton=findChild<QToolButton*>("whiteboardPenButton");
    whiteboardColorCombo=findChild<QComboBox*>("whiteboardColorCombo");
    whiteboardWidthSpin=findChild<QSpinBox*>("whiteboardWidthSpin");
    whiteboardUndoButton=findChild<QPushButton*>("whiteboardUndoButton");
    whiteboardLockButton=findChild<QToolButton*>("whiteboardLockButton");
    setupWhiteboardUi();

    // 若 UI 尚未放入这些控件，则回退到代码创建，保证兼容旧 ui 文件。
    if (!roomDock || !signalStateLabel || !roomCountLabel || !roomUserList || !roomEventLog) {
        roomDock = new QDockWidget("房间成员", this);
        roomDock->setObjectName("roomDock");

        QWidget *panel = new QWidget(roomDock);
        QVBoxLayout *layout = new QVBoxLayout(panel);
        layout->setContentsMargins(8, 8, 8, 8);
        layout->setSpacing(6);

        signalStateLabel = new QLabel("信令: 未连接", panel);
        signalStateLabel->setObjectName("signalStateLabel");
        roomCountLabel = new QLabel("在线: 0", panel);
        roomCountLabel->setObjectName("roomCountLabel");
        roomUserList = new QListWidget(panel);
        roomUserList->setObjectName("roomUserList");
        roomUserList->setContextMenuPolicy(Qt::CustomContextMenu);
        roomEventLog = new QPlainTextEdit(panel);
        roomEventLog->setObjectName("roomEventLog");
        roomEventLog->setReadOnly(true);
        roomEventLog->setMaximumBlockCount(200);
        roomEventLog->setPlaceholderText("会议事件日志");

        layout->addWidget(signalStateLabel);
        layout->addWidget(roomCountLabel);
        layout->addWidget(roomUserList, 1);
        layout->addWidget(roomEventLog, 1);
        panel->setLayout(layout);

        roomDock->setWidget(panel);
        addDockWidget(Qt::RightDockWidgetArea, roomDock);
    } else {
        roomUserList->setContextMenuPolicy(Qt::CustomContextMenu);
        roomEventLog->setReadOnly(true);
        roomEventLog->setMaximumBlockCount(200);
    }

    if (roomDock) {
        roomDock->setMinimumWidth(300);
        roomDock->setMaximumWidth(420);
        roomDock->setAllowedAreas(Qt::RightDockWidgetArea);
        roomDock->setFeatures(QDockWidget::DockWidgetClosable
                              | QDockWidget::DockWidgetMovable
                              | QDockWidget::DockWidgetFloatable);
    }
    if (auto *roomPanelToggleButton = findChild<QToolButton*>("roomPanelToggleButton")) {
        auto syncRoomPanelToggle = [this, roomPanelToggleButton]() {
            const bool visible = roomDock && roomDock->isVisible();
            QSignalBlocker blocker(roomPanelToggleButton);
            roomPanelToggleButton->setChecked(visible);
            updateRoomPanelToggleText();
            updateRoomDockPresentation();
        };
        disconnect(roomPanelToggleButton, nullptr, this, nullptr);
        connect(roomPanelToggleButton, &QToolButton::clicked, this, [this, syncRoomPanelToggle](bool checked) {
            if (!roomDock) return;
            roomDock->setVisible(checked);
            if (checked) {
                roomDock->raise();
            }
            syncRoomPanelToggle();
        });
        if (roomDock) {
            disconnect(roomDock, nullptr, this, nullptr);
            connect(roomDock, &QDockWidget::visibilityChanged, this, [syncRoomPanelToggle](bool) {
                syncRoomPanelToggle();
            });
        }
        syncRoomPanelToggle();
    }
    if (auto *tabs = findChild<QTabWidget*>("roomTabWidget")) {
        disconnect(tabs, nullptr, this, nullptr);
        connect(tabs, &QTabWidget::currentChanged, this, [this](int) {
            updateRoomDockPresentation();
        });
    }
    applyResponsiveLayout();
    if (auto *tabs = findChild<QTabWidget*>("roomTabWidget")) {
        if (tabs->currentIndex() < 0 || tabs->currentIndex() == 3) {
            tabs->setCurrentIndex(0);
        }
    }

    // M3: 聊天输入区初始化（优先使用 chatTab；兼容旧 UI 时回退到 eventTab）。
    if (!chatMessageLog) {
        chatMessageLog = roomEventLog;
    }
    if (!chatInputEdit || !sendChatButton) {
        QWidget *chatTab = findChild<QWidget*>("chatTab");
        auto *chatLayout = chatTab ? qobject_cast<QVBoxLayout*>(chatTab->layout()) : nullptr;
        if (!chatLayout) {
            QWidget *eventTab = findChild<QWidget*>("eventTab");
            chatLayout = eventTab ? qobject_cast<QVBoxLayout*>(eventTab->layout()) : nullptr;
            chatTab = eventTab;
        }
        if (chatLayout) {
            QWidget *parentWidget = chatTab ? chatTab : this;
            auto *chatRow = new QHBoxLayout;
            chatRow->setSpacing(6);

            chatInputEdit = new QTextEdit(parentWidget);
            chatInputEdit->setObjectName("chatInputEdit");
            chatInputEdit->setPlaceholderText(QStringLiteral("输入消息，Enter发送，Shift+Enter换行"));
            chatInputEdit->setAcceptRichText(false);
            chatInputEdit->setFixedHeight(56);

            sendChatButton = new QPushButton(QStringLiteral("发送"), parentWidget);
            sendChatButton->setObjectName("sendChatButton");
            sendChatButton->setMinimumWidth(56);

            chatRow->addWidget(chatInputEdit, 1);
            chatRow->addWidget(sendChatButton, 0);

            chatLayout->addLayout(chatRow);
        }
    }
    if (sendChatButton) {
        connect(sendChatButton, &QPushButton::clicked, this, &MainWindow::onSendChatClicked, Qt::UniqueConnection);
    }
    if (chatInputEdit) {
        chatInputEdit->setAcceptRichText(false);
        chatInputEdit->installEventFilter(this);
    }


    connect(roomUserList, &QListWidget::itemDoubleClicked, this, &MainWindow::onRoomUserDoubleClicked, Qt::UniqueConnection);
    connect(roomUserList, &QListWidget::customContextMenuRequested, this, &MainWindow::onRoomListContextMenu, Qt::UniqueConnection);
}

// 初始化底部菜单（美颜、更多、自控动作）
void MainWindow::setupBottomMenus()//聊天面板入口
{
    beautyStrengthSlider=findChild<QSlider*>("beautyStrengthSlider");
    beautyStrengthValueLabel=findChild<QLabel*>("beautyStrengthValueLabel");
    if(beautyStrengthSlider){
        beautyStrengthSlider->setRange(0,100);
        if(localBeautyLevel<=0){
            localBeautyLevel=60;
        }
        beautyStrengthSlider->setValue(qBound(0,localBeautyLevel,100));
        if(beautyStrengthValueLabel){
            beautyStrengthValueLabel->setText(QString::number(beautyStrengthSlider->value()));
        }
        disconnect(beautyStrengthSlider,nullptr,this,nullptr);
        connect(beautyStrengthSlider,&QSlider::valueChanged,this,[this](int v){
            const int vv=qBound(0,v,100);
            localBeautyLevel=vv;
            if(beautyStrengthValueLabel){
                beautyStrengthValueLabel->setText(QString::number(vv));
            }
            if(localBeautyStyle>0){
                applyBeautyToWorker();
            }
        });
    }

    auto *beautyBtn = findChild<QToolButton*>("beautyMenuButton");
    if (beautyBtn && !beautyBtn->menu()) {
        auto *beautyMenu = new QMenu(beautyBtn);
        auto *group=new QActionGroup(beautyMenu);
        group->setExclusive(true);

        auto *off=beautyMenu->addAction("关闭");
        auto *natural = beautyMenu->addAction("自然");
        auto *clear = beautyMenu->addAction("清晰");
        auto *soft = beautyMenu->addAction("柔和");
        beautyMenu->addSeparator();
        auto *skin = beautyMenu->addAction("磨皮");
        auto *slim = beautyMenu->addAction("瘦脸");
        auto *wrinkle = beautyMenu->addAction("祛皱");

        for(QAction *a : {off,natural,clear,soft,skin,slim,wrinkle}){
            a->setCheckable(true);
            a->setActionGroup(group);
        }

        switch (localBeautyStyle) {
        case 1: natural->setChecked(true); break;
        case 2: clear->setChecked(true); break;
        case 3: soft->setChecked(true); break;
        case 4: skin->setChecked(true); break;
        case 5: slim->setChecked(true); break;
        case 6: wrinkle->setChecked(true); break;
        default: off->setChecked(true); break;
        }

        connect(off,&QAction::triggered,this,[this](){setBeautyMode("关闭",0);});
        connect(natural,&QAction::triggered,this,[this](){setBeautyMode("自然",-1);});
        connect(clear,&QAction::triggered,this,[this](){setBeautyMode("清晰",-1);});
        connect(soft,&QAction::triggered,this,[this](){setBeautyMode("柔和",-1);});
        connect(skin,&QAction::triggered,this,[this](){setBeautyMode("磨皮",-1);});
        connect(slim,&QAction::triggered,this,[this](){setBeautyMode("瘦脸",-1);});
        connect(wrinkle,&QAction::triggered,this,[this](){setBeautyMode("祛皱",-1);});

        beautyBtn->setMenu(beautyMenu);
    }

    auto *moreBtn = findChild<QToolButton*>("moreMenuButton");
    if (moreBtn && !moreBtn->menu()) {
        auto *moreMenu = new QMenu(moreBtn);

        auto *pickShareSourceAction=moreMenu->addAction("选择共享源...");
        connect(pickShareSourceAction,&QAction::triggered,this,&MainWindow::chooseShareSource);

        selfShareToggleAction=moreMenu->addAction(localScreenShareOn?"停止屏幕共享":"开始屏幕共享");
        connect(selfShareToggleAction,&QAction::triggered,this,[this](){
            if(meetingStopped||!videoWorker){
                appendRoomEvent("请先开始会议后再进行屏幕共享");
                return;
            }
            localScreenShareOn=!localScreenShareOn;
            applyLocalVideoSource();
            sendSignalUpdate();
            appendRoomEvent(localScreenShareOn?"你已开启屏幕共享":"你已停止屏幕共享");
            refreshSelfControlActions();
        });

        auto *whiteboard = moreMenu->addAction("协作白板");
        connect(whiteboard,&QAction::triggered,this,[this](){
            if(roomDock) roomDock->show();
            if(auto *tabs=findChild<QTabWidget*>("roomTabWidget")){
                if(auto *wbTab=findChild<QWidget*>("whiteboardTab")){
                    tabs->setCurrentWidget(wbTab);
                }
            }
        });

        auto *chat = moreMenu->addAction("聊天面板");
        connect(chat, &QAction::triggered, this, [this]() {
            if(roomDock) roomDock->show();
            if (auto *tabs = findChild<QTabWidget*>("roomTabWidget")) {
                if (auto *chatTab = findChild<QWidget*>("chatTab")) {
                    tabs->setCurrentWidget(chatTab);
                } else if (auto *eventTab = findChild<QWidget*>("eventTab")) {
                    tabs->setCurrentWidget(eventTab);
                }
            }
            if (chatInputEdit) chatInputEdit->setFocus();
        });
        auto *exportLogs = moreMenu->addAction("导出最近日志");
        connect(exportLogs, &QAction::triggered, this, [this]() {
            exportRecentMeetingLogs();
        });
        moreMenu->addSeparator();
        selfMicToggleAction = moreMenu->addAction("静音我自己");
        selfCamToggleAction = moreMenu->addAction("关闭我的摄像头");
        connect(selfMicToggleAction, &QAction::triggered, this, [this]() {
            localAudioOn = !localAudioOn;
            sendSignalUpdate();
            appendRoomEvent(localAudioOn ? "你已恢复麦克风" : "你已静音自己");
            refreshSelfControlActions();
        });
        connect(selfCamToggleAction, &QAction::triggered, this, [this]() {
            localVideoOn = !localVideoOn;
            sendSignalUpdate();
            appendRoomEvent(localVideoOn ? "你已开启摄像头" : "你已关闭自己的摄像头");
            refreshSelfControlActions();
        });
        moreBtn->setMenu(moreMenu);
    }
    refreshSelfControlActions();
}

// 刷新本端麦克风/摄像头/共享动作文案
void MainWindow::refreshSelfControlActions()
{
    if (selfMicToggleAction) {
        selfMicToggleAction->setText(localAudioOn ? "静音我自己" : "恢复我的麦克风");
    }
    if (selfCamToggleAction) {
        selfCamToggleAction->setText(localVideoOn ? "关闭我的摄像头" : "开启我的摄像头");
    }
    if(selfShareToggleAction){
        selfShareToggleAction->setText(localScreenShareOn?"停止屏幕共享":"开始屏幕共享");
    }
    updateMeetingHeaderUi();
    if(whiteboardClearButton){
        whiteboardClearButton->setEnabled(canClearWhiteboard());
    }
    applyWhiteboardLockUi();
}

// 统一处理聊天回车、白板绘制、宫格双击等事件
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == chatInputEdit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers().testFlag(Qt::ShiftModifier)) {
                return false;
            }
            onSendChatClicked();
            return true;
        }
    }

    if(watched==whiteboardCanvasLabel){
        if(event->type()==QEvent::Show||event->type()==QEvent::Resize){
            ensureWhiteboardCanvas();
            return false;
        }

        if(!canWriteWhiteboard()){
            if(event->type()==QEvent::MouseButtonPress){
                appendRoomEvent("白板已锁定,仅主持人/联席主持人可书写");
            }
            if(event->type()==QEvent::MouseButtonPress
                || event->type()==QEvent::MouseButtonRelease
                || event->type()==QEvent::MouseMove
                || event->type()==QEvent::MouseButtonDblClick){
                return true;
            }
            return false;
        }

        if(!whiteboardPenEnabled) return false;

        auto *me=dynamic_cast<QMouseEvent*>(event);
        if(!me) return false;

        if(event->type()==QEvent::MouseButtonPress&&me->button()==Qt::LeftButton){
            whiteboardMouseDown=true;
            whiteboardLastPoint=mapWhiteboardPoint(me->pos());
            whiteboardActionStrokeId=QString("st_%1_%2_%3")
                                           .arg(selfStream)
                                           .arg(QDateTime::currentMSecsSinceEpoch())
                                           .arg(++whiteboardLocalSeq);
            return true;
        }

        if(event->type()==QEvent::MouseMove && whiteboardMouseDown && (me->buttons() & Qt::LeftButton)){
            const QPoint cur=mapWhiteboardPoint(me->pos());
            drawWhiteboardLine(whiteboardLastPoint,cur,true,
                                whiteboardSelectedColor(),whiteboardSelectedWidth(),
                                whiteboardActionStrokeId,selfStream);
            whiteboardLastPoint=cur;
            return true;
        }

        if(event->type()==QEvent::MouseButtonRelease&&me->button()==Qt::LeftButton){
            if(whiteboardMouseDown){
                const QPoint cur=mapWhiteboardPoint(me->pos());
                drawWhiteboardLine(whiteboardLastPoint,cur,true,
                                    whiteboardSelectedColor(),whiteboardSelectedWidth(),
                                    whiteboardActionStrokeId,selfStream);
            }
            whiteboardMouseDown=false;
            whiteboardActionStrokeId.clear();
            return true;
        }
    }

    if (watched && watched->objectName() == "remoteStageFrame" &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move ||
         event->type() == QEvent::Show || event->type() == QEvent::LayoutRequest)) {
        syncRemoteContainerGeometry();
    }

    if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
        bool ok = false;
        const int idx = watched->property("tileIndex").toInt(&ok);
        if (ok && idx >= 0 && idx < remoteTiles.size()) {
            auto &tile = remoteTiles[idx];
            if (tile.cornerBadge && tile.frame) {
                tile.cornerBadge->adjustSize();
                tile.cornerBadge->move(tile.frame->width() - tile.cornerBadge->width() - 8, 8);
                tile.cornerBadge->raise();
            }
        }
    }

    const bool isFocusSurface =
        (ui && watched == ui->remoteVideolabel) ||
        (watched == remoteContainer) ||
        (watched && watched->objectName() == "remoteStageFrame");
    if (isFocusSurface && event->type() == QEvent::MouseButtonDblClick) {
        if (!focusMode || focusedStream.isEmpty()) return true;
        if (!focusPreviewFullScreen) {
            focusPreviewFullScreen = true;
            if (roomDock) roomDock->hide();
            if (auto *topBar = findChild<QWidget*>("topBarFrame")) topBar->hide();
            if (auto *bottomBar = findChild<QWidget*>("bottomControlFrame")) bottomBar->hide();
            if (auto *localSide = findChild<QWidget*>("localSideFrame")) localSide->hide();
            if (menuBar()) menuBar()->hide();
            if (statusBar()) statusBar()->hide();
            showFullScreen();
            syncRemoteContainerGeometry();
            appendRoomEvent("已进入焦点全屏（双击退出）");
        } else {
            focusPreviewFullScreen = false;
            showNormal();
            if (roomDock) roomDock->show();
            if (auto *topBar = findChild<QWidget*>("topBarFrame")) topBar->show();
            if (auto *bottomBar = findChild<QWidget*>("bottomControlFrame")) bottomBar->show();
            if (auto *localSide = findChild<QWidget*>("localSideFrame")) localSide->show();
            if (menuBar()) menuBar()->show();
            if (statusBar()) statusBar()->show();
            syncRemoteContainerGeometry();
            appendRoomEvent("已退出焦点全屏");
        }
        return true;
    }

    if (event->type() == QEvent::MouseButtonDblClick) {
        bool ok = false;
        const int idx = watched->property("tileIndex").toInt(&ok);
        if (ok && idx >= 0 && idx < remoteTiles.size()) {
            const QString stream = remoteTiles[idx].stream;
            if (stream.isEmpty()) return true;

            const MemberState st = memberStates.value(stream);
            if (!st.pub) {
                appendRoomEvent("该成员未推流，无法拉取");
                return true;
            }

            if (focusMode && focusedStream == stream) {
                focusMode = false;
                focusedStream.clear();
                currentRemoteStream.clear();
                if (focusPreviewFullScreen) {
                    focusPreviewFullScreen = false;
                    showNormal();
                    if (roomDock) roomDock->show();
                    if (auto *topBar = findChild<QWidget*>("topBarFrame")) topBar->show();
                    if (auto *bottomBar = findChild<QWidget*>("bottomControlFrame")) bottomBar->show();
                    if (auto *localSide = findChild<QWidget*>("localSideFrame")) localSide->show();
                    if (menuBar()) menuBar()->show();
                    if (statusBar()) statusBar()->show();
                    syncRemoteContainerGeometry();
                }
                if (remoteStack && remoteGridPage) {
                    remoteStack->setCurrentWidget(remoteGridPage);
                }
                applyFocusAudioRouting();
                appendRoomEvent("已退出聚焦视图");
                refreshRemoteTiles();
                return true;
            }

            focusedStream = stream;
            focusMode = true;
            preferredRemoteStream = stream;
            if (remoteStack && ui && ui->remoteVideolabel) {
                remoteStack->setCurrentWidget(ui->remoteVideolabel);
            }
            startPullStream(stream);
            appendRoomEvent(QString("聚焦成员: %1").arg(stream));
            refreshRemoteTiles();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// 初始化远端宫格与焦点预览页面
void MainWindow::setupRemoteGridUi()
{
    if (!ui || !ui->centralwidget || !ui->remoteVideolabel) return;
    if (remoteContainer || remoteStack) return;

    QWidget *hostParent = findChild<QWidget*>("remoteStageFrame");
    if (!hostParent) hostParent = ui->centralwidget;
    remoteContainer = new QWidget(hostParent);
    remoteContainer->setObjectName("remoteContainer");
    syncRemoteContainerGeometry();

    remoteStack = new QStackedLayout(remoteContainer);
    remoteStack->setContentsMargins(0, 0, 0, 0);
    remoteStack->setSpacing(0);

    remoteGridPage = new QWidget(remoteContainer);
    remoteGridLayout = new QGridLayout(remoteGridPage);
    remoteGridLayout->setContentsMargins(2, 2, 2, 2);
    remoteGridLayout->setHorizontalSpacing(6);
    remoteGridLayout->setVerticalSpacing(6);

    constexpr int kTileCount = 9;   // 3x3 宫格，满足 2~6 人并预留扩展
    constexpr int kTileColumns = 3;
    remoteTiles.clear();
    remoteTiles.reserve(kTileCount);

    for (int i = 0; i < kTileCount; ++i) {
        RemoteTile tile;
        tile.frame = new QFrame(remoteGridPage);
        tile.frame->setFrameShape(QFrame::StyledPanel);
        tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
        tile.frame->setProperty("tileIndex", i);
        tile.frame->installEventFilter(this);

        auto *vbox = new QVBoxLayout(tile.frame);
        vbox->setContentsMargins(6, 6, 6, 6);
        vbox->setSpacing(4);

        tile.videoLabel = new QLabel("空席位", tile.frame);
        tile.videoLabel->setAlignment(Qt::AlignCenter);
        tile.videoLabel->setMinimumSize(120, 80);
        tile.videoLabel->setStyleSheet("QLabel{background:#1a1a1a;color:#d0d0d0;}");
        tile.videoLabel->setProperty("tileIndex", i);
        tile.videoLabel->installEventFilter(this);

        tile.cornerBadge = new QLabel(tile.frame);
        tile.cornerBadge->setTextFormat(Qt::RichText);
        tile.cornerBadge->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        tile.cornerBadge->setStyleSheet("QLabel{background:transparent;color:#f0f4ff;font-size:11px;}");
        tile.cornerBadge->hide();

        tile.nameLabel = new QLabel("", tile.frame);
        tile.nameLabel->setStyleSheet("QLabel{color:#f0f0f0;font-weight:600;}");
        tile.stateLabel = new QLabel("", tile.frame);
        tile.stateLabel->setStyleSheet("QLabel{color:#a0a0a0;}");

        vbox->addWidget(tile.videoLabel, 1);
        vbox->addWidget(tile.nameLabel);
        vbox->addWidget(tile.stateLabel);
        remoteGridLayout->addWidget(tile.frame, i / kTileColumns, i % kTileColumns);

        remoteTiles.push_back(tile);
    }

    if (QWidget *oldParent = ui->remoteVideolabel->parentWidget()) {
        if (QLayout *oldLayout = oldParent->layout()) {
            oldLayout->removeWidget(ui->remoteVideolabel);
        }
    }
    ui->remoteVideolabel->setParent(remoteContainer);
    ui->remoteVideolabel->setMinimumSize(0, 0);
    ui->remoteVideolabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    ui->remoteVideolabel->setScaledContents(false);
    ui->remoteVideolabel->setAlignment(Qt::AlignCenter);
    ui->remoteVideolabel->setStyleSheet("QLabel{background:black;color:#d0d0d0;}");
    ui->remoteVideolabel->installEventFilter(this);

    remoteStack->addWidget(remoteGridPage);
    remoteStack->addWidget(ui->remoteVideolabel);
    remoteStack->setCurrentWidget(remoteGridPage);

    focusStatusLabel = new QLabel(remoteContainer);
    focusStatusLabel->setObjectName("focusStatusLabel");
    focusStatusLabel->setStyleSheet("QLabel{background:rgba(0,0,0,160);color:#f0f0f0;padding:4px 8px;border-radius:4px;font-weight:600;}");
    focusStatusLabel->move(10, 10);
    focusStatusLabel->hide();
    focusStatusLabel->raise();

    remoteContainer->installEventFilter(this);
    hostParent->installEventFilter(this);
    remoteContainer->show();
}

// 同步远端展示容器在主界面的几何
void MainWindow::syncRemoteContainerGeometry()
{
    if (!remoteContainer || !ui) return;

    QWidget *targetParent = nullptr;
    QRect targetRect;
    if (focusPreviewFullScreen && ui->centralwidget) {
        targetParent = ui->centralwidget;
        targetRect = ui->centralwidget->rect();
    } else {
        QWidget *stage = findChild<QWidget*>("remoteStageFrame");
        if (!stage) {
            stage = ui->centralwidget;
        }
        targetParent = stage;
        targetRect = stage->rect().adjusted(8, 8, -8, -8);
    }

    if (!targetParent) return;
    if (remoteContainer->parentWidget() != targetParent) {
        remoteContainer->setParent(targetParent);
    }
    remoteContainer->setGeometry(targetRect);
    remoteContainer->show();
    remoteContainer->raise();
}

// 根据成员状态刷新远端宫格卡片内容
void MainWindow::refreshRemoteTiles()
{
    if (remoteTiles.isEmpty()) return;

    QStringList streams = memberStates.keys();
    std::sort(streams.begin(), streams.end(), [this](const QString &a, const QString &b) {
        const MemberState sa = memberStates.value(a);
        const MemberState sb = memberStates.value(b);
        const int ra = sa.host ? 2 : (sa.cohost ? 1 : 0);
        const int rb = sb.host ? 2 : (sb.cohost ? 1 : 0);
        if (ra != rb) return ra > rb;
        return a < b;
    });

    int displayCount = qMin(streams.size(), remoteTiles.size());
    int columns = 3;
    if (displayCount <= 1) columns = 1;
    else if (displayCount <= 4) columns = 2;
    const int rows = qMax(1, (displayCount + columns - 1) / columns);

    if (remoteGridLayout) {
        while (QLayoutItem *item = remoteGridLayout->takeAt(0)) {
            (void)item;
        }
        for (int i = 0; i < remoteTiles.size(); ++i) {
            auto &tile = remoteTiles[i];
            if (!tile.frame) continue;

            if (i < displayCount) {
                const int row = i / columns;
                const int col = i % columns;
                remoteGridLayout->addWidget(tile.frame, row, col);
                tile.frame->show();
            } else {
                tile.frame->hide();
            }
        }
        for (int c = 0; c < 3; ++c) {
            remoteGridLayout->setColumnStretch(c, c < columns ? 1 : 0);
        }
        for (int r = 0; r < 3; ++r) {
            remoteGridLayout->setRowStretch(r, r < rows ? 1 : 0);
        }
    }

    streamToTile.clear();
    for (int i = 0; i < remoteTiles.size(); ++i) {
        auto &tile = remoteTiles[i];
        const QString oldStream = tile.stream;
        tile.stream.clear();

        if (i >= streams.size()) {
            tile.hasFrame = false;
            tile.videoLabel->setPixmap(QPixmap());
            tile.videoLabel->setText("空席位");
            tile.nameLabel->clear();
            tile.stateLabel->clear();
            if (tile.cornerBadge) tile.cornerBadge->hide();
            tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
            continue;
        }

        const QString stream = streams[i];
        const MemberState st = memberStates.value(stream);
        tile.stream = stream;
        streamToTile.insert(stream, i);

        QString name = stream;
        if (st.host) name += " [主持人]";
        else if (st.cohost) name += " [联席主持人]";
        tile.nameLabel->setText(name);

        QStringList flags;
        if (!st.pub) flags << "未推流";
        tile.stateLabel->setText(flags.isEmpty() ? "双击聚焦" : flags.join(" | "));

        if(st.share) flags<<"共享中";

        auto chip = [](const QString &txt, const QString &bg) {
            return QString("<span style=\"background:%1;color:#ffffff;padding:1px 5px;border-radius:7px;\">%2</span>")
                .arg(bg, txt);
        };
        if (tile.cornerBadge) {
            QStringList chips;
            if (st.host) chips << chip("主", "#f59f00");
            else if (st.cohost) chips << chip("管", "#1971c2");
            chips << chip("麦", st.audio ? "#2f9e44" : "#c92a2a");
            chips << chip("摄", st.video ? "#2f9e44" : "#c92a2a");
            if (!st.pub) chips << chip("停", "#6c757d");
            if(st.share) chips<<chip("享","#2b8a3e");
            tile.cornerBadge->setText(chips.join(" "));
            tile.cornerBadge->adjustSize();
            tile.cornerBadge->move(tile.frame->width() - tile.cornerBadge->width() - 8, 8);
            tile.cornerBadge->show();
            tile.cornerBadge->raise();
        }

        if (oldStream != stream) {
            tile.hasFrame = false;
            tile.videoLabel->setPixmap(QPixmap());
        }
        if (!tile.hasFrame) {
            tile.videoLabel->setText(st.pub ? "双击聚焦并拉流" : "未推流");
        } else {
            tile.videoLabel->setText("");
        }

        const bool focused = focusMode && focusedStream == stream;
        if (focused) {
            tile.frame->setStyleSheet("QFrame{background:#101010;border:2px solid #2e86de;border-radius:6px;}");
        } else {
            tile.frame->setStyleSheet("QFrame{background:#101010;border:1px solid #4a4a4a;border-radius:6px;}");
        }
    }

    updateFocusStatusBadge();

    if (remoteStack) {
        if (focusMode && ui && ui->remoteVideolabel) {
            remoteStack->setCurrentWidget(ui->remoteVideolabel);
        } else if (remoteGridPage) {
            remoteStack->setCurrentWidget(remoteGridPage);
        }
    }
}

// 将指定流的帧渲染到对应宫格卡片
void MainWindow::applyTileFrame(const QString &stream, const QImage &img)
{
    if (stream.isEmpty()) return;
    const int idx = streamToTile.value(stream, -1);
    if (idx < 0 || idx >= remoteTiles.size()) return;

    auto &tile = remoteTiles[idx];
    if (!tile.videoLabel) return;

    tile.videoLabel->setPixmap(
        QPixmap::fromImage(img).scaled(tile.videoLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
    );
    tile.videoLabel->setText("");
    tile.hasFrame = true;
}

// 清空全部宫格画面并恢复占位文本
void MainWindow::clearAllTileFrames()
{
    for (auto &tile : remoteTiles) {
        tile.hasFrame = false;
        if (tile.videoLabel) {
            tile.videoLabel->setPixmap(QPixmap());
            if (tile.stream.isEmpty()) {
                tile.videoLabel->setText("空席位");
            } else {
                const MemberState st = memberStates.value(tile.stream);
                tile.videoLabel->setText(st.pub ? "双击聚焦并拉流" : "未推流");
            }
        }
        if (tile.cornerBadge) {
            if (tile.stream.isEmpty()) tile.cornerBadge->hide();
            else tile.cornerBadge->show();
        }
    }
    updateFocusStatusBadge();
}

// 更新焦点状态浮层文本与显示状态
void MainWindow::updateFocusStatusBadge()
{
    if (!focusStatusLabel) return;

    if (!focusMode || focusedStream.isEmpty()) {
        focusStatusLabel->hide();
        return;
    }

    const MemberState st = memberStates.value(focusedStream);
    QStringList flags;
    flags << QString("焦点: %1").arg(focusedStream);
    flags << (st.audio ? "麦克风开" : "麦克风关");
    flags << (st.video ? "摄像头开" : "摄像头关");
    if (!st.pub) flags << "未推流";
    if(st.share) flags<<"共享中";

    focusStatusLabel->setText(flags.join("  |  "));
    focusStatusLabel->adjustSize();
    focusStatusLabel->move(10, 10);
    focusStatusLabel->show();
    focusStatusLabel->raise();
}

// 确保房间号/用户号/流 ID 已生成
bool MainWindow::ensureRoomIdentity(bool askRoomIfEmpty)
{
    if (roomId.isEmpty() && loginRoomEdit) {
        roomId = loginRoomEdit->text().trimmed();
    }
    if (askRoomIfEmpty && roomId.isEmpty()) {
        showLoginOverlay(true, "请先输入房间号后再连接信令");
        return false;
    }
    if (roomId.isEmpty()) {
        roomId = QString::number(QRandomGenerator::global()->bounded(100000, 999999));
    }
    if (userId.isEmpty()) {
        if (!loginUser.isEmpty()) userId = loginUser;
        else userId = QString("u%1").arg(QRandomGenerator::global()->bounded(1000, 9999));
    }

    selfStream = QString("%1_%2").arg(roomId, userId);
    updateMeetingHeaderUi();
    return true;
}

// 追加系统事件到日志与聊天系统消息
void MainWindow::appendRoomEvent(const QString &text)
{
    const QString msg = QString("[%1] %2")
                            .arg(QDateTime::currentDateTime().toString("hh:mm:ss"), text);
    recentEventLogs.enqueue(msg);
    while (recentEventLogs.size() > 200) {
        recentEventLogs.dequeue();
    }
    if (roomEventLog) {
        roomEventLog->appendPlainText(msg);
    }
    if (chatMessageLog && chatMessageLog != roomEventLog) {
        const QString chatLine = QString("[%1] 系统: %2")
                                     .arg(QDateTime::currentDateTime().toString("hh:mm:ss"), text);
        chatMessageLog->appendPlainText(chatLine);
    }
    qInfo() << msg;
}

// 重建成员列表并联动宫格与拉流
void MainWindow::refreshRoomUserList()
{
    if (!roomUserList) return;

    // 聚焦流离开房间或停止推流时，自动退回宫格，避免主画面停留在失效流
    if (focusMode && !focusedStream.isEmpty()) {
        const bool exists = memberStates.contains(focusedStream);
        const bool isPublishingNow = exists && memberStates.value(focusedStream).pub;
        if (!exists || !isPublishingNow) {
            focusMode = false;
            focusedStream.clear();
            currentRemoteStream.clear();
            if (focusPreviewFullScreen) {
                focusPreviewFullScreen = false;
                showNormal();
                if (roomDock) roomDock->show();
                if (auto *topBar = findChild<QWidget*>("topBarFrame")) topBar->show();
                if (auto *bottomBar = findChild<QWidget*>("bottomControlFrame")) bottomBar->show();
                if (auto *localSide = findChild<QWidget*>("localSideFrame")) localSide->show();
                if (menuBar()) menuBar()->show();
                if (statusBar()) statusBar()->show();
                syncRemoteContainerGeometry();
            }
            if (ui && ui->remoteVideolabel) {
                ui->remoteVideolabel->clear();
            }
            if (remoteStack && remoteGridPage) {
                remoteStack->setCurrentWidget(remoteGridPage);
            }
            appendRoomEvent("聚焦成员已离开或停推，已返回宫格视图");
        }
    }

    QString selectedStream;
    if (auto *cur = roomUserList->currentItem()) {
        selectedStream = cur->data(Qt::UserRole).toString();
    }

    QStringList streams = memberStates.keys();
    std::sort(streams.begin(), streams.end(), [this](const QString &a, const QString &b) {
        const MemberState sa = memberStates.value(a);
        const MemberState sb = memberStates.value(b);
        const int ra = sa.host ? 2 : (sa.cohost ? 1 : 0);
        const int rb = sb.host ? 2 : (sb.cohost ? 1 : 0);
        if (ra != rb) return ra > rb;
        return a < b;
    });

    auto makeStateIcon = [](const MemberState &st) -> QIcon {
        QPixmap pm(66, 18);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);

        auto drawChip = [&p](int x, const QColor &bg, const QString &txt) {
            QRect r(x, 1, 20, 16);
            p.setPen(Qt::NoPen);
            p.setBrush(bg);
            p.drawRoundedRect(r, 7, 7);
            p.setPen(Qt::white);
            QFont f = p.font();
            f.setPointSize(8);
            f.setBold(true);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, txt);
        };

        drawChip(0,  st.pub   ? QColor("#2f9e44") : QColor("#6c757d"), "P");
        drawChip(23, st.audio ? QColor("#2f9e44") : QColor("#c92a2a"), "M");
        drawChip(46, st.video ? QColor("#2f9e44") : QColor("#c92a2a"), "V");

        if (st.host || st.cohost) {
            const QColor roleColor = st.host ? QColor("#ffd43b") : QColor("#4dabf7");
            p.setPen(QPen(roleColor, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(QRect(0, 0, 65, 17), 8, 8);
        }
        return QIcon(pm);
    };

    roomUserList->blockSignals(true);
    roomUserList->clear();
    roomUserList->setIconSize(QSize(66, 18));
    for (const QString &stream : streams) {
        const MemberState st = memberStates.value(stream);
        QString text = stream;
        if (st.host) text += "  ·  主持人";
        else if (st.cohost) text += "  ·  联席主持人";
        else text += "  ·  成员";
        if (!st.pub) text += "  ·  未推流";
        if(st.share) text+="  .  共享中";

        auto *item = new QListWidgetItem(text, roomUserList);
        item->setData(Qt::UserRole, stream);
        item->setIcon(makeStateIcon(st));
        const QString roleText = st.host ? "主持人" : (st.cohost ? "联席主持人" : "成员");
        item->setToolTip(QString("角色: %1\n推流: %2\n麦克风: %3\n摄像头: %4\n共享: %5")
                             .arg(roleText)
                             .arg(st.pub ? "开启" : "关闭")
                             .arg(st.audio ? "开启" : "关闭")
                             .arg(st.video ? "开启" : "关闭")
                             .arg(st.share?"是":"否"));

        if (st.host || st.cohost) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        if (!selectedStream.isEmpty() && stream == selectedStream) {
            roomUserList->setCurrentItem(item);
        }
    }
    roomUserList->blockSignals(false);

    if (roomCountLabel) {
        roomCountLabel->setText(QString("在线: %1").arg(streams.size()));
    }
    applyWhiteboardLockUi();
    refreshRemoteTiles();
    syncGridPullers();
}

// 发送入会消息 join
void MainWindow::sendSignalJoin()
{
    if (!signalAuthed) {
        appendRoomEvent("尚未完成登录，已阻止入会");
        return;
    }
    if (!signalSocket || !signalConnected) return;
    if (!ensureRoomIdentity(false)) return;

    QJsonObject obj;
    obj["type"] = "join";
    obj["room"] = roomId;
    obj["user"] = userId;
    obj["stream"] = selfStream;
    obj["audio"] = localAudioOn;
    obj["video"] = localVideoOn;
    obj["pub"] = isPublishing;
    obj["share"]=localScreenShareOn;

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    appendRoomEvent(QString("已加入房间 %1，用户 %2").arg(roomId, selfStream));
}

// 发送离会消息 leave
void MainWindow::sendSignalLeave()
{
    if (!signalSocket || signalSocket->state() != QAbstractSocket::ConnectedState) return;

    QJsonObject obj;
    obj["type"] = "leave";
    obj["room"] = roomId;
    obj["stream"] = selfStream;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 发送成员状态更新 update
void MainWindow::sendSignalUpdate()
{
    if (!signalSocket || !signalConnected) return;

    QJsonObject obj;
    obj["type"] = "update";
    obj["room"] = roomId;
    obj["stream"] = selfStream;
    obj["audio"] = localAudioOn;
    obj["video"] = localVideoOn;
    obj["pub"] = isPublishing;
    obj["share"]=localScreenShareOn;

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 发送主持控制命令 cmd
void MainWindow::sendSignalCmd(const QString &toStream, const QString &action)
{
    if (!signalSocket || !signalConnected || toStream.isEmpty() || action.isEmpty()) return;

    QJsonObject obj;
    obj["type"] = "cmd";
    obj["room"] = roomId;
    obj["to"] = toStream;
    obj["action"] = action;
    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    qInfo() << "[SignalCmd] room=" << roomId << "to=" << toStream << "action=" << action;
}

// 发送聊天消息 chat
void MainWindow::sendSignalChat(const QString &content)
{
    if(!signalSocket||!signalConnected) return;
    if(content.trimmed().isEmpty()) return;

    QJsonObject obj;
    obj["type"]="chat";
    obj["room"]=roomId;
    obj["user"]=userId;
    obj["stream"]=selfStream;
    obj["content"]=content;

    const qint64 nowMs=QDateTime::currentMSecsSinceEpoch();
    obj["msg_id"]=QString("%1_%2_%3").arg(selfStream).arg(nowMs).arg(++chatLocalSeq);
    obj["ts"]=nowMs ;
    obj["ver"]=1;

    signalSocket->sendTextMessage(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

// 弹出共享源选择并应用到当前会话
void MainWindow::chooseShareSource()
{
    const auto candidates=buildShareSourceCandidates();
    if(candidates.isEmpty()){
        appendRoomEvent("未发现可共享源");
        return;
    }

    QStringList labels;
    int currentIndex=0;
    for(int i=0;i<candidates.size();++i){
        labels<<candidates[i].label;
        const bool match=candidates[i].isWindow
                            ?(shareWindowId!=0&&candidates[i].windowId==shareWindowId)
                            :(shareWindowId==0&&candidates[i].screenIndex==shareScreenIndex);
        if(match) currentIndex=i;
    }

    bool ok=false;
    const QString picked=QInputDialog::getItem(this,"选择共享源","请选择共享对象:",labels,currentIndex,false,&ok);
    if(!ok||picked.isEmpty()) return;

    const int idx=labels.indexOf(picked);
    if(idx<0) return;

    const auto &sel=candidates[idx];
    shareWindowId=sel.windowId;
    shareScreenIndex=sel.screenIndex;
    shareSourceName=sel.label;

    appendRoomEvent(QString("共享源已切换：%1").arg(shareSourceName));

    if(localScreenShareOn){
        applyLocalVideoSource();//共享中切源即时生效
    }
    refreshSelfControlActions();
}

// 构建可共享的屏幕/窗口候选列表
QVector<MainWindow::ShareSourceCandidate> MainWindow::buildShareSourceCandidates() const
{
    QVector<ShareSourceCandidate> out;

    const auto screens=QGuiApplication::screens();
    for(int i=0;i<screens.size();++i){
        ShareSourceCandidate c;
        c.isWindow=false;
        c.screenIndex=i;
        c.windowId=0;
        c.label=QString("屏幕%1 (%2x%3) ").arg(i+1).arg(screens[i]->size().width()).arg(screens[i]->size().height());
        out.push_back(c);
    }

#ifdef Q_OS_WIN
    QVector<WinShareEntry> winList;
    EnumWindows(enumShareWindowProc,reinterpret_cast<LPARAM>(&winList));
    QSet<quint64> seen;
    for(const auto &w:winList){
        if(seen.contains(w.hwnd)) continue;
        seen.insert(w.hwnd);
        ShareSourceCandidate c;
        c.isWindow=true;
        c.windowId=w.hwnd;
        c.label=QString("窗口：%1").arg(w.title);
        out.push_back(c);
    }
#endif
    return out;
}

// 追加聊天消息并进行 msg_id 去重
void MainWindow::appendChatMessage(const QString &fromStream, const QString &content, qint64 tsMs, bool isSelf, const QString &msgId)
{
    if(content.trimmed().isEmpty()) return;

    //消息去重，同一个msg_id只显示一次
    if(!msgId.isEmpty()){
        if(seenChatMsgIds.contains(msgId)) return;
        seenChatMsgIds.insert(msgId);
        if(seenChatMsgIds.size()>1000){
            seenChatMsgIds.clear();//简单限界，避免集合无限增长
        }
    }

    const QString timeText=
        (tsMs>0)
        ? QDateTime::fromMSecsSinceEpoch(tsMs).toString("hh:mm:ss")
        : QDateTime::currentDateTime().toString("hh:mm:ss");

    const QString who=isSelf ? QStringLiteral("我"):fromStream;
    const QString line=QString("[%1] %2: %3").arg(timeText,who,content);

    if(chatMessageLog){
        chatMessageLog->appendPlainText(line);
    }
    qInfo()<<"[chat]"<<line;
}

// 信令连接成功后的登录/状态同步入口
void MainWindow::onSignalConnected()
{
    if (shuttingDown) return;
    const int reconnectSnapshot = signalReconnectAttempt;
    manualSignalDisconnect = false;
    resetSignalReconnectState();

    signalConnected = true;
    signalAuthed = false;
    authRegisterTried = pendingAuthRegister;
    if (signalStateLabel) signalStateLabel->setText("信令: 已连接");
    updateMeetingHeaderUi();
    updatePrimaryActionTexts();

    if (reconnectSnapshot > 0) {
        ++totalSignalReconnectCount;
        appendRoomEvent(QString("信令重连成功（第 %1 次）").arg(reconnectSnapshot));
    }
    appendRoomEvent(pendingAuthRegister ? "信令已连接，正在注册..." : "信令已连接，正在登录...");
    if (pendingAuthRegister) {
        sendSignalAuthRegister();
    } else {
        sendSignalAuthLogin();
    }
    refreshSelfControlActions();
    updateMeetingStatsUi();
}

// 信令断开后的状态清理与重连调度
void MainWindow::onSignalDisconnected()
{
    signalAuthed = false;
    authRegisterTried = false;
    signalConnected = false;
    if (shuttingDown) return;
    if (signalStateLabel) signalStateLabel->setText("信令: 未连接");
    updateMeetingHeaderUi();
    updatePrimaryActionTexts();
    appendRoomEvent("信令已断开");

    // 手动断开信令时，立即停止当前拉流，避免继续播放远端。
    stopCurrentPull(true);
    focusedStream.clear();
    focusMode = false;
    clearAllTileFrames();
    if (remoteStack && remoteGridPage) {
        remoteStack->setCurrentWidget(remoteGridPage);
    }
    if (ui && ui->remoteVideolabel) {
        ui->remoteVideolabel->clear();
    }
    memberStates.clear();
    roomHostStream.clear();
    refreshRoomUserList();
    refreshSelfControlActions();
    seenChatMsgIds.clear();
    if(chatInputEdit) chatInputEdit->clear();
    seenWhiteboardMsgIds.clear();
    whiteboardMouseDown=false;
    clearWhiteboard(false);

    if (!manualSignalDisconnect && !meetingStopped && !shuttingDown) {
        scheduleSignalReconnect("检测到信令断开");
    }
    updateMeetingStatsUi();
}

// 信令消息分发中心（认证/成员/控制/聊天/白板）
void MainWindow::onSignalTextMessage(const QString &msg)
{
    if (shuttingDown) return;
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    const QJsonObject obj = doc.object();
    const QString type = obj.value("type").toString();

    if (type == "auth_ok") {
        signalAuthed = true;
        authRegisterTried = false;
        pendingAuthRegister = false;
        loginUser = obj.value("user").toString(loginUser);
        if (!loginUser.isEmpty()) userId = loginUser;
        appendRoomEvent(QString("登录成功: %1").arg(loginUser));
        showLoginOverlay(false);
        sendSignalJoin();
        return;
    }

    if (type == "auth_registered") {
        appendRoomEvent("账号注册成功，正在登录...");
        pendingAuthRegister = false;
        sendSignalAuthLogin();
        return;
    }

    if (type == "auth_fail") {
        const QString code = obj.value("code").toString();
        const QString msgText = obj.value("msg").toString("登录失败");
        appendRoomEvent(QString("登录失败[%1]: %2").arg(code, msgText));
        pendingAuthRegister = false;
        signalAuthed = false;
        QString hint;
        if (code == "no_user") {
            hint = "账号不存在，请点击“注册并进入”";
        } else if (code == "bad_password") {
            hint = "密码错误，请检查后重试";
        } else if (code == "exists") {
            hint = "账号已存在，请点击“登录并进入”";
        } else if (code == "bad_args") {
            hint = "账号或密码格式不正确";
        } else if (code == "db_error") {
            hint = "服务端数据库异常，请稍后重试";
        } else {
            hint = QString("登录失败：%1").arg(msgText);
        }
        showLoginOverlay(true, hint);
        return;
    }

    if (type == "auth_required") {
        signalAuthed = false;
        appendRoomEvent("服务端要求先登录");
        showLoginOverlay(true, "服务端要求先登录");
        return;
    }


    if(type=="chat"){
        const QString room=obj.value("room").toString();
        if(!roomId.isEmpty()&&room!=roomId) return;

        const QString fromStream=obj.value("stream").toString();
        const QString content=obj.value("content").toString();
        const QString msgId=obj.value("msg_id").toString();
        const qint64 tsMs=obj.value("ts").toVariant().toLongLong();

        if(fromStream.isEmpty()||content.trimmed().isEmpty()) return;

        appendChatMessage(fromStream,content,tsMs,fromStream==selfStream,msgId);
        return;
    }

    if(type=="wb"){
        const QString room=obj.value("room").toString();
        if(!roomId.isEmpty()&&room!=roomId) return;

        const QString fromStream=obj.value("stream").toString();
        if(fromStream.isEmpty()) return;
        if(fromStream==selfStream) return;//自己发的本地已画，避免重复

        const QString msgId=obj.value("msg_id").toString();
        if(!msgId.isEmpty()){
            if(seenWhiteboardMsgIds.contains(msgId)) return;
            seenWhiteboardMsgIds.insert(msgId);
            if(seenWhiteboardMsgIds.size()>3000) seenWhiteboardMsgIds.clear();
        }

        const QString op=obj.value("op").toString();
        if(op=="lock"||op=="unlock"){
            whiteboardLocked=(op=="lock");
            applyWhiteboardLockUi();
            appendRoomEvent(whiteboardLocked
                            ? QString("%1 锁定了白板").arg(fromStream)
                            : QString("%1 解锁了白板").arg(fromStream));
            return;
        }
        if(op=="clear"){
            clearWhiteboard(false,fromStream);
            return;
        }
        if(op=="draw"){
            ensureWhiteboardCanvas();
            if(whiteboardCanvas.isNull()) return;

            const int w=qMax(1,whiteboardCanvas.width()-1);
            const int h=qMax(1,whiteboardCanvas.height()-1);

            auto denormX=[&](int n){ return qBound(0,(n*w)/10000,w);};
            auto denormY=[&](int n){ return qBound(0,(n*h)/10000,h);};

            const QPoint p1(denormX(obj.value("x1n").toInt()),denormY(obj.value("y1n").toInt()));
            const QPoint p2(denormX(obj.value("x2n").toInt()),denormY(obj.value("y2n").toInt()));

            const QColor c(obj.value("color").toString("#e03131"));
            const int pw=qBound(1,obj.value("pw").toInt(3),12);
            const QString sid=obj.value("stroke_id").toString(obj.value("msg_id").toString());

            drawWhiteboardLine(p1,p2,false,c,pw,sid,fromStream);
            return;
        }
        if(op=="undo"){
            const QString sid=obj.value("stroke_id").toString();
            removeStrokeById(sid,fromStream);
            return;
        }
    }
    if (type == "members") {
        const QString room = obj.value("room").toString();
        if (!roomId.isEmpty() && room != roomId) return;

        const bool serverWbLock=obj.value("wb_lock").toBool(false);
        if(whiteboardLocked!=serverWbLock){
            whiteboardLocked=serverWbLock;
            applyWhiteboardLockUi();
        }

        const bool oldSelfHost = memberStates.value(selfStream).host;
        const bool oldSelfCoHost = memberStates.value(selfStream).cohost;
        QHash<QString, MemberState> newStates;
        roomHostStream.clear();

        const QJsonArray arr = obj.value("members").toArray();
        for (const QJsonValue &v : arr) {
            if (!v.isObject()) continue;
            const QJsonObject m = v.toObject();
            MemberState st;
            st.user = m.value("user").toString();
            st.stream = m.value("stream").toString();
            st.audio = m.value("audio").toBool(true);
            st.video = m.value("video").toBool(true);
            st.pub = m.value("pub").toBool(false);
            st.share=m.value("share").toBool(false);
            const QString role = m.value("role").toString();
            st.host = (role == "host");
            st.cohost = (role == "cohost");
            if (st.stream.isEmpty()) continue;
            if (st.host) roomHostStream = st.stream;
            newStates.insert(st.stream, st);
        }

        if (roomHostStream.isEmpty() && !newStates.isEmpty()) {
            QStringList streams = newStates.keys();
            std::sort(streams.begin(), streams.end());
            roomHostStream = streams.first();
            newStates[roomHostStream].host = true;
        }

        memberStates = newStates;
        refreshRoomUserList();

        if (memberStates.contains(selfStream)) {
            const MemberState selfState = memberStates.value(selfStream);
            const bool oldAudio = localAudioOn;
            const bool oldVideo = localVideoOn;
            localAudioOn = selfState.audio;
            localVideoOn = selfState.video;

            if (oldAudio != localAudioOn) {
                appendRoomEvent(localAudioOn ? "麦克风已恢复" : "主持人已将你静音");
            }
            if (oldVideo != localVideoOn) {
                appendRoomEvent(localVideoOn ? "摄像头已开启" : "主持人已关闭你的摄像头");
            }
            const bool oldShare=localScreenShareOn;
            localScreenShareOn=selfState.share;
            if(oldShare!=localScreenShareOn){
                appendRoomEvent(localScreenShareOn?"屏幕共享已开启":"屏幕共享已停止");
                applyLocalVideoSource();
            }
            if (!oldSelfHost && selfState.host) {
                appendRoomEvent("你已成为主持人");
            } else if (oldSelfHost && !selfState.host) {
                appendRoomEvent("你的主持人身份已转移");
            }
            if (!oldSelfCoHost && selfState.cohost) {
                appendRoomEvent("你已成为联席主持人");
            } else if (oldSelfCoHost && !selfState.cohost && !selfState.host) {
                appendRoomEvent("你的联席主持人身份已取消");
            }
            if (oldAudio != localAudioOn || oldVideo != localVideoOn||oldShare!=localScreenShareOn) {
                refreshSelfControlActions();
            }
        }

        // 不在 members 刷新里主动 stopCurrentPull()，避免状态瞬时抖动触发误停拉流。
        // 拉流切换仅由用户双击、断开信令、结束会议这三条路径控制。
        return;
    }

    if (type == "ctrl") {
        const QString to = obj.value("to").toString();
        const QString action = obj.value("action").toString();
        if (to != selfStream) return;

        if (action == "mute_audio") {
            localAudioOn = false;
            appendRoomEvent("主持人已将你静音");
            refreshSelfControlActions();
            return;
        }
        if (action == "mute_video") {
            localVideoOn = false;
            appendRoomEvent("主持人已关闭你的摄像头");
            refreshSelfControlActions();
            return;
        }
        if (action == "unmute_audio" || action == "restore_audio" || action == "audio_on") {
            localAudioOn = true;
            appendRoomEvent("主持人已恢复你的麦克风");
            refreshSelfControlActions();
            return;
        }
        if (action == "unmute_video" || action == "restore_video" || action == "video_on") {
            localVideoOn = true;
            appendRoomEvent("主持人已开启你的摄像头");
            refreshSelfControlActions();
            return;
        }
        if (action == "set_host") {
            appendRoomEvent("你被设为主持人");
            return;
        }
        if (action == "set_cohost") {
            appendRoomEvent("你被设为联席主持人");
            return;
        }
        if (action == "unset_cohost") {
            appendRoomEvent("你的联席主持人身份被取消");
            return;
        }
        if (action == "kick") {
            appendRoomEvent("你已被主持人移出会议");
            on_stopMeetingButton_clicked();
            return;
        }
        if(action=="stop_share"){
            localScreenShareOn=false;
            applyLocalVideoSource();
            appendRoomEvent("主持人已停止你的屏幕共享");
            sendSignalUpdate();
            refreshSelfControlActions();
            return;
        }
    }
}

// 双击成员列表项：进入焦点并拉流
void MainWindow::onRoomUserDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    const QString stream = item->data(Qt::UserRole).toString();
    if (stream.isEmpty()) return;
    const MemberState st = memberStates.value(stream);
    if (!st.pub) {
        appendRoomEvent("该成员未推流，无法拉取");
        return;
    }
    focusMode = true;
    focusedStream = stream;
    if (remoteStack && ui && ui->remoteVideolabel) {
        remoteStack->setCurrentWidget(ui->remoteVideolabel);
    }
    preferredRemoteStream = stream;
    startPullStream(stream);
    refreshRemoteTiles();
}

// 成员右键菜单：主持控制与全体管理
void MainWindow::onRoomListContextMenu(const QPoint &pos)
{
    if (!roomUserList) return;
    QListWidgetItem *item = roomUserList->itemAt(pos);
    if (!item) return;

    const QString targetStream = item->data(Qt::UserRole).toString();
    if (targetStream.isEmpty()) return;
    const MemberState selfState = memberStates.value(selfStream);
    const bool selfIsHost = selfState.host;
    const bool selfIsCoHost = selfState.cohost;

    QMenu menu(this);
    QAction *picked = nullptr;

    if (targetStream == selfStream) {
        QAction *selfAudioToggle = menu.addAction(localAudioOn ? "静音我自己" : "恢复我自己的麦克风");
        QAction *selfVideoToggle = menu.addAction(localVideoOn ? "关闭我自己的摄像头" : "开启我自己的摄像头");

        QAction *allMuteAudio = nullptr;
        QAction *allUnmuteAudio = nullptr;
        QAction *allMuteVideo = nullptr;
        QAction *allUnmuteVideo = nullptr;
        QAction *allStopShare=nullptr;

        if (selfIsHost) {
            menu.addSeparator();
            allMuteAudio = menu.addAction("全体静音（不含自己）");
            allUnmuteAudio = menu.addAction("全体恢复麦克风（不含自己）");
            menu.addSeparator();
            allMuteVideo = menu.addAction("全体关闭摄像头（不含自己）");
            allUnmuteVideo = menu.addAction("全体开启摄像头（不含自己）");
            allStopShare=menu.addAction("全体停止共享(不含自己");
        }

        picked = menu.exec(roomUserList->viewport()->mapToGlobal(pos));
        if (!picked) return;

        if (picked == selfAudioToggle) {
            localAudioOn = !localAudioOn;
            sendSignalUpdate();
            appendRoomEvent(localAudioOn ? "你已恢复麦克风" : "你已静音自己");
            refreshSelfControlActions();
            return;
        }
        if (picked == selfVideoToggle) {
            localVideoOn = !localVideoOn;
            sendSignalUpdate();
            appendRoomEvent(localVideoOn ? "你已开启摄像头" : "你已关闭自己的摄像头");
            refreshSelfControlActions();
            return;
        }

        if (!selfIsHost) return;

        auto sendToAll = [this](const QString &action, const QString &eventText) {
            int sent = 0;
            for (auto it = memberStates.cbegin(); it != memberStates.cend(); ++it) {
                const QString s = it.key();
                if (s.isEmpty() || s == selfStream) continue;
                sendSignalCmd(s, action);
                ++sent;
            }
            appendRoomEvent(QString("%1，已发送 %2 人").arg(eventText).arg(sent));
        };

        if (picked == allMuteAudio) {
            sendToAll("mute_audio", "主持人执行全体静音");
        } else if (picked == allUnmuteAudio) {
            sendToAll("unmute_audio", "主持人执行全体恢复麦克风");
        } else if (picked == allMuteVideo) {
            sendToAll("mute_video", "主持人执行全体关闭摄像头");
        } else if (picked == allUnmuteVideo) {
            sendToAll("unmute_video", "主持人执行全体开启摄像头");
        }else if(picked==allStopShare){
            sendToAll("stop_share","主持人执行全体停止共享");
        }
        return;
    }

    if (!(selfIsHost || selfIsCoHost)) {
        appendRoomEvent("仅主持人/联席主持人可管理其他成员");
        return;
    }

    const MemberState targetState = memberStates.value(targetStream);
    if (!selfIsHost && targetState.host) {
        appendRoomEvent("联席主持人不可管理主持人");
        return;
    }

    QAction *muteAudio = menu.addAction("静音该成员");
    QAction *unmuteAudio = menu.addAction("恢复该成员麦克风");
    menu.addSeparator();
    QAction *muteVideo = menu.addAction("关闭该成员摄像头");
    QAction *unmuteVideo = menu.addAction("开启该成员摄像头");
    menu.addSeparator();
    QAction *kick = menu.addAction("踢出该成员");

    QAction *setCoHost = nullptr;
    QAction *unsetCoHost = nullptr;
    QAction *transferHost = nullptr;
    QAction *stopShare=nullptr;
    if (selfIsHost) {
        menu.addSeparator();
        if (!targetState.host && !targetState.cohost) {
            setCoHost = menu.addAction("邀请为联席主持人");
        } else if (targetState.cohost) {
            unsetCoHost = menu.addAction("取消联席主持人");
        }
        if (!targetState.host) {
            transferHost = menu.addAction("转移主持人给该成员");
        }
        if(targetState.share){
            menu.addSeparator();
            stopShare=menu.addAction("停止该成员共享");
        }
    }

    picked = menu.exec(roomUserList->viewport()->mapToGlobal(pos));
    if (!picked) return;

    if (picked == muteAudio) {
        sendSignalCmd(targetStream, "mute_audio");
        appendRoomEvent(QString("已对 %1 发送静音").arg(targetStream));
    } else if (picked == unmuteAudio) {
        sendSignalCmd(targetStream, "unmute_audio");
        appendRoomEvent(QString("已对 %1 发送恢复麦克风").arg(targetStream));
    } else if (picked == muteVideo) {
        sendSignalCmd(targetStream, "mute_video");
        appendRoomEvent(QString("已对 %1 发送关闭摄像头").arg(targetStream));
    } else if (picked == unmuteVideo) {
        sendSignalCmd(targetStream, "unmute_video");
        appendRoomEvent(QString("已对 %1 发送开启摄像头").arg(targetStream));
    } else if (picked == kick) {
        sendSignalCmd(targetStream, "kick");
        appendRoomEvent(QString("已踢出 %1").arg(targetStream));
    } else if (picked == setCoHost) {
        sendSignalCmd(targetStream, "set_cohost");
        appendRoomEvent(QString("已邀请 %1 为联席主持人").arg(targetStream));
    } else if (picked == unsetCoHost) {
        sendSignalCmd(targetStream, "unset_cohost");
        appendRoomEvent(QString("已取消 %1 的联席主持人身份").arg(targetStream));
    } else if (picked == transferHost) {
        const auto ret = QMessageBox::question(
            this, "转移主持人",
            QString("确认将主持人转移给 %1 ？").arg(targetStream)
        );
        if (ret == QMessageBox::Yes) {
            sendSignalCmd(targetStream, "set_host");
            appendRoomEvent(QString("已将主持人转移给 %1").arg(targetStream));
        }
    }else if(picked==stopShare){
        sendSignalCmd(targetStream,"stop_share");
        appendRoomEvent(QString("已对%1发送停止共享").arg(targetStream));
    }
}

// 发送聊天输入框内容
void MainWindow::onSendChatClicked()
{
    if(!chatInputEdit) return;

    if(!signalConnected||!signalSocket||signalSocket->state()!=QAbstractSocket::ConnectedState){
        appendRoomEvent("信令未连接,无法发送聊天消息");
        return;
    }

    if(!ensureRoomIdentity(false)) return;

    QString content=chatInputEdit->toPlainText().trimmed();
    if(content.isEmpty()) return;

    //先做硬限制,避免超长消息刷屏
    if(content.size()>500){
        content=content.left(500);
    }

    sendSignalChat(content);
    chatInputEdit->clear();
}

// 获取当前应保持拉流显示的 stream 列表
QStringList MainWindow::currentDisplayStreams() const
{
    QStringList streams;
    for (const auto &tile : remoteTiles) {
        if (tile.stream.isEmpty()) continue;
        const MemberState st = memberStates.value(tile.stream);
        if (st.pub) streams.push_back(tile.stream);
    }
    return streams;
}

// 应用焦点音频路由（仅焦点流出声）
void MainWindow::applyFocusAudioRouting()
{
    for (auto it = pullSessions.begin(); it != pullSessions.end(); ++it) {
        PullSession *sess = it.value();
        if (!sess || !sess->puller) continue;
        const bool enable = focusMode && !focusedStream.isEmpty() && sess->stream == focusedStream;
        QMetaObject::invokeMethod(sess->puller, "setAudioEnabled", Qt::QueuedConnection, Q_ARG(bool, enable));
    }
}

// 确保目标流拉流会话存在并启动
void MainWindow::ensurePullSession(const QString &stream)
{
    if (stream.isEmpty()) return;
    if (pullSessions.contains(stream)) return;

    PullSession *sess = new PullSession;
    sess->stream = stream;
    sess->puller = new rtmppuller(nullptr);
    sess->thread = new QThread(this);
    sess->thread->setObjectName(QString("pullThread_%1").arg(stream));
    sess->puller->moveToThread(sess->thread);
    pullSessions.insert(stream, sess);

    connect(sess->puller, &rtmppuller::finished, sess->thread, &QThread::quit, Qt::QueuedConnection);
    connect(sess->puller, &rtmppuller::videoFrameReady, this, [this, stream](const QImage &img) {
        if (PullSession *s = pullSessions.value(stream, nullptr)) {
            s->retryCount = 0;
            s->retryEpoch = 0;
        }
        latestRemoteFrames.insert(stream, img);
        applyTileFrame(stream, img);
        if (focusMode && focusedStream == stream && ui && ui->remoteVideolabel) {
            ui->remoteVideolabel->setPixmap(
                QPixmap::fromImage(img).scaled(ui->remoteVideolabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation)
            );
        }
        if (camRecording && recorder && recorder->isOpen()) {
            const bool useRemoteRecord = (!isPublishing || !videoWorker);
            if (useRemoteRecord && focusMode && focusedStream == stream) {
                recorder->pushVideoFrame(img);
            }
        }
    });
    connect(sess->puller, &rtmppuller::audioPcmReady, this, [this, stream](const QByteArray &pcm, int sampleRate, int channels) {
        Q_UNUSED(sampleRate);
        Q_UNUSED(channels);
        if (!(camRecording && recorder && recorder->isOpen())) return;
        const bool useRemoteRecord = (!isPublishing || !audioWorker);
        if (!useRemoteRecord) return;
        if (!(focusMode && focusedStream == stream)) return;
        if (pcm.isEmpty()) return;
        const int nbSamples = pcm.size() / 2; // mono s16
        if (nbSamples <= 0) return;
        recorder->pushAudioPCM(reinterpret_cast<const uint8_t*>(pcm.constData()), nbSamples);
    });
    connect(sess->puller, &rtmppuller::errorOccurred, this, [this, stream](const QString &e) {
        PullSession *s = pullSessions.value(stream, nullptr);
        if (!s) return;
        appendRoomEvent(QString("拉流错误(%1): %2").arg(stream, e));

        const QString errLower = e.toLower();
        const bool nonNetwork =
            errLower.contains("operation not permitted") ||
            errLower.contains("no such stream") ||
            errLower.contains("immediate exit requested");
        if (!nonNetwork && (stream == focusedStream || stream == selfStream)) {
            handleNetworkIssue(QString("拉流抖动(%1)").arg(stream), 1);
        }

        if (meetingStopped || !signalConnected) return;
        const QStringList needed = currentDisplayStreams();
        if (!needed.contains(stream)) return;

        if (++s->retryCount > 8) {
            appendRoomEvent(QString("拉流重试超过上限: %1").arg(stream));
            return;
        }

        const int retryIndex = s->retryCount;
        const int exp = qMin(retryIndex - 1, 4);
        const int baseDelay = 300 * (1 << exp);
        const int jitter = QRandomGenerator::global()->bounded(120);
        const int delayMs = qMin(5000, baseDelay + jitter);
        const quint64 epoch = ++s->retryEpoch;
        ++totalPullRetryCount;
        updateMeetingStatsUi();
        appendRoomEvent(QString("准备重试拉流(%1/8): %2").arg(retryIndex).arg(stream));

        QTimer::singleShot(delayMs, this, [this, stream, epoch]() {
            if (meetingStopped || !signalConnected) return;
            PullSession *s2 = pullSessions.value(stream, nullptr);
            if (!s2) return;
            if (s2->retryEpoch != epoch) return;
            const QStringList needed2 = currentDisplayStreams();
            if (!needed2.contains(stream)) return;
            stopPullSession(stream, false);
            ensurePullSession(stream);
            applyFocusAudioRouting();
        });
    });

    sess->thread->start(QThread::HighPriority);
    const QString url = buildRtmpPlayUrl(stream);
    QMetaObject::invokeMethod(sess->puller, "startPull", Qt::QueuedConnection, Q_ARG(QString, url));
    appendRoomEvent(QString("开始拉流: %1").arg(stream));
}

// 停止并销毁指定拉流会话
void MainWindow::stopPullSession(const QString &stream, bool waitForQuit)
{
    PullSession *sess = pullSessions.value(stream, nullptr);
    if (!sess) return;

    latestRemoteFrames.remove(stream);
    if (sess->puller) {
        disconnect(sess->puller, nullptr, this, nullptr);
        sess->puller->stop();
    }

    bool threadStopped = true;
    if (sess->thread) {
        sess->thread->quit();
        int waitMs = waitForQuit ? 2500 : 200;
        threadStopped = sess->thread->wait(waitMs);
        if (!threadStopped) {
            qWarning() << "[RtmpPuller]" << stream << "stop wait timeout, detach";
            sess->thread->requestInterruption();
            if (sess->puller) {
                QObject::connect(sess->thread, &QThread::finished, sess->puller, &QObject::deleteLater, Qt::UniqueConnection);
            }
            sess->thread->setParent(nullptr);
            QObject::connect(sess->thread, &QThread::finished, sess->thread, &QObject::deleteLater, Qt::UniqueConnection);
        } else {
            delete sess->thread;
        }
        sess->thread = nullptr;
    }

    // 仅在线程确认结束后同步销毁 puller；否则交由 finished->deleteLater 释放
    if (threadStopped && sess->puller) {
        delete sess->puller;
        sess->puller = nullptr;
    } else if (!threadStopped) {
        sess->puller = nullptr;
    }

    pullSessions.remove(stream);
    delete sess;
}

// 停止并销毁全部拉流会话
void MainWindow::stopAllPullSessions(bool waitForQuit)
{
    const QStringList keys = pullSessions.keys();
    for (const QString &s : keys) {
        stopPullSession(s, waitForQuit);
    }
}

// 按宫格需要增减拉流会话并应用音频路由
void MainWindow::syncGridPullers()
{
    if (!signalConnected || meetingStopped) {
        stopAllPullSessions(false);
        return;
    }

    const QStringList neededList = currentDisplayStreams();
    const QSet<QString> needed = QSet<QString>(neededList.begin(), neededList.end());
    const QStringList current = pullSessions.keys();

    for (const QString &s : current) {
        if (!needed.contains(s)) {
            stopPullSession(s, false);
        }
    }
    for (const QString &s : needed) {
        ensurePullSession(s);
    }
    applyFocusAudioRouting();
}

// 进入焦点模式并启动目标流拉取
void MainWindow::startPullStream(const QString &stream)
{
    if (stream.isEmpty()) return;
    currentRemoteStream = stream;
    focusedStream = stream;
    focusMode = true;
    if (remoteStack && ui && ui->remoteVideolabel) {
        remoteStack->setCurrentWidget(ui->remoteVideolabel);
    }
    ensurePullSession(stream);
    applyFocusAudioRouting();
}

// 停止当前所有拉流并清空主流标记
void MainWindow::stopCurrentPull(bool waitForQuit)
{
    stopAllPullSessions(waitForQuit);
    currentRemoteStream.clear();
}
