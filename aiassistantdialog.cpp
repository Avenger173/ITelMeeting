#include "aiassistantdialog.h"
#include "ui_aiassistantdialog.h"

#include <QDateTime>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QUrl>

AiAssistantDialog::AiAssistantDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::AiAssistantDialog)
    , networkManager(new QNetworkAccessManager(this))
{
    ui->setupUi(this);
    setWindowTitle(QStringLiteral("AI助手"));
    setModal(false);
    setMinimumSize(680, 560);

    ui->promptTextEdit->installEventFilter(this);
    ui->modeComboBox->setItemData(0, QStringLiteral("general"));
    ui->modeComboBox->setItemData(1, QStringLiteral("project"));

    {
        QSettings settings(QStringLiteral("SmartMeet"), QStringLiteral("SmartMeet"));
        const QString savedMode = settings.value(QStringLiteral("ai/mode"), QStringLiteral("general")).toString();
        const int idx = ui->modeComboBox->findData(savedMode);
        ui->modeComboBox->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    connect(ui->sendButton, &QPushButton::clicked, this, &AiAssistantDialog::onSendClicked, Qt::UniqueConnection);
    connect(ui->clearButton, &QPushButton::clicked, this, &AiAssistantDialog::onClearClicked, Qt::UniqueConnection);
    connect(ui->modeComboBox, &QComboBox::currentIndexChanged, this, [this](int) {
        QSettings settings(QStringLiteral("SmartMeet"), QStringLiteral("SmartMeet"));
        settings.setValue(QStringLiteral("ai/mode"), currentAssistantMode());
        historyTurns.clear();
        transcriptLines.clear();
        pendingTranscriptIndex = -1;
        pendingSpeaker.clear();
        appendTranscript(QStringLiteral("系统"), QStringLiteral("助手模式已切换，短期记忆已重置。"));
        updateUiState();
    });

    appendTranscript(QStringLiteral("系统"), QStringLiteral("欢迎使用 AI 助手。这里的问答仅当前客户端可见，不会广播到房间。"));
    updateUiState();
}

AiAssistantDialog::~AiAssistantDialog()
{
    delete ui;
}

void AiAssistantDialog::configureAssistant(bool enabled,
                                          const QString &serviceBaseUrlValue,
                                          const QString &assistantNameValue,
                                          int timeoutMsValue)
{
    assistantEnabled = enabled;
    serviceBaseUrl = normalizedUrlBase(serviceBaseUrlValue);
    if (!assistantNameValue.trimmed().isEmpty()) {
        assistantName = assistantNameValue.trimmed();
    }
    timeoutMs = timeoutMsValue > 0 ? timeoutMsValue : 600000;
    setWindowTitle(assistantName);
    updateUiState();
}

void AiAssistantDialog::setSessionContext(const QString &profile,
                                          const QString &roomIdValue,
                                          const QString &userIdValue,
                                          const QString &streamIdValue)
{
    activeProfile = profile;
    roomId = roomIdValue;
    userId = userIdValue;
    streamId = streamIdValue;
}

void AiAssistantDialog::openWithPrompt(const QString &prompt, bool autoSubmit)
{
    if (!prompt.trimmed().isEmpty()) {
        ui->promptTextEdit->setPlainText(prompt.trimmed());
    }
    show();
    raise();
    activateWindow();
    ui->promptTextEdit->setFocus();
    if (autoSubmit && !prompt.trimmed().isEmpty()) {
        submitPrompt(prompt.trimmed());
    }
}

bool AiAssistantDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == ui->promptTextEdit && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
            if (keyEvent->modifiers().testFlag(Qt::ShiftModifier)) {
                return false;
            }
            onSendClicked();
            return true;
        }
    }
    return QDialog::eventFilter(watched, event);
}

void AiAssistantDialog::onSendClicked()
{
    submitPrompt(ui->promptTextEdit->toPlainText().trimmed());
}

void AiAssistantDialog::onClearClicked()
{
    historyTurns.clear();
    transcriptLines.clear();
    pendingTranscriptIndex = -1;
    pendingSpeaker.clear();
    appendTranscript(QStringLiteral("系统"), QStringLiteral("问答记录已清空，短期记忆也一起重置了。"));
}

