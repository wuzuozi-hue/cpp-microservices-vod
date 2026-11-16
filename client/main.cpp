#include <QtCore/QJsonArray>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtCore/QUuid>
#include <QtGui/QImageReader>
#include <QtGui/QPainter>
#include <QtGui/QShortcut>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSlider>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <mpv/client.h>
#include <functional>
#include <memory>

class Player : public QWidget {
public:
    explicit Player(bool headless, QWidget* parent = nullptr) : QWidget(parent) {
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 20, 24, 20); layout->setSpacing(12);
        title_ = new QLabel("选择视频开始播放", this); title_->setStyleSheet("font-size: 17px; font-weight: 700; color: #f1eff9;"); layout->addWidget(title_);
        screen_ = new QWidget(this); screen_->setMinimumSize(480, 270); screen_->setStyleSheet("background:#161b22");
        screen_->setAttribute(Qt::WA_NativeWindow); layout->addWidget(screen_, 1);
        position_ = new QSlider(Qt::Horizontal, this); position_->setRange(0, 1000); position_->setObjectName("playbackPosition"); layout->addWidget(position_);
        auto* buttons = new QHBoxLayout;
        auto* rewind = new QPushButton("⏪ 10秒", this); rewind->setObjectName("rewind"); buttons->addWidget(rewind);
        auto* pause = new QPushButton("暂停", this); pause->setObjectName("playPause"); buttons->addWidget(pause);
        auto* forward = new QPushButton("10秒 ⏩", this); forward->setObjectName("forward"); buttons->addWidget(forward);
        clock_ = new QLabel("00:00 / 00:00", this); clock_->setStyleSheet("color: #a9b0c2; font-size: 13px;"); buttons->addWidget(clock_);
        buttons->addStretch();
        quality_ = new QLabel("清晰度：当前视频源", this); quality_->setObjectName("videoQuality"); quality_->setToolTip("当前视频只有一条清晰度的视频流"); buttons->addWidget(quality_);
        auto* speed = new QComboBox(this); speed->setObjectName("playbackSpeed");
        for (double rate : {0.5, 0.75, 1.0, 1.25, 1.5, 2.0}) speed->addItem(QString::number(rate) + "x", rate);
        speed->setCurrentIndex(2); buttons->addWidget(speed);
        auto* label = new QLabel("音量", this); buttons->addWidget(label);
        auto* volume = new QSlider(Qt::Horizontal, this); volume->setRange(0, 100); volume->setValue(70); volume->setMaximumWidth(150); buttons->addWidget(volume);
        auto* fullscreen = new QPushButton("全屏", this); fullscreen->setObjectName("fullscreen"); buttons->addWidget(fullscreen);
        layout->addLayout(buttons);
        mpv_ = mpv_create();
        if (!mpv_) { layout->addWidget(new QLabel("无法初始化播放器", this)); return; }
        if (headless) { mpv_set_option_string(mpv_, "vo", "null"); mpv_set_option_string(mpv_, "ao", "null"); }
        else {
            int64_t wid = static_cast<int64_t>(screen_->winId());
            if (mpv_set_option(mpv_, "wid", MPV_FORMAT_INT64, &wid) < 0) { mpv_terminate_destroy(mpv_); mpv_ = nullptr; return; }
        }
        mpv_set_option_string(mpv_, "terminal", "no");
        if (mpv_initialize(mpv_) < 0) { mpv_terminate_destroy(mpv_); mpv_ = nullptr; return; }
        mpv_observe_property(mpv_, 1, "duration", MPV_FORMAT_DOUBLE);
        mpv_observe_property(mpv_, 2, "time-pos", MPV_FORMAT_DOUBLE);
        mpv_observe_property(mpv_, 3, "pause", MPV_FORMAT_FLAG);
        mpv_observe_property(mpv_, 4, "width", MPV_FORMAT_INT64);
        mpv_observe_property(mpv_, 5, "height", MPV_FORMAT_INT64);
        double initialVolume = 70; mpv_set_property(mpv_, "volume", MPV_FORMAT_DOUBLE, &initialVolume);
        connect(pause, &QPushButton::clicked, this, [this] {
            if (!mpv_) return; const char* command[] = {"cycle", "pause", nullptr}; mpv_command_async(mpv_, 0, command);
        });
        auto seek = [this](const char* amount) { if (mpv_) { const char* command[] = {"seek", amount, "relative", nullptr}; mpv_command_async(mpv_, 0, command); } };
        connect(rewind, &QPushButton::clicked, this, [seek] { seek("-10"); });
        connect(forward, &QPushButton::clicked, this, [seek] { seek("10"); });
        connect(speed, &QComboBox::currentIndexChanged, this, [this, speed](int) {
            if (mpv_) { double value = speed->currentData().toDouble(); mpv_set_property_async(mpv_, 0, "speed", MPV_FORMAT_DOUBLE, &value); }
        });
        auto toggleFullscreen = [this, fullscreen] {
            auto* top = window(); if (top->isFullScreen()) { top->showNormal(); fullscreen->setText("全屏"); }
            else { top->showFullScreen(); fullscreen->setText("退出全屏"); }
        };
        connect(fullscreen, &QPushButton::clicked, this, toggleFullscreen);
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        connect(escape, &QShortcut::activated, this, [this, fullscreen] { if (window()->isFullScreen()) { window()->showNormal(); fullscreen->setText("全屏"); } });
        connect(volume, &QSlider::valueChanged, this, [this](int value) {
            if (mpv_) { double v = value; mpv_set_property_async(mpv_, 0, "volume", MPV_FORMAT_DOUBLE, &v); }
        });
        connect(position_, &QSlider::sliderReleased, this, [this] {
            if (!mpv_ || duration_ <= 0) return;
            double target = duration_ * position_->value() / 1000.0;
            mpv_set_property_async(mpv_, 0, "time-pos", MPV_FORMAT_DOUBLE, &target);
        });
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, [this, pause] {
            if (!mpv_) return;
            for (;;) {
                auto* event = mpv_wait_event(mpv_, 0);
                if (event->event_id == MPV_EVENT_NONE) break;
                if (event->event_id == MPV_EVENT_FILE_LOADED) playback_state_ = 1;
                if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && event->data) {
                    const auto* property = static_cast<mpv_event_property*>(event->data);
                    if (property->format == MPV_FORMAT_DOUBLE && property->data) {
                        const double value = *static_cast<double*>(property->data);
                        if (event->reply_userdata == 1) duration_ = value;
                        if (event->reply_userdata == 2) current_ = value;
                    }
                    if (event->reply_userdata == 3 && property->format == MPV_FORMAT_FLAG && property->data)
                        pause->setText(*static_cast<int*>(property->data) ? "播放" : "暂停");
                    if (property->format == MPV_FORMAT_INT64 && property->data) {
                        if (event->reply_userdata == 4) width_ = *static_cast<int64_t*>(property->data);
                        if (event->reply_userdata == 5) height_ = *static_cast<int64_t*>(property->data);
                        if (width_ > 0 && height_ > 0) quality_->setText(QString("当前 %1×%2").arg(width_).arg(height_));
                    }
                }
                if (event->event_id == MPV_EVENT_END_FILE) {
                    const auto* end = static_cast<mpv_event_end_file*>(event->data);
                    if (end && end->reason == MPV_END_FILE_REASON_EOF && playback_state_ == 1) playback_state_ = 2;
                    else if (!end || end->reason == MPV_END_FILE_REASON_ERROR) { playback_state_ = -1; title_->setText("播放失败，请检查视频或网络连接"); }
                }
            }
            if (duration_ > 0) {
                if (!position_->isSliderDown()) position_->setValue(qBound(0, static_cast<int>(1000 * current_ / duration_), 1000));
                auto timeText = [](double value) { const int seconds = qMax(0, static_cast<int>(value)); return QString("%1:%2").arg(seconds / 60, 2, 10, QChar('0')).arg(seconds % 60, 2, 10, QChar('0')); };
                clock_->setText(timeText(current_) + " / " + timeText(duration_));
            }
        });
        timer_->start(200);
    }
    ~Player() override { if (timer_) timer_->stop(); if (mpv_) mpv_terminate_destroy(mpv_); }
    bool available() const { return mpv_ != nullptr; }
    int playbackState() const { return playback_state_; }
