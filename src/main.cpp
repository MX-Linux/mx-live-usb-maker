/**********************************************************************
 *  main.cpp
 **********************************************************************
 * Copyright (C) 2017 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *
 * This is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this package. If not, see <http://www.gnu.org/licenses/>.
 **********************************************************************/

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QStandardPaths>
#include <QTranslator>

#include <memory>

#include <fcntl.h>
#include <unistd.h>

#include "common.h"
#include "mainwindow.h"

// Function-local static to avoid initialization order issues
static QFile &logFile()
{
    static QFile file;
    return file;
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg);

int main(int argc, char *argv[])
{
    const QByteArray option = argc == 2 ? QByteArray(argv[1]) : QByteArray();
    const bool informational = option == "-h" || option == "--help" || option == "--help-all"
                               || option == "-v" || option == "--version";
    std::unique_ptr<QCoreApplication> app;
    if (informational) {
        app = std::make_unique<QCoreApplication>(argc, argv);
    } else {
        if (getuid() == 0) {
            qputenv("XDG_RUNTIME_DIR", "/run/user/0");
            qunsetenv("SESSION_MANAGER");
        }
        // Set Qt platform to XCB (X11) if not already set and we're in X11 environment
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && !qEnvironmentVariableIsEmpty("DISPLAY")
            && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
            qputenv("QT_QPA_PLATFORM", "xcb");
        }

        app = std::make_unique<QApplication>(argc, argv);
        if (getuid() == 0) {
            qputenv("HOME", SystemPaths::ROOT_HOME.toUtf8());
        }
    }

    QApplication::setApplicationVersion(VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QObject::tr("Program for creating a live-usb from an iso-file, another live-usb, "
                    "a live-cd/dvd, or a running live system."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QObject::tr("filename"), QObject::tr("Name of .iso file to open"),
                                 QObject::tr("[filename]"));
    parser.parse(app->arguments());
    if (parser.isSet("help")) {
        QTextStream out(stdout);
        out << parser.helpText()
            << "\nKernel Boot Options (Live-usb-storage):\n"
            << "  nostore        Persistently disable storage (creates a flag on the device).\n"
            << "                 Storage stays disabled until dostore is used.\n"
            << "  dostore        Re-enable storage by removing the persistent nostore flag.\n"
            << "  disablestore   Disable storage for the current boot session only.\n";
        return 0;
    }
    if (parser.isSet("help-all")) {
        parser.process(*app);
    }
    if (parser.isSet("version")) {
        QTextStream(stdout) << QApplication::applicationName() << ' ' << QApplication::applicationVersion() << '\n';
        return 0;
    }

    QApplication::setDesktopFileName("mx-live-usb-maker");

    QApplication::setWindowIcon(QIcon::fromTheme(QApplication::applicationName()));

    QTranslator qtTranslator;
    if (qtTranslator.load(QLocale::system(), "qt", "_", QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtTranslator);
    }

    QTranslator qtBaseTranslator;
    if (qtBaseTranslator.load("qtbase_" + QLocale::system().name(), QLibraryInfo::path(QLibraryInfo::TranslationsPath))) {
        QApplication::installTranslator(&qtBaseTranslator);
    }

    QTranslator appTranslator;
    if (appTranslator.load(QApplication::applicationName() + "_" + QLocale::system().name(),
                           "/usr/share/" + QApplication::applicationName() + "/locale")) {
        QApplication::installTranslator(&appTranslator);
    }

    // Root guard
    QFile loginUidFile{SystemPaths::PROC_SELF_LOGINUID};
    if (loginUidFile.open(QIODevice::ReadOnly)) {
        const QString loginUid = QString(loginUidFile.readAll()).trimmed();
        loginUidFile.close();
        if (loginUid == "0") {
            QMessageBox::critical(
                nullptr, QObject::tr("Error"),
                QObject::tr(
                    "You seem to be logged in as root, please log out and log in as normal user to use this program."));
            exit(EXIT_FAILURE);
        }
    }

    const auto executablePath = QStandardPaths::findExecutable("pkexec");
    if (executablePath.isEmpty() && getuid() != 0) {
        QMessageBox::critical(nullptr, QObject::tr("Error"), QObject::tr("You must run this program as root."));
        exit(EXIT_FAILURE);
    }

    const auto logFileName = SystemPaths::sessionLogPath();
    logFile().setFileName(logFileName);
    if (logFile().exists() && QFileInfo(logFile()).isWritable()) {
        QFile::remove(logFileName + ".old");
        QFile::rename(logFileName, logFileName + ".old");
    }
    if (logFile().exists() && !QFileInfo(logFile()).isWritable()) {
        logFile().setFileName(logFileName + "_new");
    }
    // Open without following a symlink at the final component, so the
    // world-writable /tmp fallback cannot be redirected at another file.
    const int logFd = ::open(logFile().fileName().toLocal8Bit().constData(),
                             O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (logFd < 0 || !logFile().open(logFd, QIODevice::ReadWrite, QFileDevice::AutoCloseHandle)) {
        if (logFd >= 0) {
            ::close(logFd);
        }
    }
    qInstallMessageHandler(messageHandler);
    qDebug().noquote() << QApplication::applicationName() << QObject::tr("version:")
                       << QApplication::applicationVersion();
    MainWindow w(QApplication::arguments());
    w.show();
    return QApplication::exec();
}

void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    QTextStream termOut(stdout);
    termOut << msg << '\n';

    QTextStream out(&logFile());
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz ");
    switch (type) {
    case QtInfoMsg:
        out << "INF ";
        break;
    case QtDebugMsg:
        out << "DBG ";
        break;
    case QtWarningMsg:
        out << "WRN ";
        break;
    case QtCriticalMsg:
        out << "CRT ";
        break;
    case QtFatalMsg:
        out << "FTL ";
        break;
    }
    out << context.category << ": " << msg << '\n';
}
