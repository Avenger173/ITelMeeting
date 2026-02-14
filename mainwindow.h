#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QFile>
#include <QAudioFormat>
#include <QIODevice>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QAudioSink>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QWebSocket>
#include <QHash>
#include <QListWidget>
#include <QListWidgetItem>
#include <QDockWidget>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QFrame>
#include <QStackedLayout>
#include <QVector>
#include <QSet>

#include "videocapture.h"
#include "audiocapture.h"
#include "avrecorder.h"
#include "avsender.h"
#include "avreceiver.h"
#include "rtmppusher.h"
#include "avnetencoder.h"
#include "rtmppuller.h"
#include "avaudioencoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void on_startMeetingButton_clicked();
    void on_switchCameraButton_clicked();
    void on_captureImageButton_clicked();
    void on_startAudioButton_clicked();
    void on_stopAudioButton_clicked();
    void on_startRecordButton_clicked();
    void on_stopRecordButton_clicked();

    void onDebugStartEmptyRecord();
    void onDebugStopEmptyRecord();
    void onDebugGen3sTestVideo();
    void onDebugStartCamRecord();
    void onDebugStopCamRecord();
    void onDebugStartEmptyAV();
    void onDebugStartAudioRecord();
    void onDebugStopAudioRecord();
    void onDebugStartAVRecord();
    void onDebugStopAVRecord();

    void on_startReceiveButton_clicked();
    void on_stopMeetingButton_clicked();

    void onSignalConnected();
    void onSignalDisconnected();
    void onSignalTextMessage(const QString &msg);
    void onRoomUserDoubleClicked(QListWidgetItem *item);
    void onRoomListContextMenu(const QPoint &pos);

private:
    struct MemberState {
        QString user;
        QString stream;
        bool audio = true;
        bool video = true;
        bool pub = false;
        bool host = false;
    };

    void setupSignalUi();
    bool ensureRoomIdentity(bool askRoomIfEmpty);
    void appendRoomEvent(const QString &text);
    void refreshRoomUserList();

    void sendSignalJoin();
    void sendSignalLeave();
    void sendSignalUpdate();
    void sendSignalCmd(const QString &toStream, const QString &action);

    void setupRemoteGridUi();
    void refreshRemoteTiles();
    void applyTileFrame(const QString &stream, const QImage &img);
    void clearAllTileFrames();
    void syncGridPullers();
    void ensurePullSession(const QString &stream);
    void stopPullSession(const QString &stream, bool waitForQuit = true);
    void stopAllPullSessions(bool waitForQuit = true);
    void applyFocusAudioRouting();
    QStringList currentDisplayStreams() const;

    void startPullStream(const QString &stream);
    void stopCurrentPull(bool waitForQuit = true);

private:
    Ui::MainWindow *ui = nullptr;
    QTimer *timer = nullptr;

    QThread *videoThread = nullptr;
    VideoCapture *videoWorker = nullptr;

    QThread *audioThread = nullptr;
    AudioCapture *audioWorker = nullptr;

    QAudioSink *audioSink = nullptr;
    QIODevice *audioOutput = nullptr;
    SwrContext *playSwrCtx = nullptr;
    QAudioFormat playFormat;
    QMetaObject::Connection recordConn;
    QMetaObject::Connection audioSendConn;
    QMetaObject::Connection videoSendConn;

    bool isRecording = false;
    AvRecorder *recorder = nullptr;
    bool camRecording = false;
    int recFps = 30;
    qint64 lastPushMs = 0;

    AVSender *sender = nullptr;
    AVReceiver *receiver = nullptr;

    RtmpPusher *pusher = nullptr;
    AvNetEncoder *netEnc = nullptr;
    AvAudioEncoder *audioEnc = nullptr;

    struct PullSession {
        QString stream;
        rtmppuller *puller = nullptr;
        QThread *thread = nullptr;
        int retryCount = 0;
    };
    QHash<QString, PullSession*> pullSessions;

    QThread *encThread = nullptr;
    QThread *audioEncThread = nullptr;
    QThread *pushThread = nullptr;

    bool audioStopped = false;
    bool meetingStopped = false;
    bool audioPlayEnabled = false;

    QWebSocket *signalSocket = nullptr;
    bool signalConnected = false;
    QString signalUrl = "ws://127.0.0.1:9001";
    QString roomId;
    QString userId;
    QString selfStream;
    QString roomHostStream;

    bool localAudioOn = true;
    bool localVideoOn = true;
    bool isPublishing = false;

    QHash<QString, MemberState> memberStates;
    QString preferredRemoteStream;
    QString currentRemoteStream;
    bool stopMeetingInProgress = false;

    QDockWidget *roomDock = nullptr;
    QPushButton *connectSignalButton = nullptr;
    QLabel *signalStateLabel = nullptr;
    QLabel *roomCountLabel = nullptr;
    QListWidget *roomUserList = nullptr;
    QPlainTextEdit *roomEventLog = nullptr;

    struct RemoteTile {
        QFrame *frame = nullptr;
        QLabel *videoLabel = nullptr;
        QLabel *nameLabel = nullptr;
        QLabel *stateLabel = nullptr;
        QString stream;
        bool hasFrame = false;
    };
    QWidget *remoteContainer = nullptr;
    QWidget *remoteGridPage = nullptr;
    QStackedLayout *remoteStack = nullptr;
    QVector<RemoteTile> remoteTiles;
    QHash<QString, int> streamToTile;
    QString focusedStream;
    bool focusMode = false;
};

#endif // MAINWINDOW_H
