#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QFile>
#include <QAudioFormat>
#include <QIODevice>
#include <QAudioDevice>
#include <QMediaDevices>
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QWebSocket>
#include <QHash>
#include <QListWidget>
#include <QListWidgetItem>
#include <QDockWidget>
#include <QPushButton>
#include <QToolButton>
#include <QPlainTextEdit>
#include <QLabel>
#include <QFrame>
#include <QStackedLayout>
#include <QVector>
#include <QSet>
#include <QAction>
#include <QTextEdit>
#include <QColor>
#include <QComboBox>
#include <QSpinBox>
#include <QSlider>
#include <QLine>

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
    void on_captureImageButton_clicked();
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
    void onSendChatClicked();

private:
    struct MemberState {
        QString user;
        QString stream;
        bool audio = true;
        bool video = true;
        bool pub = false;
        bool host = false;
        bool cohost = false;
        bool share=false;
    };

    void setupSignalUi();
    bool ensureRoomIdentity(bool askRoomIfEmpty);
    void appendRoomEvent(const QString &text);
    void refreshRoomUserList();
    void setupBottomMenus();
    void refreshSelfControlActions();

    void sendSignalJoin();
    void sendSignalLeave();
    void sendSignalUpdate();
    void sendSignalCmd(const QString &toStream, const QString &action);
    void sendSignalChat(const QString &content);
    //鍏变韩婧愰€夋嫨
    struct  ShareSourceCandidate
    {
        bool isWindow=false;//false=灞忓箷锛宼ure=绐楀彛
        int screenIndex=0;//灞忓箷绱㈠紩
        quint64 windowId=0;//绐楀彛鍙ユ焺(windows)
        QString label;//UI鏄剧ず鍚?
    };
    QVector<ShareSourceCandidate> buildShareSourceCandidates() const;
    void chooseShareSource();

    void appendChatMessage(const QString &fromStream,const QString &content,qint64 tsMs,bool isSelf,const QString &msgId=QString());

    void setupRemoteGridUi();
    void syncRemoteContainerGeometry();
    void refreshRemoteTiles();
    void applyTileFrame(const QString &stream, const QImage &img);
    void clearAllTileFrames();
    void updateFocusStatusBadge();
    void syncGridPullers();
    void ensurePullSession(const QString &stream);
    void stopPullSession(const QString &stream, bool waitForQuit = true);
    void stopAllPullSessions(bool waitForQuit = true);
    void applyFocusAudioRouting();
    QStringList currentDisplayStreams() const;

    void startPullStream(const QString &stream);
    void stopCurrentPull(bool waitForQuit = true);
    void startAudioCapture();
    void stopAudioCapture();

    void applyLocalVideoSource();
    void applyBeautyToWorker();
    void setBeautyMode(const QString &modeName,int level);
    //鐧芥澘
    void setupWhiteboardUi();
    void ensureWhiteboardCanvas();
    void updateWhiteboardCanvasLabel();
    QPoint mapWhiteboardPoint(const QPoint &widgetPos) const;

    QColor whiteboardSelectedColor() const;
    int whiteboardSelectedWidth() const;
    bool canClearWhiteboard() const;

    void drawWhiteboardLine(const QPoint &from,const QPoint &to,bool broadcast,
                            const QColor &color,int width,const QString &strokeId,const QString &ownerStream);
    void redrawWhiteboardFromStrokes();
    void removeStrokeById(const QString &strokeId,const QString &byStream=QString());
    void undoLastLocalStroke(bool broadcast);

    void clearWhiteboard(bool broadcast,const QString &byStream=QString());
    void sendSignalWhiteboardDraw(const QPoint &from,const QPoint &to,
                                  const QColor &color,int width,const QString &strokeId);
    void sendSignalWhiteboardClear();
    void sendSignalWhiteboardUndo(const QString &strokeId);

    bool canWriteWhiteboard() const;
    void applyWhiteboardLockUi();
    void sendSignalWhiteboardLock(bool locked);

    bool ensureSignalCredential();
    void sendSignalAuthLogin();
    void sendSignalAuthRegister();
private:
    Ui::MainWindow *ui = nullptr;
    QTimer *timer = nullptr;

    QThread *videoThread = nullptr;
    VideoCapture *videoWorker = nullptr;

    QThread *audioThread = nullptr;
    AudioCapture *audioWorker = nullptr;

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
    QPlainTextEdit *chatMessageLog = nullptr;
    QTextEdit *chatInputEdit = nullptr;
    QPushButton *sendChatButton = nullptr;
    QSet<QString> seenChatMsgIds;
    quint64 chatLocalSeq = 0;
    QAction *selfMicToggleAction = nullptr;
    QAction *selfCamToggleAction = nullptr;
    QAction *selfShareToggleAction=nullptr;

    struct RemoteTile {
        QFrame *frame = nullptr;
        QLabel *videoLabel = nullptr;
        QLabel *nameLabel = nullptr;
        QLabel *stateLabel = nullptr;
        QLabel *cornerBadge = nullptr;
        QString stream;
        bool hasFrame = false;
    };
    QWidget *remoteContainer = nullptr;
    QWidget *remoteGridPage = nullptr;
    QStackedLayout *remoteStack = nullptr;
    QGridLayout *remoteGridLayout = nullptr;
    QLabel *focusStatusLabel = nullptr;
    QVector<RemoteTile> remoteTiles;
    QHash<QString, int> streamToTile;
    QString focusedStream;
    bool focusMode = false;
    bool localScreenShareOn=false;
    int shareScreenIndex=0;

    int localBeautyLevel=60;//0~100，默认强度
    QString localBeautyMode=QStringLiteral("鍏抽棴");
    int localBeautyStyle=0;//0关 1自然 2清晰 3柔和 4磨皮 5瘦脸 6祛皱
    QSlider *beautyStrengthSlider=nullptr;
    QLabel *beautyStrengthValueLabel=nullptr;
    QToolButton *whiteboardLockButton=nullptr;
    bool whiteboardLocked=false;

    quint64 shareWindowId=0;
    QString shareSourceName=QStringLiteral("灞忓箷1");

    QLabel *whiteboardCanvasLabel=nullptr;
    QPushButton *whiteboardClearButton=nullptr;
    QToolButton *whiteboardPenButton=nullptr;
    QImage whiteboardCanvas;
    bool whiteboardPenEnabled=true;
    bool whiteboardMouseDown=false;
    QPoint whiteboardLastPoint;
    QSet<QString> seenWhiteboardMsgIds;
    quint64 whiteboardLocalSeq=0;

    QComboBox *whiteboardColorCombo=nullptr;
    QSpinBox *whiteboardWidthSpin=nullptr;
    QPushButton *whiteboardUndoButton=nullptr;

    struct WhiteboardStroke
    {
        QString strokeId;
        QString ownerStream;
        QColor color=QColor("#e03131");
        int width=3;
        QVector<QLine> segments;
    };
    QVector<WhiteboardStroke> WhiteboardStrokes;
    QString whiteboardActionStrokeId;

    bool signalAuthed = false;
    bool authRegisterTried = false;
    QString loginUser;
    QString loginPassword;
};

#endif // MAINWINDOW_H

