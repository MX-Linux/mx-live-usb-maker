/**********************************************************************
 *  helper.cpp
 **********************************************************************
 * Copyright (C) 2026-2026 MX Authors
 *
 * Authors: Adrian
 *          MX Linux <http://mxlinux.org>
 *          OpenAI Codex
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

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QHash>
#include <QProcess>

#include <cstdio>
#include <unistd.h>

namespace
{
struct ProcessResult
{
    bool started {};
    int exitCode {1};
    QProcess::ExitStatus exitStatus {QProcess::NormalExit};
};

void writeAndFlush(FILE *stream, const QByteArray &data)
{
    if (data.isEmpty()) {
        return;
    }

    std::fwrite(data.constData(), 1, static_cast<size_t>(data.size()), stream);
    std::fflush(stream);
}

void printError(const QString &message)
{
    writeAndFlush(stderr, message.toUtf8() + '\n');
}

[[nodiscard]] const QHash<QString, QStringList> &allowedCommands()
{
    static const QHash<QString, QStringList> commands {
        {"dd", {"/usr/bin/dd", "/bin/dd"}},
        {"kill", {"/usr/bin/kill", "/bin/kill"}},
#ifdef APPIMAGE_BUILD
        {"live-usb-maker", {"/bin/sh"}},
#else
        {"live-usb-maker", {"/usr/local/bin/live-usb-maker", "/usr/bin/live-usb-maker"}},
#endif
        {"umount", {"/usr/bin/umount", "/bin/umount"}},
    };
    return commands;
}

[[nodiscard]] QString resolveBinary(const QStringList &candidates)
{
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists() && info.isExecutable()) {
            return candidate;
        }
    }
    return {};
}

[[nodiscard]] ProcessResult runProcess(const QString &program, const QStringList &args, const QString &inputFile)
{
    ProcessResult result;
    QProcess process;
    QEventLoop loop;

    QObject::connect(&process, &QProcess::readyReadStandardOutput, [&process] {
        writeAndFlush(stdout, process.readAllStandardOutput());
    });
    QObject::connect(&process, &QProcess::readyReadStandardError, [&process] {
        writeAndFlush(stderr, process.readAllStandardError());
    });
    QObject::connect(&process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), &loop,
                     [&loop, &result](int exitCode, QProcess::ExitStatus exitStatus) {
                         result.exitCode = exitCode;
                         result.exitStatus = exitStatus;
                         loop.quit();
                     });

    if (!inputFile.isEmpty()) {
        process.setStandardInputFile(inputFile);
    }
    process.start(program, args, QIODevice::ReadOnly);
    if (!process.waitForStarted()) {
        printError(QString("Failed to start %1").arg(program));
        result.exitCode = 127;
        return result;
    }

    result.started = true;
    loop.exec();

    writeAndFlush(stdout, process.readAllStandardOutput());
    writeAndFlush(stderr, process.readAllStandardError());
    return result;
}

[[nodiscard]] int handleExec(const QStringList &args)
{
    if (args.isEmpty()) {
        printError(QStringLiteral("exec requires a command name"));
        return 1;
    }

    const QString command = args.constFirst();
    const auto commandIt = allowedCommands().constFind(command);
    if (commandIt == allowedCommands().constEnd()) {
        printError(QString("Command is not allowed: %1").arg(command));
        return 127;
    }

    QString resolvedBinary = resolveBinary(commandIt.value());
    if (resolvedBinary.isEmpty()) {
        printError(QString("Command is not available: %1").arg(command));
        return 127;
    }

    QStringList commandArgs = args.mid(1);
    QString inputFile;
#ifdef APPIMAGE_BUILD
    bool elevate = getuid() != 0;
    if (command == QLatin1String("live-usb-maker")) {
        // Open the payload before pkexec. Root cannot normally traverse the user's FUSE mount.
        inputFile = QCoreApplication::applicationDirPath() + "/backend.tar.gz";
        if (commandArgs == QStringList {QStringLiteral("--help")}) {
            elevate = false;
        }
        commandArgs.prepend(command); // sh -c uses this as $0.
        commandArgs.prepend(QStringLiteral(R"sh(set -eu
base=/tmp
if [ "$(id -u)" = 0 ]; then base=/var/cache; fi
work=$(mktemp -d "$base/mx-live-usb-maker.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
trap 'exit 1' HUP INT TERM
cat > "$work/backend.tar.gz"
(cd "$work" && echo '%1  backend.tar.gz' | sha256sum --check --quiet)
tar -xzf "$work/backend.tar.gz" --no-same-owner -C "$work"
/bin/bash "$work/bin/live-usb-maker" "$@"
)sh").arg(QStringLiteral(APPIMAGE_BACKEND_SHA256)));
        commandArgs.prepend(QStringLiteral("-c"));
    }
    if (elevate) {
        commandArgs.prepend(resolvedBinary);
        resolvedBinary = QStringLiteral("/usr/bin/pkexec");
    }
#endif
    const ProcessResult result = runProcess(resolvedBinary, commandArgs, inputFile);
    if (!result.started) {
        return result.exitCode;
    }
    if (result.exitStatus != QProcess::NormalExit) {
        return 1;
    }
    return result.exitCode;
}
} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments().mid(1);
    if (arguments.isEmpty()) {
        printError(QStringLiteral("Missing helper action"));
        return 1;
    }

    const QString action = arguments.constFirst();
    const QStringList remainingArgs = arguments.mid(1);

    if (action == QLatin1String("exec")) {
        return handleExec(remainingArgs);
    }

    printError(QString("Unsupported helper action: %1").arg(action));
    return 1;
}