void AiAssistantDialog::appendTranscript(const QString &speaker, const QString &content)
{
    if (content.trimmed().isEmpty()) {
        return;
    }
    transcriptLines.append(makeTranscriptLine(speaker, content));
    refreshTranscriptView();
}

QString AiAssistantDialog::makeTranscriptLine(const QString &speaker, const QString &content) const
{
    return QStringLiteral("[%1] %2: %3")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("hh:mm:ss")),
             speaker,
             content.trimmed());
}

void AiAssistantDialog::refreshTranscriptView()
{
    ui->historyPlainTextEdit->setPlainText(transcriptLines.join(QLatin1Char('\n')));
    auto *bar = ui->historyPlainTextEdit->verticalScrollBar();
    if (bar) {
        bar->setValue(bar->maximum());
    }
}

void AiAssistantDialog::setPendingTranscript(const QString &speaker, const QString &content)
{
    pendingSpeaker = speaker;
    pendingTranscriptIndex = transcriptLines.size();
    transcriptLines.append(makeTranscriptLine(speaker, content));
    refreshTranscriptView();
}

void AiAssistantDialog::resolvePendingTranscript(const QString &speaker, const QString &content)
{
    if (content.trimmed().isEmpty()) {
        return;
    }

    if (pendingTranscriptIndex >= 0 && pendingTranscriptIndex < transcriptLines.size()) {
        transcriptLines[pendingTranscriptIndex] = makeTranscriptLine(speaker, content);
    } else {
        transcriptLines.append(makeTranscriptLine(speaker, content));
    }

    pendingTranscriptIndex = -1;
    pendingSpeaker.clear();
    refreshTranscriptView();
}

QString AiAssistantDialog::buildAssistantUrl() const
{
    const QString base = normalizedUrlBase(serviceBaseUrl);
    if (base.isEmpty()) {
        return QString();
    }
    if (base.endsWith(QStringLiteral("/assistant"), Qt::CaseInsensitive)) {
        return base;
    }
    return QStringLiteral("%1/assistant").arg(base);
}

QString AiAssistantDialog::normalizedUrlBase(QString value) const
{
    value = value.trimmed();
    while (value.endsWith(QLatin1Char('/'))) {
        value.chop(1);
    }
    return value;
}

QString AiAssistantDialog::currentAssistantMode() const
{
    if (!ui->modeComboBox) {
        return QStringLiteral("general");
    }
    const QString mode = ui->modeComboBox->currentData().toString().trimmed();
    return mode.isEmpty() ? QStringLiteral("general") : mode;
}

QString AiAssistantDialog::currentPendingSpeaker() const
{
    return pendingSpeaker.isEmpty() ? assistantName : pendingSpeaker;
}

void AiAssistantDialog::rememberTurn(const QString &role, const QString &content)
{
    const QString cleanContent = content.trimmed();
    if (cleanContent.isEmpty()) {
        return;
    }

    historyTurns.push_back({role, cleanContent});
    while (historyTurns.size() > historyMessageLimit) {
        historyTurns.remove(0);
    }
}

void AiAssistantDialog::updateUiState()
{
    ui->sendButton->setEnabled(assistantEnabled && !requestInFlight);
    ui->promptTextEdit->setEnabled(assistantEnabled && !requestInFlight);
    ui->modeComboBox->setEnabled(!requestInFlight);

    if (!assistantEnabled) {
        ui->assistantHintLabel->setText(QStringLiteral("AI 助手未启用，请检查 smartmeet.ini 的 [ai] 配置。"));
    } else if (currentAssistantMode() == QStringLiteral("project")) {
        ui->assistantHintLabel->setText(
            QStringLiteral("项目助手：优先回答 SmartMeet 的功能、部署和架构问题。"));
    } else {
        ui->assistantHintLabel->setText(
            QStringLiteral("通用助手：普通问题也可以直接问。"));
    }

    ui->statusLabel->setText(requestInFlight
                                 ? QStringLiteral("状态：AI 正在思考中，请稍等…")
                                 : QStringLiteral("状态：就绪 | 短期记忆最近 %1 条消息").arg(historyMessageLimit));
}

