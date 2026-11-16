#define VOD_CLIENT_NO_MAIN
#include "../client/main.cpp"
#include <QtTest/QTest>
#include <QtWidgets/QAbstractButton>
#include <functional>
#include <iostream>

template <typename T> T* get(Window& window, const char* name) {
    auto* item = window.findChild<T*>(name);
    if (!item) { std::cerr << "missing UI control: " << name << '\n'; std::exit(1); }
    return item;
}
void wait_for(const std::function<bool()>& condition, const char* description) {
    for (int i = 0; i < 200 && !condition(); ++i) QTest::qWait(50);
    if (!condition()) { std::cerr << "UI timeout: " << description << '\n'; std::exit(1); }
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    if (argc != 3) return 2;
    Window window(true); window.show();
    auto* navigation = get<QListWidget>(window, "mainNavigation");
    auto* pages = window.findChild<QTabWidget*>();
    if (!pages || navigation->count() != pages->count() || navigation->count() < 8) return 1;
    navigation->setCurrentRow(1);
    if (pages->currentIndex() != 1) return 1;
    pages->setCurrentIndex(0);
    if (navigation->currentRow() != 0) return 1;
    auto* player = static_cast<Player*>(pages->widget(pages->count() - 1));
    auto* speed = get<QComboBox>(window, "playbackSpeed");
    if (!player || speed->currentData().toDouble() != 1.0) return 1;
    speed->setCurrentIndex(speed->findData(1.5));
    wait_for([&] { return player->playbackSpeedForTest() == 1.5; }, "playback speed");
    get<QLineEdit>(window, "serverAddress")->setText(argv[1]);
    auto* status = get<QLabel>(window, "connectionStatus");
    get<QPushButton>(window, "guestLogin")->click();
    wait_for([&] { return status->text() == "访客会话"; }, "guest login");
    bool promptedToLogin = false;
    QTimer::singleShot(100, [&] {
        for (auto* widget : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(widget)) {
            promptedToLogin = box->text().contains("请先使用邮箱或密码登录");
            if (auto* button = box->button(QMessageBox::Ok)) button->click();
        }
    });
    get<QPushButton>(window, "uploadVideo")->click();
    if (!promptedToLogin) { std::cerr << "guest was not prompted to log in\n"; return 1; }
    get<QLineEdit>(window, "email")->setText("student@example.test");
    get<QPushButton>(window, "sendCode")->click();
    wait_for([&] { return status->text() == "验证码已发送"; }, "send code");
    get<QLineEdit>(window, "verificationCode")->setText("123456");
    get<QPushButton>(window, "emailLogin")->click();
    wait_for([&] { return status->text() == "已登录"; }, "email login");
    get<QPushButton>(window, "refreshProfile")->click();
    wait_for([&] { return get<QLabel>(window, "profileEmail")->text() == "student@example.test"; }, "profile");
    get<QLineEdit>(window, "nickname")->setText("student");
    get<QPushButton>(window, "setNickname")->click();
    wait_for([&] { return status->text() == "昵称已保存"; }, "nickname");
    get<QLineEdit>(window, "newPassword")->setText("practicePassword9!");
    get<QPushButton>(window, "setPassword")->click();
    wait_for([&] { return status->text() == "密码已设置"; }, "set password");
    get<QLineEdit>(window, "videoPath")->setText(argv[2]);
    const auto coverPath = QString::fromUtf8(argv[2]) + ".png";
    QImage image(8, 8, QImage::Format_RGB32); image.fill(Qt::blue);
    if (!image.save(coverPath, "PNG")) return 1;
    get<QLineEdit>(window, "avatarPath")->setText(coverPath);
    get<QPushButton>(window, "saveAvatar")->click();
    wait_for([&] { return status->text() == "头像已保存"; }, "save avatar");
    wait_for([&] { return !get<QLabel>(window, "avatarPreview")->pixmap().isNull(); }, "avatar preview");
    get<QLineEdit>(window, "coverPath")->setText(coverPath);
    get<QLineEdit>(window, "videoTitle")->setText("Test title");
    get<QTextEdit>(window, "videoDescription")->setPlainText("Test description");
    get<QSpinBox>(window, "videoType")->setValue(4);
    get<QSpinBox>(window, "videoDuration")->setValue(12);
    get<QLineEdit>(window, "videoTags")->setText("7,8");
    get<QPushButton>(window, "uploadVideo")->click();
    auto* uploaded = get<QLineEdit>(window, "uploadedVideoId");
    if (get<QPushButton>(window, "uploadVideo")->isEnabled()) { std::cerr << "upload button should be disabled while submitting\n"; return 1; }
    wait_for([&] { return uploaded->text() == "test-video-id"; }, "upload and submit");
    if (!get<QPushButton>(window, "uploadVideo")->isEnabled()) { std::cerr << "upload button should be enabled after submitting\n"; return 1; }
    get<QPushButton>(window, "refreshMyVideos")->click();
    auto* my_table = get<QTableWidget>(window, "myVideos");
    wait_for([&] { return my_table->rowCount() == 1; }, "my videos");
    if (!my_table->item(0, 1) || my_table->item(0, 1)->text() != "待审核") return 1;
    my_table->selectRow(0);
    get<QPushButton>(window, "haltVideo")->click();
    wait_for([&] { return status->text() == "视频已下架"; }, "halt video");
    get<QPushButton>(window, "resubmitVideo")->click();
    wait_for([&] { return status->text() == "已重新提交审核"; }, "resubmit video");
    QTimer::singleShot(100, [] {
        for (auto* widget : QApplication::topLevelWidgets()) if (auto* box = qobject_cast<QMessageBox*>(widget)) {
            if (auto* button = box->button(QMessageBox::Yes)) button->click();
        }
    });
    get<QPushButton>(window, "removeVideo")->click();
    wait_for([&] { return status->text() == "视频已删除"; }, "remove video");
    get<QLineEdit>(window, "videoSearch")->setText("Slow");
    get<QPushButton>(window, "refreshPublic")->click();
    QTest::qWait(100);
    get<QLineEdit>(window, "videoSearch")->setText("Fast");
    get<QPushButton>(window, "refreshPublic")->click();
    auto* public_table = get<QTableWidget>(window, "publicVideos");
    wait_for([&] { return public_table->rowCount() == 1 && public_table->item(0, 0) && public_table->item(0, 0)->text() == "Fast video"; }, "newest search");
    QTest::qWait(1000);
    if (!public_table->item(0, 0) || public_table->item(0, 0)->text() != "Fast video") { std::cerr << "old search replaced the newest result\n"; return 1; }
    get<QLineEdit>(window, "videoSearch")->setText("Public");
    get<QPushButton>(window, "refreshPublic")->click();
    wait_for([&] { return public_table->rowCount() == 1 && public_table->item(0, 0) && public_table->item(0, 0)->text() == "Public video"; }, "public list");
    if (!public_table->item(0, 0) || public_table->item(0, 0)->text() != "Public video") return 1;
    auto* cards = get<QListWidget>(window, "videoCards");
    if (cards->count() != 1 || !cards->item(0)->text().startsWith("Public video")) return 1;
    cards->setCurrentRow(0);
    if (public_table->currentRow() != 0) return 1;
    get<QPushButton>(window, "followAuthor")->click();
    wait_for([&] { return status->text() == "已关注作者"; }, "follow author");
    get<QPushButton>(window, "unfollowAuthor")->click();
    wait_for([&] { return status->text() == "已取消关注"; }, "unfollow author");
    get<QPushButton>(window, "toggleLike")->click();
    wait_for([&] { return status->text() == "已点赞，点赞 1"; }, "like video");
    get<QLineEdit>(window, "barrageText")->setText("hello");
    get<QSpinBox>(window, "barrageSeconds")->setValue(3);
    get<QPushButton>(window, "sendBarrage")->click();
    wait_for([&] { return status->text() == "弹幕已发送"; }, "send barrage");
    get<QPushButton>(window, "loadBarrage")->click();
    wait_for([&] { return get<QTextEdit>(window, "barrageList")->toPlainText().contains("3 秒：hello"); }, "load barrage");
    get<QPushButton>(window, "refreshReview")->click();
    auto* review_table = get<QTableWidget>(window, "reviewVideos");
    wait_for([&] { return review_table->rowCount() == 1; }, "review list");
    get<QLineEdit>(window, "reviewVideoId")->setText("test-video-id");
    get<QPushButton>(window, "approveVideo")->click();
    wait_for([&] { return status->text() == "审核完成"; }, "review");
    get<QLineEdit>(window, "adminEmail")->setText("admin@example.test");
    get<QLineEdit>(window, "adminNickname")->setText("reviewer");
    get<QLineEdit>(window, "adminMemo")->setText("note");
    get<QPushButton>(window, "addAdmin")->click();
    wait_for([&] { return status->text() == "管理员设置已保存"; }, "add administrator");
    get<QPushButton>(window, "refreshAdmins")->click();
    auto* admins = get<QTableWidget>(window, "adminTable");
    wait_for([&] { return admins->rowCount() == 1; }, "administrator list");
    get<QLineEdit>(window, "adminId")->setText("admin-id");
    get<QPushButton>(window, "removeAdmin")->click();
    wait_for([&] { return status->text() == "管理权限已移除"; }, "remove administrator");
    get<QPushButton>(window, "logout")->click();
    wait_for([&] { return status->text() == "未登录"; }, "logout");
    get<QLineEdit>(window, "username")->setText("student");
    get<QLineEdit>(window, "loginPassword")->setText("practicePassword9!");
    get<QPushButton>(window, "passwordLogin")->click();
    wait_for([&] { return status->text() == "已登录"; }, "password login");
    std::cout << "Qt UI network workflow passed\n";
}
