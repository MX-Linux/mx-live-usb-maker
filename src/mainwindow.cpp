/**********************************************************************
 *  mainwindow.cpp
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

#include "mainwindow.h"
#include "common.h"
#include "ui_mainwindow.h"

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStorageInfo>
#include <QTextStream>
#include <sys/stat.h>

#include "about.h"
#include <chrono>

using namespace std::chrono_literals;

MainWindow::MainWindow(const QStringList &args, QDialog *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow),
      advancedOptions(false)
{
    ui->setupUi(this);
    setGeneralConnections();

    // Setup options
    const QString defaultLum = "live-usb-maker";
    QSettings settings("/etc/mx-live-usb-maker/mx-live-usb-maker.conf", QSettings::NativeFormat);
    QString lumName = settings.value("LUM", defaultLum).toString();

    // Try /usr/local/bin first, then fall back to /usr/bin
    LUM = "/usr/local/bin/" + lumName;
    if (!QFile::exists(LUM)) {
        LUM = "/usr/bin/" + lumName;
    }
    qDebug() << "LUM is:" << LUM;

    sizeCheck = settings.value("SizeCheck", 128).toUInt(); // in GB


    setWindowFlags(Qt::Window); // for the close, min, and max buttons
    setup();
    ui->comboUsb->addItems(buildUsbList());

    if (args.size() > 1 && args.at(1) != "%f") {
        QString fileName = QFileInfo(args.at(1)).absoluteFilePath();
        if (QFileInfo(fileName).isFile()) {
            setSourceFile(fileName);
        }
    }
    adjustSize();
}

MainWindow::~MainWindow()
{
    delete ui;
}

bool MainWindow::checkDestSize()
{
    // Get target device size information
    bool ok = false;
    const quint64 diskSizeBytes = cmd.getOut(QString("lsblk --output SIZE -n --bytes /dev/%1 | head -1").arg(device), Cmd::QuietMode::Yes).toULongLong(&ok);
    if (!ok) {
        qDebug() << "Failed to parse disk size for device:" << device;
        return false; // Cannot proceed without valid disk size
    }
    const quint64 diskSizeGB = diskSizeBytes / BYTES_PER_GB; // Convert to GB
    qDebug() << "Target device:" << device << "Raw size (bytes):" << diskSizeBytes << "Size (GB):" << diskSizeGB;

    // Calculate source size
    const quint64 sourceSizeBytes = calculateSourceSize();

    // Validate size compatibility
    if (!validateSizeCompatibility(sourceSizeBytes, diskSizeBytes)) {
        return false;
    }

    // Check for large device warning
    return confirmLargeDeviceWarning(diskSizeGB);
}

bool MainWindow::isRunningLive()
{
    static const QSet<QString> liveFsTypes = {"aufs", "overlay"};

    // First, try to detect using QStorageInfo
    QStorageInfo storageInfo("/");
    QString fsType = QString::fromUtf8(storageInfo.fileSystemType());
    if (liveFsTypes.contains(fsType)) {
        return true;
    }

    // Fallback: parse /proc/mounts for the root filesystem
    QFile mountsFile("/proc/mounts");
    if (mountsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&mountsFile);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            // /proc/mounts format: device mountpoint fstype ...
            // We want the line where mountpoint is /
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() > 2 && parts.at(1) == "/") {
                if (liveFsTypes.contains(parts.at(2))) {
                    mountsFile.close();
                    return true;
                }
                break; // found root, no need to continue
            }
        }
        mountsFile.close();
    }

    // If both methods fail, return false
    return false;
}

bool MainWindow::isToRam()
{
    return QFileInfo::exists("/live/config/did-toram");
}

void MainWindow::makeUsb(const QStringList &options)
{
    device = ui->comboUsb->currentText().split(' ').first();
    QString source = ui->pushSelectSource->property("filename").toString();
    QString sourceSize;

    if (!ui->checkCloneLive->isChecked() && !ui->checkCloneMode->isChecked()) {
        sourceSize = cmd.getOut(QString("du -m \"%1\" 2>/dev/null | cut -f1").arg(source), Cmd::QuietMode::Yes);
    } else if (ui->checkCloneMode->isChecked()) {
        sourceSize = cmd.getOut(QString("du -m --summarize \"%1\" 2>/dev/null | cut -f1").arg(source), Cmd::QuietMode::Yes);
        QString rootPartition = cmd.getOut(QString("df --output=source \"%1\" | awk 'END{print $1}'").arg(source));
        source = "clone=" + source;
        if ("/dev/" + device == getDrivePath(rootPartition)) {
            showErrorAndReset(tr("Source and destination are on the same device, please select again."));
            return;
        }
    } else if (ui->checkCloneLive->isChecked()) {
        source = "clone";
        QString path = isToRam() ? "/live/to-ram" : "/live/boot-dev";
        sourceSize = cmd.getOut(QString("du -m --summarize %1 2>/dev/null | cut -f1").arg(path), Cmd::QuietMode::Yes);
    }

    if (!checkDestSize()) {
        showErrorAndReset();
        return;
    }

    // Check amount of IO on device before copy, this is in sectors
    bool ok = false;
    const quint64 start_io = cmd.getOut(QString("awk '{print $7}' /sys/block/%1/stat").arg(device), Cmd::QuietMode::Yes).toULongLong(&ok);
    if (!ok) {
        qDebug() << "Failed to parse initial IO stats for device:" << device;
        ui->progBar->setMinimum(0); // Use default values
    } else {
        ui->progBar->setMinimum(static_cast<int>(start_io));
    }

    const quint64 sourceSizeMB = sourceSize.toULongLong(&ok);
    if (!ok) {
        qDebug() << "Failed to parse source size:" << sourceSize;
        ui->progBar->setMaximum(100); // Use default progress range
    } else {
        const quint64 isoSectors = sourceSizeMB * SECTORS_PER_MB;
        ui->progBar->setMaximum(static_cast<int>(isoSectors + start_io));
    }

    QStringList liveUsbArgs {"gui"};
    liveUsbArgs += options;
    liveUsbArgs << "-C" << "off" << "--from=" + source << "-t" << "/dev/" + device;
    if (ui->radioDd->isChecked()) {
        const QStringList partitionClearArgs {"gui", "partition-clear", "-NC", "off", "--target", device};
        connect(&cmd, &Cmd::readyReadStandardOutput, this, &MainWindow::updateOutput);
        qDebug() << cmd.getOutAsRoot(LUM, partitionClearArgs, Cmd::QuietMode::Yes);
        ui->outputBox->insertPlainText(tr("Writing %1 using 'dd' command to /dev/%2,\n\n"
                                          "Please wait until the process is completed")
                                           .arg(source, device));
        setConnections();
        cmd.procAsRoot(QStringLiteral("dd"), {"bs=1M", "if=" + source, "of=/dev/" + device}, nullptr, nullptr,
                       Cmd::QuietMode::Yes);
        return;
    }
    setConnections();
    cmd.procAsRoot(LUM, liveUsbArgs, nullptr, nullptr, Cmd::QuietMode::Yes);
}

void MainWindow::setup()
{
    connect(QApplication::instance(), &QApplication::aboutToQuit, this, &MainWindow::cleanup);
    setWindowTitle(tr("MX Live Usb Maker"));

    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    ui->outputBox->setFont(font);

    defaultHeight = geometry().height();
    ui->groupAdvOptions->setVisible(false);
    ui->pushBack->setVisible(false);
    ui->stackedWidget->setCurrentIndex(0);
    ui->pushCancel->setEnabled(true);
    ui->pushNext->setEnabled(true);
    ui->outputBox->setCursorWidth(0);
    adjustSize();
    height = geometry().height();

    QRegularExpression rx("\\w*");
    QValidator *validator = new QRegularExpressionValidator(rx, this);
    ui->textLabel->setValidator(validator);

    // Set save boot directory option to disable unless update mode is checked
    ui->checkSaveBoot->setEnabled(false);

    // Enable clone live option only if running live and not encrypted
    ui->checkCloneLive->setEnabled(isRunningLive() && !QFile::exists("/live/config/encrypted"));

    // Dynamically show or hide data format options based on availability
    const QString lumHelp = cmd.getOut(LUM + " --help 2>&1", Cmd::QuietMode::Yes);
    const bool dataFirstAvailable = lumHelp.contains("--data-first");
    ui->checkDataFirst->setVisible(dataFirstAvailable);
    ui->comboBoxDataFormat->setVisible(dataFirstAvailable);
    ui->labelFormat->setVisible(dataFirstAvailable);
    ui->spinBoxDataSize->setVisible(dataFirstAvailable);

    // Disable by default and enable only when checking the box
    ui->comboBoxDataFormat->setEnabled(false);
    ui->labelFormat->setEnabled(false);
    ui->spinBoxDataSize->setEnabled(false);
}

void MainWindow::setGeneralConnections()
{
    connect(ui->checkCloneLive, &QCheckBox::clicked, this, &MainWindow::checkCloneLive_clicked);
    connect(ui->checkCloneMode, &QCheckBox::clicked, this, &MainWindow::checkCloneMode_clicked);
    connect(ui->checkDataFirst, &QCheckBox::clicked, this, &MainWindow::checkDataFirst_clicked);
    connect(ui->checkUpdate, &QCheckBox::clicked, this, &MainWindow::checkUpdate_clicked);
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAbout_clicked);
    connect(ui->pushBack, &QPushButton::clicked, this, &MainWindow::pushBack_clicked);
    connect(ui->pushCancel, &QPushButton::clicked, this, &MainWindow::close);
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelp_clicked);
    connect(ui->pushLumLogFile, &QPushButton::clicked, this, &MainWindow::pushLumLogFile_clicked);
    connect(ui->pushNext, &QPushButton::clicked, this, &MainWindow::pushNext_clicked);
    connect(ui->pushOptions, &QPushButton::clicked, this, &MainWindow::pushOptions_clicked);
    connect(ui->pushRefresh, &QPushButton::clicked, this, &MainWindow::pushRefresh_clicked);
    connect(ui->pushSelectSource, &QPushButton::clicked, this, &MainWindow::pushSelectSource_clicked);
    connect(ui->radioDd, &QRadioButton::clicked, this, &MainWindow::radioDd_clicked);
    connect(ui->radioNormal, &QRadioButton::clicked, this, &MainWindow::radioNormal_clicked);
    connect(ui->spinBoxSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::spinBoxSize_valueChanged);
    connect(ui->textLabel, &QLineEdit::textChanged, this, &MainWindow::textLabel_textChanged);
}

// Build the option list to be passed to live-usb-maker
QStringList MainWindow::buildOptionList()
{
    QStringList optionsList {"-N"};
    const bool updateMode = ui->checkUpdate->isChecked();

    // Map the checkboxes to the corresponding options
    // Note: partition/format options are skipped in update mode as a safeguard
    std::map<QCheckBox *, QString> checkboxOptions {
        {ui->checkKeep, "-k"},
        {ui->checkPretend, "-p"},
        {ui->checkSaveBoot, "-S"},
        {ui->checkUpdate, "-u"},
        {ui->checkForceUsb, "--force=usb"},
        {ui->checkForceAutomount, "--force=automount"},
        {ui->checkForceNofuse, "--force=nofuse"},
    };

    // Partition/format options - only add if not in update mode
    if (!updateMode) {
        checkboxOptions[ui->checkEncrypt] = "-E";
        checkboxOptions[ui->checkGpt] = "-g";
        checkboxOptions[ui->checkSetPmbrBoot] = "--gpt-pmbr";
        checkboxOptions[ui->checkForceMakefs] = "--force=makefs";
    }

    // Add options for the checked checkboxes
    for (const auto &[checkBox, option] : checkboxOptions) {
        if (checkBox->isChecked()) {
            optionsList.append(option);
        }
    }

    // Add additional options (skip partition/format options in update mode)
    if (!updateMode) {
        if (ui->spinBoxEsp->value() != 50) {
            optionsList.append("--esp-size=" + ui->spinBoxEsp->cleanText());
        }
        if (ui->spinBoxSize->value() < ui->spinBoxSize->maximum()) {
            optionsList.append("--size=" + ui->spinBoxSize->cleanText());
        }
        if (!ui->textLabel->text().isEmpty()) {
            optionsList.append("--label=" + ui->textLabel->text());
        }
        if (ui->checkDataFirst->isChecked()) {
            optionsList.append("--data-first=" + ui->spinBoxDataSize->cleanText() + ","
                               + ui->comboBoxDataFormat->currentText());
        }
    }

    // Add the verbosity option
    if (ui->sliderVerbosity->value() == 1) {
        optionsList.append("-V");
    } else if (ui->sliderVerbosity->value() == 2) {
        optionsList.append("-VV");
    }

    qDebug() << "Options:" << optionsList;
    return optionsList;
}

void MainWindow::cleanup()
{
    // Check if we actually did any work that needs privileged cleanup
    bool needsPrivilegedCleanup = false;

    // Check 1: Are there any mount points?
    const QString mountPath = "/run/live-usb-maker";
    if (Cmd().run("mountpoint -q " + mountPath, Cmd::QuietMode::Yes) ||
        Cmd().run("mountpoint -q " + mountPath + "/main", Cmd::QuietMode::Yes)) {
        needsPrivilegedCleanup = true;
    }

    // Check 2: Are there running processes?
    if (cmd.state() != QProcess::NotRunning) {
        needsPrivilegedCleanup = true;
    }

    // Only do privileged operations if we actually need them
    if (needsPrivilegedCleanup) {
        if (cmd.state() != QProcess::NotRunning) {
            QTimer::singleShot(10s, this, [this] {
                if (cmd.state() != QProcess::NotRunning) {
                    Cmd().procAsRoot(QStringLiteral("kill"), {"-9", "--", "-" + QString::number(cmd.processId())},
                                     nullptr, nullptr, Cmd::QuietMode::Yes);
                }
            });
            Cmd().procAsRoot(QStringLiteral("kill"), {"--", "-" + QString::number(cmd.processId())}, nullptr, nullptr,
                             Cmd::QuietMode::Yes);
        }

        if (Cmd().run("mountpoint -q " + mountPath, Cmd::QuietMode::Yes)) {
            Cmd().procAsRoot(QStringLiteral("umount"), {"-R", "-l", mountPath}, nullptr, nullptr, Cmd::QuietMode::Yes);
        }
        if (Cmd().run("mountpoint -q " + mountPath + "/main", Cmd::QuietMode::Yes)) {
            Cmd().procAsRoot(QStringLiteral("umount"), {"-l", mountPath + "/main", mountPath + "/uefi"}, nullptr,
                             nullptr, Cmd::QuietMode::Yes);
        }

        const QString pid = QString::number(QApplication::applicationPid());
        if (!Cmd().run("ps --ppid " + pid, Cmd::QuietMode::Yes)) {
            Cmd().procAsRoot(QStringLiteral("kill"), {"--", "-" + pid}, nullptr, nullptr, Cmd::QuietMode::Yes);
        }
    }
}

QStringList MainWindow::buildUsbList()
{
    const QString drives = cmd.getOut("lsblk --nodeps -nlo NAME,SIZE,MODEL,VENDOR -I 3,8,22,179,202,252,253,254,259", Cmd::QuietMode::Yes).trimmed();
    return removeUnsuitable(drives.split('\n'));
}

QString MainWindow::expandDevicePath(const QString &device)
{
    QString dev = device.trimmed();
    QString candidate;

    if (dev.startsWith("/dev/")) {
        candidate = dev;
    } else if (dev.startsWith("dev/")) {
        candidate = "/" + dev;
    } else if (dev.startsWith('/')) {
        candidate = "/dev" + dev;
    } else {
        candidate = "/dev/" + dev;
    }

    struct stat st {};
    const QByteArray pathBytes = candidate.toLocal8Bit();
    if (::stat(pathBytes.constData(), &st) != 0) {
        return {};
    }
    if (!S_ISBLK(st.st_mode)) {
        return {};
    }
    return candidate;
}

QString MainWindow::getDriveName(const QString &device)
{
    QString name = device.trimmed();
    if (name.startsWith("/dev/")) {
        name = name.mid(5);
    } else if (name.startsWith("dev/")) {
        name = name.mid(4);
    } else if (name.startsWith('/')) {
        name = name.mid(1);
    }

    if (name.contains("mmcblk") || name.contains("nvme")) {
        const QRegularExpression pDigitRe("p\\d$");
        if (pDigitRe.match(name).hasMatch()) {
            name.chop(2);
        }
        return name;
    }

    const QRegularExpression digitRe("\\d$");
    if (digitRe.match(name).hasMatch()) {
        name.chop(1);
    }
    if (digitRe.match(name).hasMatch()) {
        name.chop(1);
    }
    return name;
}

QString MainWindow::getDrivePath(const QString &device)
{
    const QString name = getDriveName(device);
    if (name.isEmpty()) {
        return {};
    }
    return "/dev/" + name;
}

bool MainWindow::isUsbOrRemovable(const QString &device)
{
    const QString devicePath = expandDevicePath(device);
    if (devicePath.isEmpty()) {
        return false;
    }

    const QString driveName = getDriveName(devicePath);
    if (driveName.isEmpty()) {
        return false;
    }

    const QString sysBlock = "/sys/block/" + driveName;
    if (!QFileInfo::exists(sysBlock)) {
        return false;
    }

    const QString devPath = QFileInfo(sysBlock + "/device").canonicalFilePath();
    if (devPath.isEmpty()) {
        return false;
    }

    if (devPath.contains("sdmmc")) {
        return true;
    }

    QFile removableFile(sysBlock + "/removable");
    if (removableFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString flag = QString::fromUtf8(removableFile.readAll()).trimmed();
        if (flag == "1") {
            return true;
        }
    }

    if (QFileInfo(devPath).fileName().startsWith("usb")) {
        return true;
    }

    return devPath.contains("/usb");
}

QString MainWindow::readInitrdParam(const QString &name, const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream in(&file);
    const QRegularExpression re(QString("^\\s*%1=(.*)$").arg(QRegularExpression::escape(name)));
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QRegularExpressionMatch match = re.match(line);
        if (match.hasMatch()) {
            QString value = match.captured(1).trimmed();
            if ((value.startsWith('"') && value.endsWith('"')) || (value.startsWith('\'') && value.endsWith('\''))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
    }
    return {};
}

QString MainWindow::getLiveDeviceName()
{
    const QString cryptUuid = readInitrdParam("CRYPT_UUID");
    QString liveDevPath;
    if (!cryptUuid.isEmpty()) {
        QDir uuidDir("/dev/disk/by-uuid");
        if (uuidDir.exists()) {
            const QFileInfoList entries = uuidDir.entryInfoList(QDir::NoDotAndDotDot | QDir::System);
            for (const QFileInfo &entry : entries) {
                if (entry.fileName() == cryptUuid) {
                    liveDevPath = entry.canonicalFilePath();
                    break;
                }
            }
        }
    }

    if (liveDevPath.isEmpty()) {
        QFile mounts("/proc/mounts");
        if (mounts.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString content = QString::fromUtf8(mounts.readAll());
            mounts.close();
            const QStringList bootDevPaths = {"/live/boot-dev", "/run/initramfs/boot-dev"};
            const QStringList lines = content.split('\n', Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                const QStringList parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() > 1 && bootDevPaths.contains(parts.at(1))) {
                    liveDevPath = parts.at(0);
                    break;
                }
            }
        }
    }

    if (liveDevPath.isEmpty()) {
        return {};
    }
    return QFileInfo(liveDevPath).fileName();
}

// Remove unsuitable drives from the list (live and unremovable)
QStringList MainWindow::removeUnsuitable(const QStringList &devices)
{
    QStringList suitableDevices;
    suitableDevices.reserve(devices.size());
    const QString liveDrive = getDriveName(getLiveDeviceName());
    const QString rootDrive
        = cmd.getOut("lsblk -nlso NAME,PKNAME,TYPE $(findmnt / -no SOURCE) | grep 'disk' | awk '{print $1}'", Cmd::QuietMode::Yes)
              .trimmed();
    for (const QString &deviceInfo : devices) {
        const QString deviceName = deviceInfo.split(' ').first();
        const bool deviceIsUsbOrRemovable = ui->checkForceUsb->isChecked() || isUsbOrRemovable(deviceName);
        if (deviceIsUsbOrRemovable && deviceName != liveDrive && deviceName != rootDrive) {
            suitableDevices.append(deviceInfo);
        }
    }
    return suitableDevices;
}

void MainWindow::cmdDone()
{
    timer.stop();
    ui->progBar->setValue(ui->progBar->maximum());
    setCursor(Qt::ArrowCursor);
    ui->pushBack->show();
    if ((cmd.exitCode() == 0 && cmd.exitStatus() == QProcess::NormalExit) || ui->checkPretend->isChecked()) {
        QMessageBox::information(this, tr("Success"), tr("LiveUSB creation successful!"));
    } else {
        cleanup();
        QMessageBox::critical(this, tr("Failure"), tr("Error encountered in the LiveUSB creation process"));
    }
    cmd.disconnect();
}

void MainWindow::setConnections()
{
    timer.start(1s);
    connect(&cmd, &Cmd::readyReadStandardOutput, this, &MainWindow::updateOutput);
    connect(&timer, &QTimer::timeout, this, &MainWindow::updateBar);
    connect(&cmd, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::cmdDone);
}

// Set proper default mode based on iso contents
void MainWindow::setDefaultMode(const QString &isoName)
{
    if (!isantiX_mx_family(isoName)) {
        ui->radioDd->click();
        ui->radioNormal->setChecked(false);
    } else {
        ui->radioDd->setChecked(false);
        ui->radioNormal->click();
    }
}

void MainWindow::updateBar()
{
    QFile statFile(QString("/sys/block/%1/stat").arg(device));
    if (!statFile.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open stat file:" << statFile.errorString();
        return; // Skip this update
    }
    QString out = statFile.readAll();
    if (out.isEmpty()) {
        qDebug() << "Empty stat file content";
        return;
    }
    quint64 current_io = out.section(QRegularExpression("\\s+"), 7, 7).toULongLong();
    ui->progBar->setValue(static_cast<int>(current_io));
    statFile.close();
}

void MainWindow::updateOutput()
{
    QString output = cmd.readAllStandardOutput();
    // Remove escape sequences that are not handled by code
    const QRegularExpression re(u8R"(\[0m|\]0;|\|\|\[1000D|\[74C||\[\?25l|\[\?25h|\[0;36m|\[1;37m)");
    output.remove(re);
    ui->outputBox->moveCursor(QTextCursor::End);
    if (output.contains('\r')) {
        ui->outputBox->moveCursor(QTextCursor::Up, QTextCursor::KeepAnchor);
        ui->outputBox->moveCursor(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    }
    ui->outputBox->insertPlainText(output);

    QScrollBar *sb = ui->outputBox->verticalScrollBar();
    sb->setValue(sb->maximum());
    QApplication::processEvents();
}

void MainWindow::pushNext_clicked()
{
    if (ui->stackedWidget->currentIndex() != 0) {
        return;
    }
    if (ui->comboUsb->currentText().isEmpty()) {
        QMessageBox::critical(this, tr("Error"), tr("Please select a USB device to write to"));
        return;
    }
    const QString targetLabel = ui->comboUsb->currentText().simplified();
    QString msg;
    if (ui->checkUpdate->isChecked() && !ui->radioDd->isChecked()) {
        msg = tr("Update mode is selected. The live system on %1 will be updated with the new ISO without reformatting the "
                 "device.\n\nExisting data and persistence should remain, but please back up anything important.\n\nDo you "
                 "wish to continue?")
                  .arg(targetLabel);
    } else {
        msg = tr("These actions will destroy all data on \n\n") + targetLabel + "\n\n " + tr("Do you wish to continue?");
    }
    if (QMessageBox::Yes != QMessageBox::warning(this, windowTitle(), msg, QMessageBox::Yes, QMessageBox::No)) {
        return;
    }
    QString sourceFilename = ui->pushSelectSource->property("filename").toString();
    if (!QFileInfo::exists(sourceFilename) && sourceFilename != "clone") {
        emit ui->pushSelectSource->clicked();
        return;
    }
    if (cmd.state() != QProcess::NotRunning) {
        ui->stackedWidget->setCurrentWidget(ui->outputPage);
        return;
    }
    ui->pushNext->setEnabled(false);
    ui->stackedWidget->setCurrentWidget(ui->outputPage);
    makeUsb(buildOptionList());
}

void MainWindow::pushBack_clicked()
{
    setWindowTitle("MX Live Usb Maker");
    ui->stackedWidget->setCurrentIndex(0);
    ui->pushNext->setEnabled(true);
    ui->pushBack->hide();
    ui->outputBox->clear();
    ui->progBar->setValue(0);
}

void MainWindow::pushAbout_clicked()
{
    hide();
    displayAboutMsgBox(
        tr("About %1").arg(windowTitle()),
        "<p align=\"center\"><b><h2>" + windowTitle() + "</h2></b></p><p align=\"center\">" + tr("Version: ")
            + QApplication::applicationVersion() + "</p><p align=\"center\"><h3>"
            + tr("Program for creating a live-usb from an iso-file, another live-usb, a live-cd/dvd, or a running live "
                 "system.")
            + R"(</h3></p><p align="center"><a href="http://mxlinux.org">http://mxlinux.org</a><br /></p><p align="center">)"
            + tr("Copyright (c) MX Linux") + "<br /><br /></p>",
        "/usr/share/doc/mx-live-usb-maker/license.html", tr("%1 License").arg(windowTitle()), this);
    show();
}

void MainWindow::pushHelp_clicked()
{
    const QString helpPath = "/usr/share/doc/mx-live-usb-maker/mx-live-usb-maker.html";
    displayHelpDoc(helpPath, tr("%1 Help").arg(windowTitle()));
}

void MainWindow::pushSelectSource_clicked()
{
    const QString user = cmd.getOut("logname", Cmd::QuietMode::Yes);
    const QString home = "/home/" + user;
    if (QString selected; !ui->checkCloneLive->isChecked() && !ui->checkCloneMode->isChecked()) {
        selected = QFileDialog::getOpenFileName(this, tr("Select an ISO file to write to the USB drive"), home,
                                                tr("ISO Files (*.iso);;All Files (*.*)"));
        if (!selected.isEmpty()) {
            setSourceFile(selected);
        }
    } else if (ui->checkCloneMode->isChecked()) {
        selected = QFileDialog::getExistingDirectory(this, tr("Select Source Directory"), QDir::rootPath(),
                                                     QFileDialog::ShowDirsOnly);
        if (QFileInfo::exists(selected + "/antiX/linuxfs") || QFileInfo::exists(selected + "/linuxfs")) {
            ui->pushSelectSource->setText(selected);
            ui->pushSelectSource->setProperty("filename", selected);
        } else {
            if (selected == '/') {
                selected.clear();
            }
            QMessageBox::critical(this, tr("Failure"), tr("Could not find linuxfs file").arg(selected));
        }
    }
}

void MainWindow::pushRefresh_clicked()
{
    ui->comboUsb->clear();
    ui->comboUsb->addItems(buildUsbList());
}

void MainWindow::pushOptions_clicked()
{
    advancedOptions = !advancedOptions;
    ui->groupAdvOptions->setVisible(advancedOptions);
    ui->pushOptions->setText(advancedOptions ? tr("Hide advanced options") : tr("Show advanced options"));
    ui->pushOptions->setIcon(QIcon::fromTheme(advancedOptions ? "go-up" : "go-down"));

    if (advancedOptions) {
        // Expand to show advanced options but keep window resizable
        setMinimumHeight(defaultHeight);
        resize(width(), defaultHeight);
    } else {
        // Collapse to hide advanced options
        setMinimumHeight(height);
        resize(width(), height);
    }
}

void MainWindow::textLabel_textChanged(QString arg1)
{
    ui->textLabel->setText(arg1.remove(' '));
    ui->textLabel->setCursorPosition(arg1.length());
}

void MainWindow::checkUpdate_clicked(bool checked)
{
    ui->checkSaveBoot->setEnabled(checked);
    if (!checked) {
        ui->checkSaveBoot->setChecked(false);
    }

    // Disable options that conflict with update mode (would force reformat)
    const bool enabled = !checked;

    // Size options
    ui->spinBoxSize->setEnabled(enabled);
    ui->label_percent->setEnabled(enabled);

    // Label option
    ui->textLabel->setEnabled(enabled);
    ui->label_part_label->setEnabled(enabled);

    // ESP size option
    ui->spinBoxEsp->setEnabled(enabled);
    ui->labelSizeEsp->setEnabled(enabled);

    // Data partition option
    ui->checkDataFirst->setEnabled(enabled);
    ui->spinBoxDataSize->setEnabled(enabled);
    ui->comboBoxDataFormat->setEnabled(enabled);
    ui->labelFormat->setEnabled(enabled);

    // Partition/format options
    ui->checkEncrypt->setEnabled(enabled);
    ui->checkGpt->setEnabled(enabled);
    ui->checkSetPmbrBoot->setEnabled(enabled);
    ui->checkForceMakefs->setEnabled(enabled);

    // Reset to defaults when update mode is enabled
    if (checked) {
        // Block signals to prevent valueChanged from re-enabling controls
        ui->spinBoxSize->blockSignals(true);
        ui->spinBoxSize->setValue(ui->spinBoxSize->maximum());
        ui->spinBoxSize->blockSignals(false);

        ui->spinBoxEsp->setValue(50);
        ui->textLabel->clear();
        ui->checkDataFirst->setChecked(false);
        ui->spinBoxDataSize->setValue(ui->spinBoxDataSize->minimum());
        ui->checkEncrypt->setChecked(false);
        ui->checkGpt->setChecked(false);
        ui->checkSetPmbrBoot->setChecked(false);
        ui->checkForceMakefs->setChecked(false);
    }
}

void MainWindow::checkCloneMode_clicked(bool checked)
{
    if (checked) {
        ui->pushSelectSource->setStyleSheet("text-align: center;");
        ui->checkCloneLive->setChecked(false);
        checkCloneLive_clicked(false);
        ui->label_3->setText("<b>" + tr("Select Source") + "</b>");
        ui->pushSelectSource->setText(tr("Select Source Directory"));
        ui->pushSelectSource->setIcon(QIcon::fromTheme("folder"));
        ui->radioDd->setDisabled(true);
    } else {
        ui->label_3->setText("<b>" + tr("Select ISO file") + "</b>");
        ui->pushSelectSource->setText(tr("Select ISO"));
        ui->pushSelectSource->setIcon(QIcon::fromTheme("user-home"));
        ui->checkCloneLive->setEnabled(isRunningLive());
        ui->radioDd->setEnabled(true);
    }
    ui->pushSelectSource->setProperty("filename", "");
}

void MainWindow::checkCloneLive_clicked(bool checked)
{
    if (checked) {
        ui->pushSelectSource->setStyleSheet("text-align: center;");
        ui->checkCloneMode->setChecked(false);
        ui->label_3->setText("<b>" + tr("Select Source") + "</b>");
        ui->pushSelectSource->setEnabled(false);
        ui->pushSelectSource->setText(tr("clone"));
        ui->pushSelectSource->setProperty("filename", "clone");
        ui->pushSelectSource->setIcon(QIcon::fromTheme("tools-media-optical-copy"));
        ui->pushSelectSource->blockSignals(true);
        ui->radioDd->setDisabled(true);
    } else {
        ui->label_3->setText("<b>" + tr("Select ISO file") + "</b>");
        ui->pushSelectSource->setEnabled(true);
        ui->pushSelectSource->setText(tr("Select ISO"));
        ui->pushSelectSource->setProperty("filename", "");
        ui->pushSelectSource->setIcon(QIcon::fromTheme("user-home"));
        ui->pushSelectSource->blockSignals(false);
        ui->radioDd->setDisabled(false);
    }
}

void MainWindow::radioDd_clicked()
{
    ui->radioNormal->setChecked(false);
    ui->checkCloneLive->setChecked(false);
    ui->checkCloneMode->setChecked(false);
    ui->checkEncrypt->setChecked(false);
    ui->checkCloneLive->setEnabled(false);
    ui->checkCloneMode->setEnabled(false);
    ui->checkEncrypt->setEnabled(false);
    ui->checkPretend->setEnabled(false);
    if (ui->groupAdvOptions->isVisible()) {
        pushOptions_clicked();
    }
    ui->pushOptions->setEnabled(false);
    ui->label_percent->setEnabled(false);
    ui->label_part_label->setEnabled(false);
    ui->spinBoxSize->setEnabled(false);
    ui->textLabel->setEnabled(false);
}

void MainWindow::radioNormal_clicked()
{
    ui->checkCloneLive->setEnabled(isRunningLive());
    ui->checkCloneMode->setEnabled(true);
    ui->checkPretend->setEnabled(true);
    ui->pushOptions->setEnabled(true);

    // Only re-enable these if update mode is not checked
    const bool updateMode = ui->checkUpdate->isChecked();
    ui->checkEncrypt->setEnabled(!updateMode);
    ui->label_percent->setEnabled(!updateMode);
    ui->label_part_label->setEnabled(!updateMode);
    ui->spinBoxSize->setEnabled(!updateMode);
    ui->textLabel->setEnabled(!updateMode);
}

bool MainWindow::isantiX_mx_family(const QString &selected)
{
    return Cmd().run(
        QStringLiteral("xorriso -indev '%1' -find /antiX -name linuxfs -prune  2>/dev/null | grep -q /antiX/linuxfs")
            .arg(selected),
        Cmd::QuietMode::Yes);
}

void MainWindow::pushLumLogFile_clicked()
{
    QFileInfo lum(LUM);
    QString logFileName = lum.baseName() + ".log";
    QString logFilePath = "/var/log/" + logFileName;
    QString tempLogFilePath = "/tmp/" + logFileName;
    qDebug() << "lumlog" << tempLogFilePath;

    if (!QFileInfo::exists(logFilePath)) {
        QMessageBox::information(this, QApplication::applicationName(),
                                 tr("Could not find a log file at: ") + logFilePath);
        return;
    }

    // Generate temporary log file by reversing the log file until the delimiter, then reversing it back
    QString cmdStr = QString("tac %1 | sed '/^={60}=$/q' | tac > %2").arg(logFilePath, tempLogFilePath);
    Cmd().run(cmdStr);
    displayDoc(tempLogFilePath, lum.baseName());
}

void MainWindow::spinBoxSize_valueChanged(int arg1)
{
    // Don't re-enable controls if update mode is checked
    if (ui->checkUpdate->isChecked()) {
        return;
    }
    const int max = ui->spinBoxSize->maximum();
    ui->checkDataFirst->setEnabled(arg1 == max);
    ui->spinBoxDataSize->setEnabled(arg1 == max);
    ui->comboBoxDataFormat->setEnabled(arg1 == max);
    ui->labelFormat->setEnabled(arg1 == max);
}

void MainWindow::checkDataFirst_clicked(bool checked)
{
    ui->spinBoxSize->setDisabled(checked);
    ui->comboBoxDataFormat->setEnabled(checked);
    ui->labelFormat->setEnabled(checked);
    ui->spinBoxDataSize->setEnabled(checked);
}

void MainWindow::setSourceFile(const QString &fileName)
{
    ui->pushSelectSource->setText(fileName);
    ui->pushSelectSource->setProperty("filename", fileName);
    ui->pushSelectSource->setToolTip(fileName);
    ui->pushSelectSource->setIcon(QIcon::fromTheme("media-cdrom"));
    ui->pushSelectSource->setStyleSheet("text-align: left;");
    setDefaultMode(fileName); // Set proper default mode based on iso contents
}

void MainWindow::showErrorAndReset(const QString &message)
{
    if (!message.isEmpty()) {
        QMessageBox::critical(this, tr("Failure"), message);
    }
    ui->stackedWidget->setCurrentWidget(ui->selectionPage);
    ui->pushNext->setEnabled(true);
    setCursor(Qt::ArrowCursor);
}

quint64 MainWindow::calculateSourceSize()
{
    const QString sourceFilename = ui->pushSelectSource->property("filename").toString();
    qDebug() << "Source filename property:" << sourceFilename;

    // Handle source size calculation based on operation mode
    if (ui->checkCloneMode->isChecked() || ui->checkCloneLive->isChecked()) {
        QString linuxfsPath;

        // Determine the linuxfs path based on clone mode
        if (ui->checkCloneMode->isChecked()) {
            // Clone mode: check for linuxfs file in standard locations
            linuxfsPath = sourceFilename + "/antiX/linuxfs";
            if (!QFileInfo::exists(linuxfsPath)) {
                linuxfsPath = sourceFilename + "/linuxfs";
            }
        } else {
            // Clone live system mode: use current live system
            linuxfsPath = "/live/linux";
        }

        // Calculate source size if linuxfs path exists
        if (QFileInfo::exists(linuxfsPath)) {
            const QFileInfo info(linuxfsPath);
            if (info.isDir()) {
                // For directories, get used space via df command
                const QString cmdStr = QString("df --output=used -B1 \"%1\" | tail -1").arg(linuxfsPath);
                const QString usedStr = cmd.getOut(cmdStr, Cmd::QuietMode::Yes).trimmed();
                bool ok = false;
                const quint64 sourceSizeBytes = usedStr.toULongLong(&ok);
                return ok ? sourceSizeBytes : 0;
            } else {
                // For files, get size directly
                return info.size();
            }
        } else {
            // Show warning only in clone mode (not for live cloning)
            if (ui->checkCloneMode->isChecked()) {
                QMessageBox::warning(this, tr("Source Error"),
                    tr("Could not find the source linuxfs file."));
            }
            return 0;
        }
    } else {
        // Standard ISO file mode
        if (!sourceFilename.isEmpty() && QFileInfo::exists(sourceFilename)) {
            const quint64 sourceSizeBytes = QFileInfo(sourceFilename).size();
            qDebug() << "ISO file size (bytes):" << sourceSizeBytes;
            return sourceSizeBytes;
        } else {
            qDebug() << "ISO file does not exist or not specified:" << sourceFilename;
            return 0;
        }
    }
}

bool MainWindow::validateSizeCompatibility(const quint64 sourceSize, const quint64 diskSize) const
{
    // Check if source is larger than destination
    if (sourceSize > 0 && diskSize < sourceSize) {
        const QString msg = tr("Warning: The target device (%1) is smaller than the source (%2). The data might not fit. Do you want to continue?")
                        .arg(device)
                        .arg(ui->pushSelectSource->text());
        return QMessageBox::Yes == QMessageBox::warning(const_cast<MainWindow*>(this), tr("Size Warning"), msg, QMessageBox::Yes, QMessageBox::No);
    }
    return true;
}

bool MainWindow::confirmLargeDeviceWarning(const quint64 diskSizeGB) const
{
    if (diskSizeGB > sizeCheck) { // Warn user when writing to large drives (potentially unintended)
        const QString msg = tr("The target device %1 is larger than %2 GB.\n\n"
                         "This may indicate you have selected the wrong device.\n"
                         "Are you sure you want to proceed?")
                          .arg(device)
                          .arg(sizeCheck);
        const int ret = QMessageBox::warning(
            const_cast<MainWindow*>(this),
            tr("Large Target Device Warning"),
            msg,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        return (ret == QMessageBox::Yes);
    }
    return true;
}
