#include "window.h"
#include "workers.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QJsonDocument>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>

int main(int argc, char **argv) {
    bool child = argc > 2 && QByteArray(argv[1]) == "--worker";
    if (child) {
        QCoreApplication app(argc, argv);
        QCoreApplication::setApplicationName("AudioLibrary");
        return runWorker(QString::fromUtf8(argv[2]), app);
    }
    QApplication app(argc, argv);
    app.setApplicationName("AudioLibrary");
    app.setApplicationDisplayName(QStringLiteral("源音效管理工具"));
    app.setApplicationVersion(QString::fromLatin1(AUDIO_LIBRARY_VERSION));
    app.setOrganizationName("AudioLibrary");
    QIcon appIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        appIcon.addFile(QStringLiteral(":/brand/icon_%1.png").arg(size), QSize(size, size));
    app.setWindowIcon(appIcon);
    const QString originalRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QString dataRoot = QDir(originalRoot).filePath("row-wave-v0.4");
    const auto args = app.arguments();
    const auto at = args.indexOf("--data-dir");
    if (at >= 0 && at + 1 < args.size())
        dataRoot = args[at + 1];
    QDir().mkpath(dataRoot);
    QLockFile profileLock(QDir(dataRoot).filePath("application.lock"));
    profileLock.setStaleLockTime(0);
    if (!profileLock.tryLock()) {
        QMessageBox::information(nullptr, QStringLiteral("源音效管理工具"),
                                 QStringLiteral("这个版本已经打开，请使用已有窗口。"));
        return 0;
    }
    const bool custom = at >= 0 && at + 1 < args.size();
    Window window(dataRoot, custom ? QString() : QDir(originalRoot).filePath("library.sqlite"),
                  custom ? QString() : QDir(originalRoot).filePath("peaks"));
    window.setWindowIcon(appIcon);
    window.show();
    return app.exec();
}