bool AiAssistantDialog::submitPrompt(const QString &prompt)
{
    const QString cleanPrompt = prompt.trimmed();
    if (cleanPrompt.isEmpty()) {
        ui->statusLabel->setText(QStringLiteral("状态：请先输入问题"));
        return false;
    }
    if (!assistantEnabled || buildAssistantUrl().isEmpty()) {
        appendTranscript(assistantName, QStringLiteral("AI 服务未启用，请检查 smartmeet.ini 的 [ai] 配置。"));
        updateUiState();
        return false;
    }
    if (requestInFlight) {
        appendTranscript(assistantName, QStringLiteral("上一条问题还在思考中，先等等我呀。"));
        return false;
    }

    appendTranscript(QStringLiteral("我"), cleanPrompt);

    QNetworkRequest request{QUrl(buildAssistantUrl())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

    QJsonArray historyArray;
    for (const auto &turn : historyTurns) {
        QJsonObject item;
        item[QStringLiteral("role")] = turn.role;
        item[QStringLiteral("content")] = turn.content;
        historyArray.push_back(item);
    }

    QJsonObject payload;
    payload[QStringLiteral("message")] = cleanPrompt;
    payload[QStringLiteral("room_id")] = roomId;
    payload[QStringLiteral("user_id")] = userId;
    payload[QStringLiteral("stream")] = streamId;
    payload[QStringLiteral("profile")] = activeProfile;
    payload[QStringLiteral("assistant_mode")] = currentAssistantMode();
    payload[QStringLiteral("history")] = historyArray;

    QNetworkReply *reply = networkManager->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    if (!reply) {
        appendTranscript(assistantName, QStringLiteral("AI 请求创建失败。"));
        return false;
    }

    requestInFlight = true;
    ui->promptTextEdit->clear();
    setPendingTranscript(assistantName, QStringLiteral("思考中…这题我得认真想一下。"));
    updateUiState();

    auto *timeoutTimer = new QTimer(reply);
    timeoutTimer->setSingleShot(true);
    connect(timeoutTimer, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("timedOut", true);
        reply->abort();
    });
    timeoutTimer->start(timeoutMs);

    connect(reply, &QNetworkReply::finished, this, [this, reply, cleanPrompt]() {
        const bool timedOut = reply->property("timedOut").toBool();
        const QByteArray body = reply->readAll();
        requestInFlight = false;
        updateUiState();

        if (reply->error() != QNetworkReply::NoError) {
            resolvePendingTranscript(currentPendingSpeaker(),
                                     timedOut
                                         ? QStringLiteral("我这边思考超时了。可以再问一次，或者把 smartmeet.ini 里的 AI 超时再调大一点。")
                                         : QStringLiteral("这次回答失败了：%1").arg(reply->errorString()));
            reply->deleteLater();
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            resolvePendingTranscript(currentPendingSpeaker(), QStringLiteral("我收到了异常返回，暂时没法正确组织回答。"));
            reply->deleteLater();
            return;
        }

        const QJsonObject obj = doc.object();
        const QString answer = obj.value(QStringLiteral("answer")).toString().trimmed();
        const QString provider = obj.value(QStringLiteral("provider")).toString().trimmed();
        const QString model = obj.value(QStringLiteral("model")).toString().trimmed();
        const QString speaker = (provider.isEmpty() && model.isEmpty())
                                    ? assistantName
                                    : QStringLiteral("%1 (%2/%3)")
                                          .arg(assistantName,
                                               provider.isEmpty() ? QStringLiteral("local") : provider,
                                               model.isEmpty() ? QStringLiteral("default") : model);
        const QString finalAnswer = answer.isEmpty()
                                        ? QStringLiteral("这次我没拿到有效回答，你可以换个问法再试试。")
                                        : answer;

        rememberTurn(QStringLiteral("user"), cleanPrompt);
        rememberTurn(QStringLiteral("assistant"), finalAnswer);
        resolvePendingTranscript(speaker, finalAnswer);
        reply->deleteLater();
    });

    return true;
}
