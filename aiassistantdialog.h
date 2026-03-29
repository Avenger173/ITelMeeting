#ifndef AIASSISTANTDIALOG_H
#define AIASSISTANTDIALOG_H

#include <QDialog>
#include <QNetworkAccessManager>
#include <QStringList>
#include <QVector>

QT_BEGIN_NAMESPACE
namespace Ui {
class AiAssistantDialog;
}
QT_END_NAMESPACE

class AiAssistantDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AiAssistantDialog(QWidget *parent = nullptr);
    ~AiAssistantDialog() override;

    void configureAssistant(bool enabled,
                            const QString &serviceBaseUrl,
                            const QString &assistantName,
                            int timeoutMs);
    void setSessionContext(const QString &profile,
                           const QString &roomId,
                           const QString &userId,
                           const QString &streamId);
    void openWithPrompt(const QString &prompt = QString(), bool autoSubmit = false);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onSendClicked();
    void onClearClicked();

private:
    struct ConversationTurn {
        QString role;
        QString content;
    };

    void appendTranscript(const QString &speaker, const QString &content);
    QString makeTranscriptLine(const QString &speaker, const QString &content) const;
    void refreshTranscriptView();
    void setPendingTranscript(const QString &speaker, const QString &content);
    void resolvePendingTranscript(const QString &speaker, const QString &content);
    QString buildAssistantUrl() const;
    QString normalizedUrlBase(QString value) const;
    QString currentAssistantMode() const;
    QString currentPendingSpeaker() const;
    void rememberTurn(const QString &role, const QString &content);
    void updateUiState();
    bool submitPrompt(const QString &prompt);

private:
    Ui::AiAssistantDialog *ui = nullptr;
    QNetworkAccessManager *networkManager = nullptr;
    bool assistantEnabled = true;
    QString serviceBaseUrl = QStringLiteral("http://127.0.0.1:18080");
    QString assistantName = QStringLiteral("AI助手");
    int timeoutMs = 600000;
    bool requestInFlight = false;
    int historyMessageLimit = 8;

    QString activeProfile;
    QString roomId;
    QString userId;
    QString streamId;
    QVector<ConversationTurn> historyTurns;
    QStringList transcriptLines;
    int pendingTranscriptIndex = -1;
    QString pendingSpeaker;
};

#endif // AIASSISTANTDIALOG_H