#ifdef VOD_CLIENT_NO_MAIN
    double playbackSpeedForTest() const { double value = 0; if (mpv_) mpv_get_property(mpv_, "speed", MPV_FORMAT_DOUBLE, &value); return value; }
#endif
    void play(const QUrl& url, const QString& title = {}) {
        if (!mpv_) return;
        auto encoded = url.toEncoded(); const char* args[] = {"loadfile", encoded.constData(), nullptr};
        playback_state_ = 0; duration_ = 0; current_ = 0; width_ = 0; height_ = 0; position_->setValue(0); clock_->setText("00:00 / 00:00");
        quality_->setText("清晰度：当前视频源");
        title_->setText(title.isEmpty() ? "正在播放" : title);
        int paused = 0; mpv_set_property_async(mpv_, 0, "pause", MPV_FORMAT_FLAG, &paused);
        if (mpv_command_async(mpv_, 0, args) < 0) playback_state_ = -1;
    }
private:
    QWidget* screen_ = nullptr; QSlider* position_ = nullptr; QLabel *clock_ = nullptr, *title_ = nullptr, *quality_ = nullptr; QTimer* timer_ = nullptr;
    int64_t width_ = 0, height_ = 0;
    mpv_handle* mpv_ = nullptr; double duration_ = 0, current_ = 0; int playback_state_ = 0;
};

