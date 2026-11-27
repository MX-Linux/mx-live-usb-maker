#include "cmd.h"

#include <QApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include <unistd.h>

Cmd::Cmd(QObject *parent)
    : QProcess(parent),
      elevationCommand {elevationTool()},
      helperPath {QStringLiteral(HELPER_PATH)}
{
}

QString Cmd::elevationTool()
{
    if (QFile::exists("/usr/bin/pkexec")) {
        return QStringLiteral("/usr/bin/pkexec");
    }
    if (QFile::exists("/usr/bin/gksu")) {
        return QStringLiteral("/usr/bin/gksu");
    }
    if (QFile::exists("/usr/bin/sudo")) {
        return QStringLiteral("/usr/bin/sudo");
    }
    return {};
}

QString Cmd::getOut(const QString &cmd, QuietMode quiet)
{
    run(cmd, quiet);
    return QString::fromUtf8(readAllStandardOutput());
}

QString Cmd::getOutAsRoot(const QString &programPath, const QStringList &args, QuietMode quiet)
{
    QString output;
    procAsRoot(programPath, args, &output, nullptr, quiet);
    return output;
}

bool Cmd::proc(const QString &programPath, const QStringList &args, QString *output, const QByteArray *input,
               QuietMode quiet)
{
    if (state() != QProcess::NotRunning) {
        qDebug() << "Process already running:" << this->program() << arguments();
        return false;
    }
    if (quiet == QuietMode::No) {
        qDebug() << programPath << args;
    }

    QEventLoop loop;
    connect(this, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop, &QEventLoop::quit);

    start(programPath, args);
    if (input && !input->isEmpty()) {
        write(*input);
    }
    closeWriteChannel();
    loop.exec();

    if (output) {
        *output = QString::fromUtf8(readAllStandardOutput()) + QString::fromUtf8(readAllStandardError());
    }

    emit done();
    return (exitStatus() == QProcess::NormalExit && exitCode() == 0);
}

bool Cmd::helperProc(const QString &programPath, const QStringList &args, QString *output, const QByteArray *input,
                     QuietMode quiet)
{
    QStringList helperArgs {"exec", QFileInfo(programPath).fileName()};
    helperArgs += args;

#ifdef APPIMAGE_BUILD
    // The bundled helper opens AppImage resources before requesting elevation.
    return proc(QCoreApplication::applicationDirPath() + "/../libexec/mx-live-usb-maker/helper",
                helperArgs, output, input, quiet);
#else
    if (getuid() == 0) {
        return proc(helperPath, helperArgs, output, input, quiet);
    }

    if (elevationCommand.isEmpty()) {
        qWarning() << "No elevation helper available";
        return false;
    }

    QStringList elevatedArgs {helperPath};
    elevatedArgs += helperArgs;
    return proc(elevationCommand, elevatedArgs, output, input, quiet);
#endif
}

bool Cmd::procAsRoot(const QString &programPath, const QStringList &args, QString *output, const QByteArray *input,
                     QuietMode quiet)
{
    return helperProc(programPath, args, output, input, quiet);
}

bool Cmd::run(const QString &cmd, QuietMode quiet)
{
    return proc(QStringLiteral("/bin/bash"), {"-c", cmd}, nullptr, nullptr, quiet);
}