class Window : public QMainWindow {
public:
    explicit Window(bool headless) {
        setWindowTitle("视频点播"); resize(1100, 780);
        setStyleSheet(R"(
            QMainWindow, QWidget { background: #10131c; color: #e9edf7; font-size: 14px; }
            QLabel { background: transparent; }
            QTabWidget::pane { border: 1px solid #282e3d; border-radius: 14px; background: #171b26; top: -1px; }
            QTabBar::tab { background: transparent; color: #929bb0; padding: 13px 18px; border-bottom: 3px solid transparent; }
            QTabBar::tab:selected { color: #ffffff; border-bottom: 3px solid #e84950; font-weight: 700; }
            QTabBar::tab:hover:!selected { color: #ffe8e9; background: #202434; }
            QLineEdit, QTextEdit, QSpinBox { background: #202533; color: #f5f6fc; border: 1px solid #343b4d; border-radius: 8px; padding: 9px 11px; selection-background-color: #d6454d; }
            QLineEdit:focus, QTextEdit:focus, QSpinBox:focus { border: 1px solid #ed777c; }
            QComboBox { background: #202533; color: #f5f6fc; border: 1px solid #343b4d; border-radius: 7px; padding: 8px; }
            QPushButton { background: #292e3e; color: #e8e8f3; border: 1px solid #383e50; border-radius: 8px; padding: 9px 16px; font-weight: 600; }
            QPushButton:hover { background: #393f52; border-color: #70639c; }
            QPushButton:pressed { background: #4b456d; }
            QPushButton:disabled, QPushButton#uploadVideo:disabled { background: #252937; color: #7b8190; border-color: #343846; }
            QPushButton#emailLogin, QPushButton#passwordLogin, QPushButton#uploadVideo, QPushButton#playSelected { background: #df4149; color: white; border-color: #df4149; }
            QPushButton#emailLogin:hover, QPushButton#passwordLogin:hover, QPushButton#uploadVideo:hover, QPushButton#playSelected:hover { background: #f25b63; }
            QTableWidget { background: #1b1f2b; alternate-background-color: #202532; border: 1px solid #303646; border-radius: 9px; gridline-color: #292f3d; selection-background-color: #71373d; selection-color: white; }
            QListWidget#videoCards { background: transparent; border: none; outline: none; }
            QListWidget#videoCards::item { color: #eef0f8; background: #202532; border: 1px solid #303646; border-radius: 10px; padding: 8px; }
            QListWidget#videoCards::item:hover { background: #2c3043; border-color: #7564b4; }
            QListWidget#videoCards::item:selected { background: #4b3034; border-color: #f17a7f; }
            QHeaderView::section { background: #232837; color: #a5afc2; border: none; border-bottom: 1px solid #33394a; padding: 11px; font-weight: 600; }
            QProgressBar { background: #272c3b; border: none; border-radius: 5px; text-align: center; }
            QProgressBar::chunk { background: #e84950; border-radius: 5px; }
            QLabel#connectionStatus { color: #ffd2d3; background: #492c30; border-radius: 12px; padding: 6px 14px; }
        )");
        auto* root = new QWidget(this); auto* outer = new QHBoxLayout(root);
        outer->setContentsMargins(0, 0, 0, 0); outer->setSpacing(0);
        auto* sidebar = new QWidget(root); sidebar->setObjectName("sidebar"); sidebar->setFixedWidth(184);
        sidebar->setStyleSheet("QWidget#sidebar { background: #171920; border-right: 1px solid #30313a; } QListWidget { background: transparent; border: none; outline: none; } QListWidget::item { color: #bdbdc5; padding: 12px 18px; border-radius: 7px; } QListWidget::item:selected { background: #35292e; color: #ff6269; font-weight: 700; }");
        auto* sideLayout = new QVBoxLayout(sidebar); sideLayout->setContentsMargins(12, 24, 12, 18); sideLayout->setSpacing(12);
        auto* sideBrand = new QLabel("●  VOD 视频", sidebar); sideBrand->setStyleSheet("color: #f35159; font-size: 20px; font-weight: 700; padding: 0 6px;"); sideLayout->addWidget(sideBrand);
        auto* sideCaption = new QLabel("发现与管理", sidebar); sideCaption->setStyleSheet("color: #81838d; font-size: 12px; padding: 8px 8px 0;"); sideLayout->addWidget(sideCaption);
        auto* navigation = new QListWidget(sidebar); navigation->setObjectName("mainNavigation"); sideLayout->addWidget(navigation, 1);
        auto* sideFoot = new QLabel("你的私人视频空间", sidebar); sideFoot->setStyleSheet("color: #73757f; font-size: 11px; padding: 6px;"); sideLayout->addWidget(sideFoot);
        outer->addWidget(sidebar);
        auto* content = new QWidget(root); outer->addWidget(content, 1);
        auto* layout = new QVBoxLayout(content);
        layout->setContentsMargins(24, 18, 24, 22); layout->setSpacing(14);
        auto* connection = new QHBoxLayout;
        auto* brand = new QLabel("▶  VOD  视频点播", root); brand->setStyleSheet("font-size: 21px; font-weight: 700; color: #f5f1ff;"); connection->addWidget(brand);
        connection->addStretch(); connection->addWidget(new QLabel("服务地址", root));
        base_ = new QLineEdit("http://127.0.0.1:18082", root); base_->setObjectName("serverAddress"); base_->setMaximumWidth(260); connection->addWidget(base_);
        status_ = new QLabel("未登录", root); status_->setObjectName("connectionStatus"); connection->addWidget(status_); layout->addLayout(connection);
        tabs_ = new QTabWidget(content); tabs_->tabBar()->hide(); layout->addWidget(tabs_);
        setupHome(!headless); setupUpload(); setupMyVideos(); setupLogin(); setupProfile(); setupReview(); setupAdmins();
        player_ = new Player(headless, tabs_); tabs_->addTab(player_, "播放");
        for (int i = 0; i < tabs_->count(); ++i) navigation->addItem(tabs_->tabText(i));
        connect(navigation, &QListWidget::currentRowChanged, tabs_, &QTabWidget::setCurrentIndex);
        connect(tabs_, &QTabWidget::currentChanged, navigation, [navigation](int index) { navigation->setCurrentRow(index); });
        navigation->setCurrentRow(0);
        setCentralWidget(root);
    }
    bool playerAvailable() const { return player_->available(); }
    void playTest(const QString& path) { player_->play(QUrl::fromLocalFile(path)); }
    int playerState() const { return player_->playbackState(); }
private:
    using Callback = std::function<void(bool, const QJsonObject&, const QString&)>;
    QUrl endpoint(const QString& path) const {
        QUrl base(base_->text().trimmed()); base.setPath(path); base.setQuery(QString()); base.setFragment(QString()); return base;
    }
    QString requestId() const { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
    bool validBase() const {
        const QUrl url(base_->text().trimmed()); return url.isValid() && !url.host().isEmpty() && (url.scheme() == "http" || url.scheme() == "https");
    }
    void post(const QString& path, QJsonObject body, Callback callback) {
        if (!validBase()) { callback(false, {}, "网关地址无效"); return; }
        if (!body.contains("requestId")) body["requestId"] = requestId();
        QNetworkRequest request(endpoint(path)); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json"); request.setTransferTimeout(30000);
        auto* reply = network_.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [reply, callback = std::move(callback)] {
            const auto document = QJsonDocument::fromJson(reply->readAll());
            const auto object = document.object();
            const bool ok = reply->error() == QNetworkReply::NoError && document.isObject() && object.value("errorCode").toInt(-1) == 0;
            QString message = object.value("errorMsg").toString(); if (message.isEmpty() && !ok) message = reply->errorString();
            callback(ok, object, message); reply->deleteLater();
        });
    }
    void alert(const QString& message) { QMessageBox::warning(this, "提示", message); }
    bool haveSession() { if (!session_.isEmpty()) return true; alert("请先登录访客会话"); return false; }
    bool haveAccount() { if (!session_.isEmpty() && signed_in_) return true; alert("请先使用邮箱或密码登录"); return false; }
    void ensureSession(std::function<void()> ready) {
        if (!session_.isEmpty()) { ready(); return; }
        post("/HttpService/tempLogin", {}, [this, ready = std::move(ready)](bool ok, const QJsonObject& response, const QString& message) {
            if (!ok) { alert(message); return; }
            session_ = response.value("result").toObject().value("sessionId").toString();
            if (session_.isEmpty()) { alert("无法建立会话"); return; }
            status_->setText("访客会话"); ready();
        });
    }
    void displayAvatar(const QString& id) {
        avatar_label_->clear(); if (id.isEmpty() || !validBase()) return;
        QUrl url = endpoint("/HttpService/downloadPhoto"); QUrlQuery query; query.addQueryItem("fileId", id); url.setQuery(query);
        QNetworkRequest request(url); request.setTransferTimeout(30000);
        auto* reply = network_.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            const auto data = reply->readAll(); QPixmap pixmap;
            if (reply->error() == QNetworkReply::NoError && data.size() <= 5 * 1024 * 1024 && pixmap.loadFromData(data))
                avatar_label_->setPixmap(pixmap.scaled(80, 80, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            reply->deleteLater();
        });
    }
    void setupLogin() {
        auto* page = new QWidget(tabs_); auto* outer = new QVBoxLayout(page); outer->setContentsMargins(32, 28, 32, 28);
        auto* card = new QWidget(page); card->setObjectName("loginCard"); card->setMinimumWidth(440); card->setMaximumWidth(500); card->setStyleSheet("QWidget#loginCard { background: #1b1f2b; border: 1px solid #343949; border-radius: 14px; }");
        auto* form = new QFormLayout(card); form->setSpacing(14); form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setContentsMargins(26, 24, 26, 26);
        auto* heading = new QLabel("登录视频点播", card); heading->setStyleSheet("font-size: 24px; font-weight: 700; color: #f5f3ff;"); form->addRow(heading);
        auto* hint = new QLabel("使用邮箱验证码或密码登录，也可以先以访客身份浏览", card); hint->setWordWrap(true); hint->setStyleSheet("color: #a2a9bc; font-size: 13px;"); form->addRow(hint);
        outer->addWidget(card, 0, Qt::AlignHCenter); outer->addStretch();
        auto* modes = new QTabWidget(card); modes->setObjectName("loginModes");
        auto* emailPage = new QWidget(modes); auto* emailForm = new QFormLayout(emailPage); emailForm->setContentsMargins(16, 20, 16, 16); emailForm->setSpacing(12);
        email_ = new QLineEdit(emailPage); email_->setObjectName("email"); email_->setPlaceholderText("输入邮箱地址"); emailForm->addRow("邮箱", email_);
        auto* getCode = new QPushButton("获取验证码", emailPage); getCode->setObjectName("sendCode"); emailForm->addRow(getCode);
        code_ = new QLineEdit(emailPage); code_->setObjectName("verificationCode"); code_->setPlaceholderText("输入邮件中的验证码"); emailForm->addRow("验证码", code_);
        auto* login = new QPushButton("登录 / 注册", emailPage); login->setObjectName("emailLogin"); emailForm->addRow(login);
        auto* passwordPage = new QWidget(modes); auto* passwordForm = new QFormLayout(passwordPage); passwordForm->setContentsMargins(16, 20, 16, 16); passwordForm->setSpacing(12);
        username_ = new QLineEdit(passwordPage); username_->setObjectName("username"); username_->setPlaceholderText("输入邮箱或昵称"); passwordForm->addRow("账号", username_);
        login_password_ = new QLineEdit(passwordPage); login_password_->setObjectName("loginPassword"); login_password_->setEchoMode(QLineEdit::Password); login_password_->setPlaceholderText("输入登录密码"); passwordForm->addRow("密码", login_password_);
        auto* passwordLogin = new QPushButton("登录", passwordPage); passwordLogin->setObjectName("passwordLogin"); passwordForm->addRow(passwordLogin);
        modes->addTab(emailPage, "验证码登录"); modes->addTab(passwordPage, "密码登录"); form->addRow(modes);
        auto* guest = new QPushButton("先逛逛", card); guest->setObjectName("guestLogin");
        auto* logout = new QPushButton("退出登录", card); logout->setObjectName("logout"); form->addRow(guest, logout);
        connect(guest, &QPushButton::clicked, this, [this] {
            if (!session_.isEmpty()) { tabs_->setCurrentIndex(0); return; }
            post("/HttpService/tempLogin", {}, [this](bool ok, const QJsonObject& response, const QString& message) {
                if (!ok) { alert(message); return; }
                session_ = response.value("result").toObject().value("sessionId").toString(); code_id_.clear();
                status_->setText(session_.isEmpty() ? "会话无效" : "访客会话");
                if (!session_.isEmpty()) tabs_->setCurrentIndex(0);
            });
        });
        connect(getCode, &QPushButton::clicked, this, [this] {
            const auto email = email_->text().trimmed();
            if (email.isEmpty()) { alert("请输入邮箱"); return; }
            ensureSession([this, email] {
                post("/HttpService/getCode", {{"sessionId", session_}, {"email", email}}, [this, email](bool ok, const QJsonObject& response, const QString& message) {
                    if (!ok) { alert(message); return; }
                    code_id_ = response.value("result").toObject().value("codeId").toString(); code_email_ = email;
                    status_->setText("验证码已发送");
                });
            });
        });
        connect(login, &QPushButton::clicked, this, [this] {
            if (!haveSession()) return;
            if (code_id_.isEmpty() || code_email_ != email_->text().trimmed()) { alert("请先为当前邮箱获取验证码"); return; }
            post("/HttpService/vcodeLogin", {{"sessionId", session_}, {"email", code_email_}, {"codeId", code_id_}, {"verifyCode", code_->text().trimmed()}},
                [this](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } signed_in_ = true; code_id_.clear(); code_->clear(); status_->setText("已登录"); tabs_->setCurrentIndex(0); });
        });
        connect(passwordLogin, &QPushButton::clicked, this, [this] {
            ensureSession([this] {
                post("/HttpService/passwdLogin", {{"sessionId", session_}, {"username", username_->text().trimmed()}, {"password", login_password_->text()}},
                    [this](bool ok, const QJsonObject&, const QString& message) { login_password_->clear(); if (!ok) { alert(message); return; } signed_in_ = true; status_->setText("已登录"); tabs_->setCurrentIndex(0); });
            });
        });
        connect(logout, &QPushButton::clicked, this, [this] {
            if (!haveSession()) return;
            post("/HttpService/logout", {{"sessionId", session_}}, [this](bool ok, const QJsonObject&, const QString& message) {
                if (!ok) { alert(message); return; } session_.clear(); code_id_.clear(); signed_in_ = false; status_->setText("未登录");
            });
        });
        tabs_->addTab(page, "登录");
    }
    void setupHome(bool autoLoad) {
        auto* page = new QWidget(tabs_); auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 22, 24, 24); layout->setSpacing(14);
        auto* hero = new QWidget(page); hero->setObjectName("homeHero");
        hero->setStyleSheet("QWidget#homeHero { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #9b343c,stop:0.55 #512c32,stop:1 #28252b); border: 1px solid #9e494f; border-radius: 14px; }");
        auto* heroLayout = new QVBoxLayout(hero); heroLayout->setContentsMargins(25, 20, 25, 20); heroLayout->setSpacing(6);
        auto* eyebrow = new QLabel("YOUR VIDEO SPACE", hero); eyebrow->setStyleSheet("color: #ffcdce; font-size: 11px; font-weight: 700; letter-spacing: 2px; background: transparent;");
        auto* headline = new QLabel("发现下一段精彩", hero); headline->setStyleSheet("color: #ffffff; font-size: 26px; font-weight: 700; background: transparent;");
        auto* subtitle = new QLabel("探索公开作品，记录喜欢的瞬间。", hero); subtitle->setStyleSheet("color: #f3d4d5; font-size: 13px; background: transparent;");
        heroLayout->addWidget(eyebrow); heroLayout->addWidget(headline); heroLayout->addWidget(subtitle); layout->addWidget(hero);
        auto* searchRow = new QHBoxLayout;
        auto* search = new QLineEdit(page); search->setObjectName("videoSearch"); search->setPlaceholderText("搜索标题或简介"); searchRow->addWidget(search);
        auto* typeFilter = new QSpinBox(page); typeFilter->setObjectName("typeFilter"); typeFilter->setRange(-1, 65535); typeFilter->setValue(-1); typeFilter->setSpecialValueText("全部分类"); searchRow->addWidget(typeFilter);
        auto* tagFilter = new QSpinBox(page); tagFilter->setObjectName("tagFilter"); tagFilter->setRange(-1, 65535); tagFilter->setValue(-1); tagFilter->setSpecialValueText("全部标签"); searchRow->addWidget(tagFilter);
        auto* pageNumber = new QSpinBox(page); pageNumber->setObjectName("videoPage"); pageNumber->setRange(1, 1000000); pageNumber->setPrefix("第 "); pageNumber->setSuffix(" 页"); searchRow->addWidget(pageNumber);
        auto* refresh = new QPushButton("搜索 / 刷新视频", page); refresh->setObjectName("refreshPublic"); searchRow->addWidget(refresh); layout->addLayout(searchRow);
        auto* sectionRow = new QHBoxLayout;
        auto* section = new QLabel("视频库", page); section->setStyleSheet("font-size: 17px; font-weight: 700; color: #f1eff9;"); sectionRow->addWidget(section);
        auto* resultLabel = new QLabel("双击卡片开始播放", page); resultLabel->setStyleSheet("color: #9da6ba; font-size: 12px;"); sectionRow->addStretch(); sectionRow->addWidget(resultLabel); layout->addLayout(sectionRow);
        table_ = new QTableWidget(0, 2, page); table_->setHorizontalHeaderLabels({"标题", "简介"});
        table_->setObjectName("publicVideos");
        table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setAlternatingRowColors(true); table_->verticalHeader()->setVisible(false); table_->verticalHeader()->setDefaultSectionSize(92); table_->setIconSize(QSize(128, 72));
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->hide();
        auto* cards = new QListWidget(page); cards->setObjectName("videoCards"); cards->setViewMode(QListView::IconMode);
        cards->setMovement(QListView::Static); cards->setResizeMode(QListView::Adjust); cards->setWrapping(true);
        cards->setIconSize(QSize(176, 99)); cards->setGridSize(QSize(202, 176)); cards->setSpacing(8); cards->setWordWrap(true);
        layout->addWidget(cards, 1);
        connect(cards, &QListWidget::currentRowChanged, table_, [this](int row) { if (row >= 0 && row < table_->rowCount()) table_->selectRow(row); });
        connect(cards, &QListWidget::itemDoubleClicked, this, [this, cards](QListWidgetItem*) {
            const int row = cards->currentRow(); if (row >= 0) emit table_->cellDoubleClicked(row, 0);
        });
        auto* interactions = new QHBoxLayout;
        auto* play = new QPushButton("▶  播放选中视频", page); play->setObjectName("playSelected"); interactions->addWidget(play);
        auto* like = new QPushButton("点赞 / 取消", page); like->setObjectName("toggleLike"); interactions->addWidget(like);
        auto* follow = new QPushButton("关注作者", page); follow->setObjectName("followAuthor"); interactions->addWidget(follow);
        auto* unfollow = new QPushButton("取消关注", page); unfollow->setObjectName("unfollowAuthor"); interactions->addWidget(unfollow);
        auto* loadBarrage = new QPushButton("查看弹幕", page); loadBarrage->setObjectName("loadBarrage"); interactions->addWidget(loadBarrage);
        interactions->addStretch(); layout->addLayout(interactions);
        auto* compose = new QHBoxLayout;
        auto* barrageText = new QLineEdit(page); barrageText->setObjectName("barrageText"); barrageText->setPlaceholderText("写一条弹幕，分享此刻的想法"); compose->addWidget(barrageText, 1);
        auto* seconds = new QSpinBox(page); seconds->setObjectName("barrageSeconds"); seconds->setRange(0, 86400); seconds->setSuffix(" 秒"); compose->addWidget(seconds);
        auto* sendBarrage = new QPushButton("发送弹幕", page); sendBarrage->setObjectName("sendBarrage"); compose->addWidget(sendBarrage);
        layout->addLayout(compose);
        auto* barrageList = new QTextEdit(page); barrageList->setObjectName("barrageList"); barrageList->setReadOnly(true); barrageList->setMaximumHeight(100); barrageList->hide(); layout->addWidget(barrageList);
        auto selected = [this]() { auto* item = table_->item(table_->currentRow(), 0); return item ? item->data(Qt::UserRole + 1).toString() : QString(); };
        auto changeFollow = [this](bool enabled) {
            auto* item = table_->item(table_->currentRow(), 0); const auto author = item ? item->data(Qt::UserRole + 2).toString() : QString();
            if (author.isEmpty()) { alert("请先选择视频"); return; } if (!haveAccount()) return;
            post(enabled ? "/HttpService/newAttention" : "/HttpService/delAttention", {{"sessionId", session_}, {"userId", author}},
                [this, enabled](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } status_->setText(enabled ? "已关注作者" : "已取消关注"); });
        };
        connect(follow, &QPushButton::clicked, this, [changeFollow] { changeFollow(true); });
        connect(unfollow, &QPushButton::clicked, this, [changeFollow] { changeFollow(false); });
        connect(like, &QPushButton::clicked, this, [this, selected] {
            const auto id = selected(); if (id.isEmpty()) { alert("请先选择视频"); return; } if (!haveAccount()) return;
            post("/HttpService/setLike", {{"sessionId", session_}, {"videoId", id}}, [this](bool ok, const QJsonObject& response, const QString& message) {
                if (!ok) { alert(message); return; }
                const auto result = response.value("result").toObject();
                status_->setText(QString("%1，点赞 %2").arg(result.value("isLike").toBool() ? "已点赞" : "已取消点赞").arg(result.value("likeCount").toInt()));
            });
        });
        connect(loadBarrage, &QPushButton::clicked, this, [this, selected, barrageList] {
            const auto id = selected(); if (id.isEmpty()) { alert("请先选择视频"); return; }
            post("/HttpService/getBarrage", {{"videoId", id}}, [this, barrageList](bool ok, const QJsonObject& response, const QString& message) {
                if (!ok) { alert(message); return; } barrageList->clear(); barrageList->show();
                for (const auto& entry : response.value("result").toObject().value("barrageList").toArray()) {
                    const auto item = entry.toObject(); barrageList->append(QString("%1 秒：%2").arg(item.value("barrageTime").toInt()).arg(item.value("barrageContent").toString().toHtmlEscaped()));
                }
            });
        });
        connect(sendBarrage, &QPushButton::clicked, this, [this, selected, barrageText, seconds] {
            const auto id = selected(); if (id.isEmpty()) { alert("请先选择视频"); return; } if (!haveAccount()) return;
            QJsonObject info{{"barrageContent", barrageText->text()}, {"barrageTime", seconds->value()}};
            post("/HttpService/newBarrage", {{"sessionId", session_}, {"videoId", id}, {"barrageInfo", info}}, [this, barrageText](bool ok, const QJsonObject&, const QString& message) {
                if (!ok) { alert(message); return; } barrageText->clear(); status_->setText("弹幕已发送");
            });
        });
        connect(search, &QLineEdit::returnPressed, refresh, &QPushButton::click);
        connect(refresh, &QPushButton::clicked, this, [this, search, pageNumber, typeFilter, tagFilter, cards, resultLabel] {
            if (typeFilter->value() >= 0 && tagFilter->value() >= 0) { alert("请选择分类或标签之一"); return; }
            const auto requestGeneration = ++search_generation_;
            resultLabel->setText("正在加载…");
            QJsonObject body{{"sessionId", session_}, {"pageIndex", pageNumber->value()}, {"pageCount", 20}, {"searchKey", search->text()}};
            QString path = "/HttpService/keyVideoList";
            if (typeFilter->value() >= 0) { path = "/HttpService/typeVideoList"; body["videoTypeId"] = typeFilter->value(); }
            if (tagFilter->value() >= 0) { path = "/HttpService/tagVideoList"; body["videoTag"] = tagFilter->value(); }
            post(path, body,
                [this, cards, resultLabel, requestGeneration](bool ok, const QJsonObject& response, const QString& message) {
                    if (requestGeneration != search_generation_) return;
                    if (!ok) { resultLabel->setText("加载失败：" + message); return; }
                    auto videos = response.value("result").toObject().value("videoList").toArray(); table_->setRowCount(videos.size()); cards->clear();
                    resultLabel->setText(videos.isEmpty() ? "没有找到视频，换个条件试试" : QString("找到 %1 个视频 · 双击开始播放").arg(response.value("result").toObject().value("totalCount").toInt(videos.size())));
                    for (int i = 0; i < videos.size(); ++i) {
                        const auto video = videos.at(i).toObject();
                        auto* title = new QTableWidgetItem(video.value("videoTitle").toString());
                        QPixmap placeholder(176, 99); placeholder.fill(Qt::transparent);
                        {
                            QPainter painter(&placeholder); painter.setRenderHint(QPainter::Antialiasing);
                            const int hue = static_cast<int>(qHash(video.value("videoId").toString()) % 360);
                            QLinearGradient gradient(0, 0, 176, 99); gradient.setColorAt(0, QColor::fromHsv(hue, 120, 115)); gradient.setColorAt(1, QColor("#1e2538"));
                            painter.setPen(Qt::NoPen); painter.setBrush(gradient); painter.drawRoundedRect(placeholder.rect(), 7, 7);
                            painter.setBrush(QColor(255, 255, 255, 35)); painter.drawEllipse(QPoint(88, 49), 22, 22);
                            painter.setPen(Qt::white); QFont font = painter.font(); font.setPixelSize(20); painter.setFont(font); painter.drawText(placeholder.rect().adjusted(3, 0, 0, 0), Qt::AlignCenter, "▶");
                        }
                        title->setIcon(QIcon(placeholder));
                        title->setData(Qt::UserRole, video.value("videoFileId").toString()); table_->setItem(i, 0, title);
                        title->setData(Qt::UserRole + 1, video.value("videoId").toString());
                        title->setData(Qt::UserRole + 2, video.value("userId").toString());
                        table_->setItem(i, 1, new QTableWidgetItem(video.value("videoDesc").toString()));
                        const auto plays = static_cast<qint64>(video.value("playCount").toDouble());
                        auto* card = new QListWidgetItem(title->icon(), title->text() + QString("\n%1 次播放").arg(plays)); card->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                        card->setToolTip(video.value("videoDesc").toString()); cards->addItem(card);
                        const auto cover = video.value("photoFileId").toString(), videoId = video.value("videoId").toString();
                        if (!cover.isEmpty()) {
                            QUrl url = endpoint("/HttpService/downloadPhoto"); QUrlQuery query; query.addQueryItem("fileId", cover); url.setQuery(query);
                            QNetworkRequest request(url); request.setTransferTimeout(15000); auto* reply = network_.get(request);
                            connect(reply, &QNetworkReply::finished, this, [this, cards, reply, i, videoId] {
                                const auto data = reply->readAll(); QPixmap image;
                                auto* item = i < table_->rowCount() ? table_->item(i, 0) : nullptr;
                                if (item && item->data(Qt::UserRole + 1).toString() == videoId && reply->error() == QNetworkReply::NoError &&
                                    data.size() <= 5 * 1024 * 1024 && image.loadFromData(data))
                                {
                                    const auto icon = QIcon(image.scaled(176, 99, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
                                    item->setIcon(icon); if (auto* card = cards->item(i)) card->setIcon(icon);
                                }
                                reply->deleteLater();
                            });
                        }
                    }
                });
        });
        connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
            auto* item = table_->item(row, 0); if (!item) return;
            const auto id = item->data(Qt::UserRole + 1).toString();
            if (!id.isEmpty()) post("/HttpService/setPlay", {{"videoId", id}}, [](bool, const QJsonObject&, const QString&) {});
            QUrl url = endpoint("/HttpService/downloadVideo"); QUrlQuery query; query.addQueryItem("fileId", item->data(Qt::UserRole).toString()); url.setQuery(query);
            player_->play(url, item->text()); tabs_->setCurrentWidget(player_);
        });
        connect(play, &QPushButton::clicked, this, [this] {
            const int row = table_->currentRow(); if (row < 0) { alert("请选择要播放的视频"); return; }
            emit table_->cellDoubleClicked(row, 0);
        });
        tabs_->addTab(page, "首页");
        if (autoLoad) QTimer::singleShot(0, refresh, &QPushButton::click);
    }
    void setupUpload() {
        auto* page = new QWidget(tabs_); auto* form = new QFormLayout(page);
        form->setContentsMargins(34, 26, 34, 30); form->setSpacing(14); form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        auto* heading = new QLabel("发布新作品", page); heading->setStyleSheet("font-size: 22px; font-weight: 700; color: #f3f0ff;"); form->addRow(heading);
        auto* hint = new QLabel("填写信息并选择远程工作区中的视频；本机文件可先拖入 VS Code 远程文件夹。处理后进入审核。", page); hint->setWordWrap(true); hint->setStyleSheet("color: #a2a9bc; font-size: 13px;"); form->addRow(hint);
        title_ = new QLineEdit(page); title_->setObjectName("videoTitle"); description_ = new QTextEdit(page); description_->setObjectName("videoDescription");
        title_->setPlaceholderText("给作品起一个标题"); description_->setPlaceholderText("介绍一下这段视频"); description_->setMaximumHeight(110);
        form->addRow("标题", title_); form->addRow("简介", description_);
        auto* type = new QSpinBox(page); type->setObjectName("videoType"); type->setRange(0, 65535); form->addRow("分类编号", type);
        auto* duration = new QSpinBox(page); duration->setObjectName("videoDuration"); duration->setRange(0, 86400); duration->setSuffix(" 秒"); form->addRow("时长", duration);
        auto* tags = new QLineEdit(page); tags->setObjectName("videoTags"); tags->setPlaceholderText("用英文逗号分隔编号，例如 1,2"); form->addRow("标签编号", tags);
        auto* choose = new QPushButton("选择视频文件", page); file_path_ = new QLineEdit(page); file_path_->setObjectName("videoPath"); form->addRow(choose, file_path_);
        auto* chooseCover = new QPushButton("选择封面（可选）", page); auto* coverPath = new QLineEdit(page); coverPath->setObjectName("coverPath"); form->addRow(chooseCover, coverPath);
        progress_ = new QProgressBar(page); form->addRow("上传进度", progress_);
        auto* upload = new QPushButton("上传并提交", page); upload->setObjectName("uploadVideo"); form->addRow(upload);
        uploaded_id_ = new QLineEdit(page); uploaded_id_->setObjectName("uploadedVideoId"); uploaded_id_->setReadOnly(true); form->addRow("视频 ID", uploaded_id_);
        connect(choose, &QPushButton::clicked, this, [this] { auto name = QFileDialog::getOpenFileName(this, "选择视频"); if (!name.isEmpty()) file_path_->setText(name); });
        connect(chooseCover, &QPushButton::clicked, this, [this, coverPath] { auto name = QFileDialog::getOpenFileName(this, "选择封面", {}, "图片 (*.png *.jpg *.jpeg *.webp)"); if (!name.isEmpty()) coverPath->setText(name); });
        connect(upload, &QPushButton::clicked, this, [this, type, duration, tags, coverPath, upload] {
            if (!haveAccount() || !validBase()) return;
            QJsonArray tagValues; const auto parts = tags->text().split(',', Qt::SkipEmptyParts);
            if (parts.size() > 20) { alert("最多输入 20 个标签"); return; }
            for (const auto& part : parts) { bool valid = false; const int number = part.trimmed().toInt(&valid); if (!valid || number < 0 || number > 65535) { alert("标签编号应在 0 到 65535 之间"); return; } tagValues.append(number); }
            const int videoType = type->value(), videoDuration = duration->value();
            QByteArray coverData;
            if (!coverPath->text().trimmed().isEmpty()) {
                QFile cover(coverPath->text()); QImageReader reader(coverPath->text());
                if (!cover.open(QIODevice::ReadOnly) || cover.size() <= 0 || cover.size() > 5LL * 1024 * 1024 || !reader.canRead()) { alert("请选择不超过 5 MiB 的有效图片作为封面"); return; }
                coverData = cover.readAll();
            }
            QFile file(file_path_->text()); if (!file.open(QIODevice::ReadOnly)) { alert("无法打开视频文件"); return; }
            if (file.size() <= 0 || file.size() > 128LL * 1024 * 1024) { alert("请选择不超过 128 MiB 的非空视频"); return; }
            const auto title = title_->text(); const auto description = description_->toPlainText(); const auto session = session_;
            if (title.isEmpty()) { alert("请输入标题"); return; }
            upload->setEnabled(false); uploaded_id_->clear(); progress_->setValue(0); status_->setText("正在上传视频…");
            QUrl url = endpoint("/HttpService/uploadVideo"); QUrlQuery query;
            query.addQueryItem("requestId", requestId()); query.addQueryItem("sessionId", session); url.setQuery(query);
            QNetworkRequest request(url); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream"); request.setTransferTimeout(120000);
            auto* stream = new QFile(file.fileName());
            if (!stream->open(QIODevice::ReadOnly)) { delete stream; upload->setEnabled(true); status_->setText("上传失败"); alert("无法读取视频文件"); return; }
            request.setHeader(QNetworkRequest::ContentLengthHeader, stream->size());
            auto* reply = network_.post(request, stream); stream->setParent(reply);
            connect(reply, &QNetworkReply::uploadProgress, this, [this](qint64 sent, qint64 total) { if (total > 0) progress_->setValue(static_cast<int>(100 * sent / total)); });
            connect(reply, &QNetworkReply::finished, this, [this, reply, title, description, session, videoType, videoDuration, tagValues, coverData, upload] {
                auto document = QJsonDocument::fromJson(reply->readAll()); auto object = document.object();
                if (reply->error() != QNetworkReply::NoError || object.value("errorCode").toInt(-1) != 0) {
                    auto message = object.value("errorMsg").toString(); upload->setEnabled(true); status_->setText("上传失败"); alert(message.isEmpty() ? reply->errorString() : message); reply->deleteLater(); return;
                }
                const auto file_id = object.value("result").toObject().value("fileId").toString(); reply->deleteLater();
                QJsonObject info{{"videoFileId", file_id}, {"videoTitle", title}, {"videoDesc", description}, {"videoType", videoType}, {"duration", videoDuration}, {"videoTag", tagValues}};
                auto submit = [this, session, upload](QJsonObject videoInfo) {
                    status_->setText("正在提交视频…");
                    post("/HttpService/newVideo", {{"sessionId", session}, {"videoInfo", videoInfo}}, [this, upload](bool ok, const QJsonObject& response, const QString& message) {
                        upload->setEnabled(true);
                        if (!ok) { status_->setText("提交失败"); alert(message); return; }
                        uploaded_id_->setText(response.value("result").toObject().value("videoId").toString()); status_->setText("视频已提交处理");
                    });
                };
                if (coverData.isEmpty()) { submit(info); return; }
                QUrl coverUrl = endpoint("/HttpService/uploadPhoto"); QUrlQuery query;
                query.addQueryItem("requestId", requestId()); query.addQueryItem("sessionId", session); coverUrl.setQuery(query);
                QNetworkRequest coverRequest(coverUrl); coverRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream"); coverRequest.setTransferTimeout(30000);
                auto* coverReply = network_.post(coverRequest, coverData);
                status_->setText("正在上传封面…");
                connect(coverReply, &QNetworkReply::finished, this, [this, coverReply, info, submit, upload] () mutable {
                    const auto response = QJsonDocument::fromJson(coverReply->readAll()).object();
                    if (coverReply->error() != QNetworkReply::NoError || response.value("errorCode").toInt(-1) != 0) {
                        auto message = response.value("errorMsg").toString(); upload->setEnabled(true); status_->setText("封面上传失败"); alert(message.isEmpty() ? coverReply->errorString() : message); coverReply->deleteLater(); return;
                    }
                    info["photoFileId"] = response.value("result").toObject().value("fileId").toString(); coverReply->deleteLater(); submit(info);
                });
            });
        });
        tabs_->addTab(page, "上传");
    }
    void setupMyVideos() {
        auto* page = new QWidget(tabs_); auto* layout = new QVBoxLayout(page);
        auto* refresh = new QPushButton("刷新我的视频", page); refresh->setObjectName("refreshMyVideos"); layout->addWidget(refresh);
        auto* table = new QTableWidget(0, 3, page); table->setObjectName("myVideos"); table->setHorizontalHeaderLabels({"标题", "状态", "视频 ID"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); table->setEditTriggers(QAbstractItemView::NoEditTriggers); layout->addWidget(table);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        auto* actions = new QHBoxLayout;
        auto* halt = new QPushButton("下架选中视频", page); halt->setObjectName("haltVideo"); actions->addWidget(halt);
        auto* resubmit = new QPushButton("重新提交审核", page); resubmit->setObjectName("resubmitVideo"); actions->addWidget(resubmit); layout->addLayout(actions);
        auto* remove = new QPushButton("删除选中视频", page); remove->setObjectName("removeVideo"); actions->addWidget(remove);
        auto change = [this, table](bool submit) {
            auto* item = table->item(table->currentRow(), 2); if (!item) { alert("请先选择视频"); return; } if (!haveAccount()) return;
            post(submit ? "/HttpService/saleVideo" : "/HttpService/haltVideo", {{"sessionId", session_}, {"videoId", item->text()}},
                [this, submit](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } status_->setText(submit ? "已重新提交审核" : "视频已下架"); });
        };
        connect(halt, &QPushButton::clicked, this, [change] { change(false); });
        connect(resubmit, &QPushButton::clicked, this, [change] { change(true); });
        connect(remove, &QPushButton::clicked, this, [this, table] {
            auto* item = table->item(table->currentRow(), 2); if (!item) { alert("请先选择视频"); return; } if (!haveAccount()) return;
            if (QMessageBox::question(this, "删除视频", "确定删除选中的视频？删除后将从列表隐藏。") != QMessageBox::Yes) return;
            post("/HttpService/removeVideo", {{"sessionId", session_}, {"videoId", item->text()}},
                [this](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } status_->setText("视频已删除"); });
        });
        connect(refresh, &QPushButton::clicked, this, [this, table] {
            if (!haveAccount()) return;
            post("/HttpService/userVideoList", {{"sessionId", session_}, {"pageIndex", 1}, {"pageCount", 100}},
                [this, table](bool ok, const QJsonObject& response, const QString& message) {
                    if (!ok) { alert(message); return; }
                    const auto videos = response.value("result").toObject().value("videoList").toArray(); table->setRowCount(videos.size());
                    const QStringList states{"未知", "待审核", "已发布", "已驳回", "已下架", "处理中", "处理失败"};
                    for (int i = 0; i < videos.size(); ++i) {
                        const auto video = videos.at(i).toObject(); const int state = video.value("videoStatus").toInt();
                        table->setItem(i, 0, new QTableWidgetItem(video.value("videoTitle").toString()));
                        table->setItem(i, 1, new QTableWidgetItem(state >= 1 && state < states.size() ? states[state] : states[0]));
                        table->setItem(i, 2, new QTableWidgetItem(video.value("videoId").toString()));
                    }
                });
        });
        tabs_->addTab(page, "我的视频");
    }
    void setupReview() {
        auto* page = new QWidget(tabs_); auto* form = new QFormLayout(page);
        auto* refresh = new QPushButton("刷新待审核视频", page); refresh->setObjectName("refreshReview"); form->addRow(refresh);
        review_table_ = new QTableWidget(0, 2, page); review_table_->setHorizontalHeaderLabels({"标题", "视频 ID"});
        review_table_->setObjectName("reviewVideos");
        review_table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
        review_table_->setSelectionBehavior(QAbstractItemView::SelectRows); review_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        form->addRow(review_table_);
        review_id_ = new QLineEdit(page); review_id_->setObjectName("reviewVideoId"); form->addRow("待审核视频 ID", review_id_);
        auto* preview = new QPushButton("预览选中视频", page); form->addRow(preview);
        auto* approve = new QPushButton("审核通过", page); approve->setObjectName("approveVideo"); auto* reject = new QPushButton("审核驳回", page);
        form->addRow(approve, reject);
        auto review = [this](bool approved) {
            if (!haveAccount()) return;
            post("/HttpService/checkVideo", {{"sessionId", session_}, {"videoId", review_id_->text().trimmed()}, {"checkResult", approved}},
                [this](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } status_->setText("审核完成"); });
        };
        connect(approve, &QPushButton::clicked, this, [review] { review(true); });
        connect(reject, &QPushButton::clicked, this, [review] { review(false); });
        connect(refresh, &QPushButton::clicked, this, [this] {
            if (!haveAccount()) return;
            post("/HttpService/statusVideoList", {{"sessionId", session_}, {"videoStatus", 1}, {"pageIndex", 1}, {"pageCount", 20}},
                [this](bool ok, const QJsonObject& response, const QString& message) {
                    if (!ok) { alert(message); return; }
                    const auto videos = response.value("result").toObject().value("videoList").toArray();
                    review_table_->setRowCount(videos.size());
                    for (int i = 0; i < videos.size(); ++i) {
                        const auto video = videos.at(i).toObject();
                        auto* title = new QTableWidgetItem(video.value("videoTitle").toString());
                        title->setData(Qt::UserRole, video.value("videoFileId").toString());
                        review_table_->setItem(i, 0, title); review_table_->setItem(i, 1, new QTableWidgetItem(video.value("videoId").toString()));
                    }
                });
        });
        connect(review_table_, &QTableWidget::cellClicked, this, [this](int row, int) {
            if (auto* item = review_table_->item(row, 1)) review_id_->setText(item->text());
        });
        connect(preview, &QPushButton::clicked, this, [this] {
            const int row = review_table_->currentRow(); auto* item = review_table_->item(row, 0);
            if (!item || item->data(Qt::UserRole).toString().isEmpty()) { alert("请选择可预览的视频"); return; }
            QUrl url = endpoint("/HttpService/downloadVideo"); QUrlQuery query;
            query.addQueryItem("fileId", item->data(Qt::UserRole).toString()); query.addQueryItem("sessionId", session_); url.setQuery(query);
            player_->play(url); tabs_->setCurrentWidget(player_);
        });
        tabs_->addTab(page, "审核");
    }
    void setupAdmins() {
        auto* page = new QWidget(tabs_); auto* form = new QFormLayout(page);
        auto* email = new QLineEdit(page); email->setObjectName("adminEmail"); form->addRow("已注册邮箱", email);
        auto* id = new QLineEdit(page); id->setObjectName("adminId"); form->addRow("管理员 ID", id);
        auto* nickname = new QLineEdit(page); nickname->setObjectName("adminNickname"); form->addRow("管理端昵称", nickname);
        auto* memo = new QLineEdit(page); memo->setObjectName("adminMemo"); form->addRow("备注", memo);
        auto* enabled = new QCheckBox("启用管理权限", page); enabled->setObjectName("adminEnabled"); enabled->setChecked(true); form->addRow(enabled);
        auto* add = new QPushButton("添加管理员", page); add->setObjectName("addAdmin");
        auto* edit = new QPushButton("保存管理员", page); edit->setObjectName("editAdmin"); form->addRow(add, edit);
        auto* remove = new QPushButton("移除管理权限", page); remove->setObjectName("removeAdmin");
        auto* refresh = new QPushButton("刷新管理员列表", page); refresh->setObjectName("refreshAdmins"); form->addRow(remove, refresh);
        auto* table = new QTableWidget(0, 3, page); table->setObjectName("adminTable"); table->setHorizontalHeaderLabels({"邮箱", "昵称", "状态"});
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch); table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers); form->addRow(table);
        auto save = [this, email, id, nickname, memo, enabled](bool adding) {
            if (!haveAccount()) return;
            QJsonObject info{{"nickname", nickname->text()}, {"userMemo", memo->text()}, {"userStatus", enabled->isChecked() ? 1 : 0}};
            if (adding) { info["email"] = email->text().trimmed(); info["roleType"] = 2; } else info["userId"] = id->text().trimmed();
            post(adding ? "/HttpService/newAdministrator" : "/HttpService/setAdministrator", {{"sessionId", session_}, {"userInfo", info}},
                [this](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } status_->setText("管理员设置已保存"); });
        };
        connect(add, &QPushButton::clicked, this, [save] { save(true); });
        connect(edit, &QPushButton::clicked, this, [save] { save(false); });
        connect(remove, &QPushButton::clicked, this, [this, id] {
            if (!haveAccount()) return;
            post("/HttpService/delAdministrator", {{"sessionId", session_}, {"userId", id->text().trimmed()}},
                [this](bool ok, const QJsonObject&, const QString& message) { if (!ok) { alert(message); return; } status_->setText("管理权限已移除"); });
        });
        connect(refresh, &QPushButton::clicked, this, [this, table] {
            if (!haveAccount()) return;
            post("/HttpService/getAdminListByStatus", {{"sessionId", session_}, {"pageIndex", 1}, {"pageCount", 100}},
                [this, table](bool ok, const QJsonObject& response, const QString& message) {
                    if (!ok) { alert(message); return; } const auto list = response.value("result").toObject().value("userList").toArray(); table->setRowCount(list.size());
                    for (int i = 0; i < list.size(); ++i) {
                        const auto admin = list.at(i).toObject(); auto* item = new QTableWidgetItem(admin.value("email").toString());
                        item->setData(Qt::UserRole, admin.value("userId").toString()); item->setData(Qt::UserRole + 1, admin.value("userMemo").toString());
                        item->setData(Qt::UserRole + 2, admin.value("userStatus").toInt()); table->setItem(i, 0, item);
                        table->setItem(i, 1, new QTableWidgetItem(admin.value("nickname").toString()));
                        table->setItem(i, 2, new QTableWidgetItem(admin.value("userStatus").toInt() == 1 ? "启用" : "停用"));
                    }
                });
        });
        connect(table, &QTableWidget::cellClicked, this, [table, email, id, nickname, memo, enabled](int row, int) {
            auto* item = table->item(row, 0); if (!item) return;
            email->setText(item->text()); id->setText(item->data(Qt::UserRole).toString()); memo->setText(item->data(Qt::UserRole + 1).toString());
            enabled->setChecked(item->data(Qt::UserRole + 2).toInt() == 1); if (auto* name = table->item(row, 1)) nickname->setText(name->text());
        });
        tabs_->addTab(page, "管理员");
    }
    void setupProfile() {
        auto* page = new QWidget(tabs_); auto* form = new QFormLayout(page);
        auto* refresh = new QPushButton("刷新资料", page); refresh->setObjectName("refreshProfile"); form->addRow(refresh);
        profile_email_ = new QLabel(page); profile_email_->setObjectName("profileEmail"); form->addRow("邮箱", profile_email_);
        avatar_label_ = new QLabel(page); avatar_label_->setObjectName("avatarPreview"); avatar_label_->setFixedSize(80, 80); form->addRow("头像", avatar_label_);
        auto* avatarPath = new QLineEdit(page); avatarPath->setObjectName("avatarPath");
        auto* chooseAvatar = new QPushButton("选择头像", page); form->addRow(chooseAvatar, avatarPath);
        auto* saveAvatar = new QPushButton("上传头像", page); saveAvatar->setObjectName("saveAvatar"); form->addRow(saveAvatar);
        nickname_ = new QLineEdit(page); nickname_->setObjectName("nickname"); form->addRow("昵称", nickname_);
        auto* setNickname = new QPushButton("保存昵称", page); setNickname->setObjectName("setNickname"); form->addRow(setNickname);
        new_password_ = new QLineEdit(page); new_password_->setObjectName("newPassword"); new_password_->setEchoMode(QLineEdit::Password); form->addRow("新密码", new_password_);
        auto* setPassword = new QPushButton("设置密码", page); setPassword->setObjectName("setPassword"); form->addRow(setPassword);
        connect(chooseAvatar, &QPushButton::clicked, this, [this, avatarPath] {
            const auto path = QFileDialog::getOpenFileName(this, "选择头像", {}, "图片 (*.png *.jpg *.jpeg *.webp)"); if (!path.isEmpty()) avatarPath->setText(path);
        });
        connect(saveAvatar, &QPushButton::clicked, this, [this, avatarPath] {
            if (!haveAccount() || !validBase()) return;
            QFile file(avatarPath->text()); QImageReader reader(avatarPath->text());
            if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 5LL * 1024 * 1024 || !reader.canRead()) { alert("请选择不超过 5 MiB 的有效图片"); return; }
            const auto session = session_; QUrl url = endpoint("/HttpService/uploadPhoto"); QUrlQuery query;
            query.addQueryItem("requestId", requestId()); query.addQueryItem("sessionId", session); url.setQuery(query);
            QNetworkRequest request(url); request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream"); request.setTransferTimeout(30000);
            auto* reply = network_.post(request, file.readAll());
            connect(reply, &QNetworkReply::finished, this, [this, reply, session] {
                auto response = QJsonDocument::fromJson(reply->readAll()).object();
                if (reply->error() != QNetworkReply::NoError || response.value("errorCode").toInt(-1) != 0) {
                    const auto message = response.value("errorMsg").toString(); alert(message.isEmpty() ? reply->errorString() : message); reply->deleteLater(); return;
                }
                const auto id = response.value("result").toObject().value("fileId").toString(); reply->deleteLater();
                post("/HttpService/setAvatar", {{"sessionId", session}, {"fileId", id}}, [this, id](bool ok, const QJsonObject&, const QString& message) {
                    if (!ok) { alert(message); return; } status_->setText("头像已保存"); displayAvatar(id);
                });
            });
        });
        connect(refresh, &QPushButton::clicked, this, [this] {
            if (!haveAccount()) return;
            post("/HttpService/getUserInfo", {{"sessionId", session_}}, [this](bool ok, const QJsonObject& response, const QString& message) {
                if (!ok) { alert(message); return; }
                const auto info = response.value("result").toObject().value("userInfo").toObject();
                profile_email_->setText(info.value("email").toString()); nickname_->setText(info.value("nickname").toString());
                displayAvatar(info.value("userAvatarId").toString());
            });
        });
        connect(setNickname, &QPushButton::clicked, this, [this] {
            if (!haveAccount()) return;
            post("/HttpService/setNickname", {{"sessionId", session_}, {"nickname", nickname_->text()}}, [this](bool ok, const QJsonObject&, const QString& message) {
                if (!ok) { alert(message); return; } status_->setText("昵称已保存");
            });
        });
        connect(setPassword, &QPushButton::clicked, this, [this] {
            if (!haveAccount()) return;
            post("/HttpService/setPassword", {{"sessionId", session_}, {"password", new_password_->text()}}, [this](bool ok, const QJsonObject&, const QString& message) {
                new_password_->clear(); if (!ok) { alert(message); return; } status_->setText("密码已设置");
            });
        });
        tabs_->addTab(page, "我的资料");
    }
    QNetworkAccessManager network_; QTabWidget* tabs_ = nullptr; Player* player_ = nullptr;
    QLineEdit *base_ = nullptr, *email_ = nullptr, *code_ = nullptr, *title_ = nullptr, *review_id_ = nullptr, *uploaded_id_ = nullptr, *file_path_ = nullptr;
    QLineEdit *username_ = nullptr, *login_password_ = nullptr, *nickname_ = nullptr, *new_password_ = nullptr;
    QTextEdit* description_ = nullptr; QLabel *status_ = nullptr, *profile_email_ = nullptr, *avatar_label_ = nullptr;
    QProgressBar* progress_ = nullptr; QTableWidget *table_ = nullptr, *review_table_ = nullptr;
    QString session_, code_id_, code_email_; bool signed_in_ = false;
    quint64 search_generation_ = 0;
};

#ifndef VOD_CLIENT_NO_MAIN
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const bool playTest = app.arguments().contains("--self-test-play");
    const bool test = app.arguments().contains("--self-test") || playTest;
    Window window(test);
    if (test) {
        if (!window.playerAvailable()) return 1;
        if (playTest) {
            const int index = app.arguments().indexOf("--self-test-play");
            if (index + 1 >= app.arguments().size()) return 2;
            window.playTest(app.arguments().at(index + 1));
            QTimer poll; QObject::connect(&poll, &QTimer::timeout, &app, [&] {
                if (window.playerState() == 2) app.exit(0);
                else if (window.playerState() < 0) app.exit(1);
            });
            poll.start(100);
            QTimer::singleShot(15000, &app, [&app] { app.exit(1); });
            return app.exec();
        }
        QTimer::singleShot(250, &app, &QCoreApplication::quit);
    }
    else window.show();
    return app.exec();
}
#endif
