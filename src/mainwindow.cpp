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

#include <QDebug>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStorageInfo>
#include <QTextStream>

#include <chrono>

#include "about.h"
#include "common.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"

using namespace std::chrono_literals;

MainWindow::MainWindow(const QStringList &args, QDialog *parent)
    : QDialog(parent),
      ui(new Ui::MainWindow),
      cmd(this),
      timer(this),
      advancedOptions(false)
{
    ui->setupUi(this);
    setGeneralConnections();

    backendPath = LiveUsbMakerConfig::backendExecutablePath();
    if (backendPath.isEmpty()) {
        QMessageBox::critical(this, tr("Failure"),
                              tr("Could not find the live-usb backend helper."));
    }
    qDebug() << "LUM backend is:" << backendPath;

    // Load configuration settings (with defaults if file doesn't exist)
    if (QFileInfo::exists(AppPaths::CONFIG_FILE)) {
        qDebug() << "Loading configuration from:" << AppPaths::CONFIG_FILE;
        QSettings settings(AppPaths::CONFIG_FILE, QSettings::NativeFormat);
        sizeCheck = settings.value("SizeCheck", 128).toUInt(); // in GB
    } else {
        // Configuration file not found, using defaults
        sizeCheck = 128; // Default size check in GB
    }


    setWindowFlags(Qt::Window); // for the close, min, and max buttons
    setup();
    if (backendPath.isEmpty()) {
        ui->pushNext->setEnabled(false);
        ui->pushOptions->setEnabled(false);
    }
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
    const QStringList sizeLines = cmd.getOut(QString("lsblk --output SIZE -n --bytes /dev/%1").arg(device),
                                              Cmd::QuietMode::Yes).split('\n', Qt::SkipEmptyParts);
    const QString sizeOutput = sizeLines.isEmpty() ? QString() : sizeLines.first().trimmed();
    if (sizeOutput.isEmpty()) {
        qDebug() << "Empty lsblk output for device:" << device;
        return false;
    }
    const quint64 diskSizeBytes = sizeOutput.toULongLong(&ok);
    if (!ok) {
        qDebug() << "Failed to parse disk size for device:" << device << "Output:" << sizeOutput;
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
    const QStorageInfo storageInfo("/");
    const QString fsType = QString::fromUtf8(storageInfo.fileSystemType());
    if (liveFsTypes.contains(fsType)) {
        return true;
    }

    // Fallback: parse /proc/mounts for the root filesystem
    QFile mountsFile(SystemPaths::PROC_MOUNTS);
    if (mountsFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&mountsFile);
        while (!in.atEnd()) {
            const QString line = in.readLine();
            // /proc/mounts format: device mountpoint fstype ...
            // We want the line where mountpoint is /
            const auto parts = line.split(' ', Qt::SkipEmptyParts);
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
    return QFileInfo::exists(LivePaths::DID_TORAM);
}

void MainWindow::makeUsb(const QString &options)
{
    Q_UNUSED(options);
    if (backendPath.isEmpty()) {
        showErrorAndReset(tr("Could not find the live-usb backend helper."));
        return;
    }

    // Extract device name and validate it's not empty
    const QString currentText = ui->comboUsb->currentText();
    if (currentText.isEmpty()) {
        showErrorAndReset(tr("No USB device selected."));
        return;
    }
    device = currentText.split(' ').first();
    if (device.isEmpty()) {
        showErrorAndReset(tr("Invalid USB device selection."));
        return;
    }

    QString source = '"' + ui->pushSelectSource->property("filename").toString() + '"';
    QString sourceSize;

    auto firstField = [](const QString &out) {
        return out.section('\t', 0, 0).trimmed();
    };
    if (!ui->checkCloneLive->isChecked() && !ui->checkCloneMode->isChecked()) {
        sourceSize = firstField(cmd.getOut(QString("du -m %1 2>/dev/null").arg(source), Cmd::QuietMode::Yes));
    } else if (ui->checkCloneMode->isChecked()) {
        sourceSize = firstField(cmd.getOut(QString("du -m --summarize %1 2>/dev/null").arg(source), Cmd::QuietMode::Yes));
        const QStringList dfLines = cmd.getOut(QString("df --output=source %1").arg(source))
                                        .split('\n', Qt::SkipEmptyParts);
        const QString rootPartition = dfLines.isEmpty() ? QString()
                                                        : dfLines.last().section(' ', 0, 0, QString::SectionSkipEmpty).trimmed();
        source = "clone=" + source.remove('"');
        // Check if source and destination are on the same device (only if rootPartition is valid)
        if (!rootPartition.isEmpty() && "/dev/" + device == getDrivePath(rootPartition)) {
            showErrorAndReset(tr("Source and destination are on the same device, please select again."));
            return;
        }
    } else if (ui->checkCloneLive->isChecked()) {
        source = "clone";
        QString path = isToRam() ? LivePaths::TO_RAM : LivePaths::BOOT_DEV;
        sourceSize = firstField(cmd.getOut(QString("du -m --summarize %1 2>/dev/null").arg(path), Cmd::QuietMode::Yes));
    }

    if (!checkDestSize()) {
        showErrorAndReset();
        return;
    }

    // Check amount of IO on device before copy, this is in sectors
    bool ok = false;
    const quint64 startIo = cmd.getOut(QString("awk '{print $7}' /sys/block/%1/stat").arg(device), Cmd::QuietMode::Yes).toULongLong(&ok);
    if (!ok) {
        qDebug() << "Failed to parse initial IO stats for device:" << device;
        ui->progBar->setMinimum(0); // Use default values
    } else {
        ui->progBar->setMinimum(static_cast<int>(startIo));
    }

    const quint64 sourceSizeMB = sourceSize.toULongLong(&ok);
    if (!ok) {
        qDebug() << "Failed to parse source size:" << sourceSize;
        ui->progBar->setMaximum(100); // Use default progress range
    } else {
        const quint64 isoSectors = sourceSizeMB * SECTORS_PER_MB;
        ui->progBar->setMaximum(static_cast<int>(isoSectors + startIo));
    }

    QString error;
    const QString configPath = writeBackendConfig(&error);
    if (configPath.isEmpty()) {
        showErrorAndReset(tr("Failed to prepare the backend config. %1").arg(error));
        return;
    }
    lastConfigPath = configPath;
    const QStringList backendArgs{"--config", configPath};
    qDebug() << "Running backend command:" << backendPath << backendArgs;
    qDebug() << "Backend config path:" << configPath;
    operationInProgress = true;
    setConnections();
    elapsedTimer.start();
    cmd.procAsRoot(backendPath, backendArgs, nullptr, nullptr, Cmd::QuietMode::No);
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
    ui->outputBox->setReadOnly(true);
    adjustSize();
    height = geometry().height();

    // Volume label validator: alphanumeric and underscore, 1-11 characters (FAT32 limit)
    QRegularExpression rx("^[A-Za-z0-9_]{1,11}$");
    auto *validator = new QRegularExpressionValidator(rx, this);
    ui->textLabel->setValidator(validator);

    // Set save boot directory option to disable unless update mode is checked
    ui->checkSaveBoot->setEnabled(false);

    // Enable clone live option only if running live and not encrypted
    ui->checkCloneLive->setEnabled(isRunningLive() && !QFile::exists(LivePaths::ENCRYPTED));

    // Dynamically show or hide data format options based on availability
    bool dataFirstAvailable = true;
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
    connect(ui->checkCloneLive, &QCheckBox::clicked, this, &MainWindow::checkCloneLiveClicked);
    connect(ui->checkCloneMode, &QCheckBox::clicked, this, &MainWindow::checkCloneModeClicked);
    connect(ui->checkDataFirst, &QCheckBox::clicked, this, &MainWindow::checkDataFirstClicked);
    connect(ui->checkUpdate, &QCheckBox::clicked, this, &MainWindow::checkUpdateClicked);
    connect(ui->pushAbout, &QPushButton::clicked, this, &MainWindow::pushAboutClicked);
    connect(ui->pushBack, &QPushButton::clicked, this, &MainWindow::pushBackClicked);
    connect(ui->pushCancel, &QPushButton::clicked, this, &MainWindow::close);
    connect(ui->pushHelp, &QPushButton::clicked, this, &MainWindow::pushHelpClicked);
    connect(ui->pushLumLogFile, &QPushButton::clicked, this, &MainWindow::pushLumLogFileClicked);
    connect(ui->pushNext, &QPushButton::clicked, this, &MainWindow::pushNextClicked);
    connect(ui->pushOptions, &QPushButton::clicked, this, &MainWindow::pushOptionsClicked);
    connect(ui->pushRefresh, &QPushButton::clicked, this, &MainWindow::pushRefreshClicked);
    connect(ui->pushSelectSource, &QPushButton::clicked, this, &MainWindow::pushSelectSourceClicked);
    connect(ui->radioDd, &QRadioButton::clicked, this, &MainWindow::radioDdClicked);
    connect(ui->radioNormal, &QRadioButton::clicked, this, &MainWindow::radioNormalClicked);
    connect(ui->spinBoxSize, QOverload<int>::of(&QSpinBox::valueChanged), this, &MainWindow::spinBoxSizeValueChanged);
    connect(ui->textLabel, &QLineEdit::textChanged, this, &MainWindow::textLabelTextChanged);
}

// Build the option list to be passed to live-usb-maker
QString MainWindow::buildOptionList()
{
    QStringList optionsList{"-N"};
    const bool updateMode = ui->checkUpdate->isChecked();

    // Map the checkboxes to the corresponding options
    // Note: partition/format options are skipped in update mode as a safeguard
    std::map<QCheckBox *, QString> checkboxOptions{
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

    const auto options = optionsList.join(' ');
    qDebug() << "Options: " << options;
    return options;
}

LiveUsbMakerConfig MainWindow::buildConfig() const
{
    LiveUsbMakerConfig config;
    config.mode = ui->radioDd->isChecked() ? LiveUsbMakerConfig::Mode::Dd : LiveUsbMakerConfig::Mode::Normal;

    if (ui->checkCloneLive->isChecked()) {
        config.sourceMode = LiveUsbMakerConfig::SourceMode::Clone;
        config.sourcePath = QStringLiteral("clone");
    } else if (ui->checkCloneMode->isChecked()) {
        config.sourceMode = LiveUsbMakerConfig::SourceMode::CloneDir;
        config.sourcePath = ui->pushSelectSource->property("filename").toString();
    } else {
        config.sourceMode = LiveUsbMakerConfig::SourceMode::Iso;
        config.sourcePath = ui->pushSelectSource->property("filename").toString();
    }

    const QString target = ui->comboUsb->currentText().split(' ').first();
    config.targetDevice = expandDevicePath(target);

    config.pretend = ui->checkPretend->isChecked();
    config.update = ui->checkUpdate->isChecked();
    config.keepSyslinux = ui->checkKeep->isChecked();
    config.saveBoot = ui->checkSaveBoot->isChecked();
    config.encrypt = ui->checkEncrypt->isChecked();
    config.gpt = ui->checkGpt->isChecked();
    config.pmbr = ui->checkSetPmbrBoot->isChecked();
    config.forceUsb = ui->checkForceUsb->isChecked();
    config.forceAutomount = ui->checkForceAutomount->isChecked();
    config.forceMakefs = ui->checkForceMakefs->isChecked();
    config.forceNofuse = ui->checkForceNofuse->isChecked();

    config.espSizeMiB = ui->spinBoxEsp->value();
    config.label = ui->textLabel->text();

    config.dataFirst = ui->checkDataFirst->isChecked();
    config.dataPercent = ui->spinBoxDataSize->value();
    config.dataFs = ui->comboBoxDataFormat->currentText();

    if (config.dataFirst) {
        config.mainPercent = 100 - config.dataPercent;
    } else {
        config.mainPercent = ui->spinBoxSize->value();
    }

    config.verbosity = ui->sliderVerbosity->value();
    config.clonePersist = true;
    return config;
}

QString MainWindow::writeBackendConfig(QString *error) const
{
    const LiveUsbMakerConfig config = buildConfig();
    return LiveUsbMakerConfig::writeToTempFile(config, error);
}

void MainWindow::cleanup()
{
    // Use a utility Cmd object for cleanup operations (separate from main cmd member)
    Cmd utilCmd(this);

    // Check if we actually did any work that needs privileged cleanup
    bool needsPrivilegedCleanup = false;

    // Check 1: Are there any mount points?
    const QString mountPath = AppPaths::WORK_DIR;
    if (utilCmd.run("mountpoint -q " + mountPath, Cmd::QuietMode::Yes) ||
        utilCmd.run("mountpoint -q " + mountPath + "/main", Cmd::QuietMode::Yes)) {
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
                    Cmd utilCmd2(this);
                    utilCmd2.procAsRoot(QStringLiteral("kill"), {"-9", "--", "-" + QString::number(cmd.processId())},
                                        nullptr, nullptr, Cmd::QuietMode::Yes);
                }
            });
            utilCmd.procAsRoot(QStringLiteral("kill"), {"--", "-" + QString::number(cmd.processId())}, nullptr,
                               nullptr, Cmd::QuietMode::Yes);
        }

        // Attempt to unmount filesystems and check for failures
        bool unmountFailed = false;
        if (utilCmd.run("mountpoint -q " + mountPath, Cmd::QuietMode::Yes)) {
            if (!utilCmd.procAsRoot(QStringLiteral("umount"), {"-R", "-l", mountPath}, nullptr, nullptr,
                                    Cmd::QuietMode::Yes)) {
                qWarning() << "Failed to unmount" << mountPath;
                unmountFailed = true;
            }
        }
        if (utilCmd.run("mountpoint -q " + mountPath + "/main", Cmd::QuietMode::Yes)) {
            if (!utilCmd.procAsRoot(QStringLiteral("umount"), {"-l", mountPath + "/main", mountPath + "/uefi"},
                                    nullptr, nullptr, Cmd::QuietMode::Yes)) {
                qWarning() << "Failed to unmount" << mountPath + "/{main,uefi}";
                unmountFailed = true;
            }
        }

        // Warn user if unmount operations failed
        if (unmountFailed) {
            qWarning() << "Cleanup incomplete: some filesystems could not be unmounted";
            qWarning() << "You may need to manually unmount" << mountPath;
        }

        const QString pid = QString::number(QApplication::applicationPid());
        if (!utilCmd.run("ps --ppid " + pid, Cmd::QuietMode::Yes)) {
            utilCmd.procAsRoot(QStringLiteral("kill"), {"--", "-" + pid}, nullptr, nullptr, Cmd::QuietMode::Yes);
        }
    }

    // Move the application log to /var/log/ at cleanup stage
    const QString tempLog = SystemPaths::TMP_DIR + "/" + QApplication::applicationName() + ".log";
    if (QFileInfo::exists(tempLog)) {
        const QString helperPath = "/usr/lib/" + QApplication::applicationName() + "/live-usb-maker-lib";
        utilCmd.runWithPolkitAction("org.mxlinux.pkexec.live-usb-maker-lib", helperPath, {"copy_log"}, Cmd::QuietMode::Yes);
    }
}

QStringList MainWindow::buildUsbList()
{
    const QString drives = cmd.getOut("lsblk --nodeps -nlo NAME,SIZE,MODEL,VENDOR -I 3,8,22,179,259", Cmd::QuietMode::Yes).trimmed();
    // Validate that lsblk output has at least NAME and SIZE columns (MODEL/VENDOR can be empty)
    if (!ValidationUtils::validateLsblkColumns(drives, 2)) {
        qDebug() << "Invalid lsblk output format - expected at least 2 columns";
        return {};
    }
    return removeUnsuitable(drives.split('\n'));
}

QString MainWindow::expandDevicePath(const QString &device)
{
    const QString normalized = DeviceUtils::normalizePath(device);
    if (!DeviceUtils::isBlockDevice(normalized)) {
        return {};
    }
    return normalized;
}

QString MainWindow::getDriveName(const QString &device)
{
    return DeviceUtils::baseDriveName(device);
}

QString MainWindow::getDrivePath(const QString &device)
{
    return DeviceUtils::baseDrivePath(device);
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
        QDir uuidDir(SystemPaths::DEV_DISK_BY_UUID);
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
        QFile mounts(SystemPaths::PROC_MOUNTS);
        if (mounts.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&mounts);
            while (!in.atEnd()) {
                const QString line = in.readLine();
                const auto parts = line.split(' ', Qt::SkipEmptyParts);
                if (parts.size() > 1 && parts.at(1) == "/live/boot-dev") {
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
    const QString lsblkOutput
        = cmd.getOut("lsblk -nlso NAME,PKNAME,TYPE $(findmnt / -no SOURCE)", Cmd::QuietMode::Yes).trimmed();
    // Validate lsblk output format (3 columns: NAME, PKNAME, TYPE)
    if (!ValidationUtils::validateLsblkColumns(lsblkOutput, 3)) {
        // Invalid lsblk output format for root device - expected at least 3 columns
    }
    // Extract root drive name from validated output
    const QString rootDrive = lsblkOutput.split('\n')
                                  .filter(QStringLiteral("disk"))
                                  .value(0)
                                  .section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
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
    operationInProgress = false;
    ui->progBar->setValue(ui->progBar->maximum());
    setCursor(Qt::ArrowCursor);
    ui->pushBack->show();
    qDebug() << "Backend finished with exit code:" << cmd.exitCode() << "exit status:" << cmd.exitStatus();
    if (cmd.exitCode() != 0 || cmd.exitStatus() != QProcess::NormalExit) {
        qDebug() << "Backend QProcess error:" << cmd.error() << cmd.errorString();
    }
    if ((cmd.exitCode() == 0 && cmd.exitStatus() == QProcess::NormalExit) || ui->checkPretend->isChecked()) {
        qint64 elapsedMs = elapsedTimer.elapsed();
        int totalSeconds = elapsedMs / 1000;
        int minutes = totalSeconds / 60;
        int seconds = totalSeconds % 60;
        QString timeStr = QString("%1:%2").arg(minutes, 2, 10, QChar('0')).arg(seconds, 2, 10, QChar('0'));
        QMessageBox::information(this, tr("Success"), tr("LiveUSB creation successful!"));
        ui->outputBox->appendPlainText("\n" + tr("Elapsed time: %1").arg(timeStr));
        qDebug() << "LiveUSB creation successful";
    } else {
        cleanup();
        if (!lastConfigPath.isEmpty()) {
            QFileInfo configInfo(lastConfigPath);
            qDebug() << "Backend config exists:" << configInfo.exists() << "size:" << configInfo.size();
            if (configInfo.exists() && configInfo.size() <= 4096) {
                QFile configFile(lastConfigPath);
                if (configFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    qDebug().noquote() << "Backend config content:" << QString::fromUtf8(configFile.readAll());
                }
            }
        }
        QString errorMsg = tr("Error encountered in the LiveUSB creation process");
        errorMsg += QString("\n\nExit code: %1\nExit status: %2").arg(cmd.exitCode()).arg(cmd.exitStatus());
        QMessageBox::critical(this, tr("Failure"), errorMsg);
        qDebug() << "LiveUSB creation failed with exit code:" << cmd.exitCode() << "status:" << cmd.exitStatus();
    }
    cmd.disconnect();
}

void MainWindow::setConnections()
{
    // Make all signal/slot connections before starting the timer to avoid race conditions
    // Use Qt::UniqueConnection to prevent duplicate connections if called multiple times
    connect(&cmd, &Cmd::readyReadStandardOutput, this, &MainWindow::updateOutput, Qt::UniqueConnection);
    connect(&cmd, &Cmd::readyReadStandardError, this, &MainWindow::updateOutput, Qt::UniqueConnection);
    connect(&timer, &QTimer::timeout, this, &MainWindow::updateBar, Qt::UniqueConnection);
    connect(&cmd, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, &MainWindow::cmdDone, Qt::UniqueConnection);

    // Start timer after connections are established
    timer.start(1s);
}

// Set proper default mode based on iso contents
void MainWindow::setDefaultMode(const QString &isoName)
{
    const bool isMxFamily = isAntiXMxFamily(isoName);
    const bool isArchFamily = isArchIsoFamily(isoName);
    if (!isMxFamily && !isArchFamily) {
        ui->radioDd->click();
        ui->radioNormal->setChecked(false);
    } else {
        ui->radioDd->setChecked(false);
        ui->radioNormal->click();
    }
}

void MainWindow::updateBar()
{
    // Guard against updates when no operation is in progress
    if (!operationInProgress) {
        return;
    }

    // Validate device name before using it
    if (device.isEmpty() || !DeviceUtils::isValidDeviceName(device)) {
        qDebug() << "Invalid or empty device name in updateBar";
        return;
    }

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
    static const QRegularExpression whitespaceRe(QStringLiteral("\\s+"));
    quint64 currentIo = out.section(whitespaceRe, 7, 7).toULongLong();
    ui->progBar->setValue(static_cast<int>(currentIo));
    statFile.close();
}

void MainWindow::updateOutput()
{
    QString output = cmd.readAllStandardOutput();
    const QString errorOutput = cmd.readAllStandardError();
    if (!errorOutput.isEmpty()) {
        qDebug().noquote() << "Backend stderr:" << errorOutput.trimmed();
        output.append(errorOutput);
    }
    // Strip ANSI sequences and control chars that can show up in command output.
    static const QRegularExpression kAnsiCsiRegex(QStringLiteral("\\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    static const QRegularExpression kAnsiOscRegex(QStringLiteral("\\x1b\\][^\\x07\\x1b]*(?:\\x07|\\x1b\\\\)"));
    static const QRegularExpression kControlCharsRegex(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]"));
    output.remove(kAnsiOscRegex);
    output.remove(kAnsiCsiRegex);
    const bool hasCarriageReturn = output.contains('\r');
    output.remove('\r');
    output.remove(kControlCharsRegex);
    ui->outputBox->moveCursor(QTextCursor::End);
    if (hasCarriageReturn) {
        ui->outputBox->moveCursor(QTextCursor::Up, QTextCursor::KeepAnchor);
        ui->outputBox->moveCursor(QTextCursor::EndOfLine, QTextCursor::KeepAnchor);
    }
    ui->outputBox->insertPlainText(output);

    QScrollBar *sb = ui->outputBox->verticalScrollBar();
    sb->setValue(sb->maximum());
}

void MainWindow::pushNextClicked()
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

void MainWindow::pushBackClicked()
{
    setWindowTitle("MX Live Usb Maker");
    ui->stackedWidget->setCurrentIndex(0);
    ui->pushNext->setEnabled(true);
    ui->pushBack->hide();
    ui->outputBox->clear();
    ui->progBar->setValue(0);
}

void MainWindow::pushAboutClicked()
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

void MainWindow::pushHelpClicked()
{
    QString url = "/usr/share/doc/mx-live-usb-maker/mx-live-usb-maker.html";
    displayHelpDoc(url, tr("%1 Help").arg(windowTitle()));
}

void MainWindow::pushSelectSourceClicked()
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
            QMessageBox::critical(this, tr("Failure"), tr("Could not find linuxfs file in %1").arg(selected));
        }
    }
}

void MainWindow::pushRefreshClicked()
{
    ui->comboUsb->clear();
    ui->comboUsb->addItems(buildUsbList());
}

void MainWindow::pushOptionsClicked()
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

void MainWindow::textLabelTextChanged(const QString &text)
{
    QString cleaned = text;
    ui->textLabel->setText(cleaned.remove(' '));
    ui->textLabel->setCursorPosition(cleaned.length());
}

void MainWindow::checkUpdateClicked(bool checked)
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

void MainWindow::checkCloneModeClicked(bool checked)
{
    if (checked) {
        ui->pushSelectSource->setStyleSheet("text-align: center;");
        ui->checkCloneLive->setChecked(false);
        checkCloneLiveClicked(false);
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

void MainWindow::checkCloneLiveClicked(bool checked)
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

void MainWindow::radioDdClicked()
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
        pushOptionsClicked();
    }
    ui->pushOptions->setEnabled(false);
    ui->label_percent->setEnabled(false);
    ui->label_part_label->setEnabled(false);
    ui->spinBoxSize->setEnabled(false);
    ui->textLabel->setEnabled(false);
}

void MainWindow::radioNormalClicked()
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

bool MainWindow::isAntiXMxFamily(const QString &selected)
{
    Cmd utilCmd(nullptr);
    const QString out = utilCmd.getOut(
        QStringLiteral("xorriso -indev '%1' -find /antiX -name linuxfs -prune  2>/dev/null").arg(selected),
        Cmd::QuietMode::Yes);
    return out.contains(QStringLiteral("/antiX/linuxfs"));
}

bool MainWindow::isArchIsoFamily(const QString &selected)
{
    Cmd utilCmd(nullptr);
    static const QRegularExpression sfsRx(QStringLiteral("/arch/.*/airootfs\\.sfs"));
    static const QRegularExpression erofsRx(QStringLiteral("/arch/.*/airootfs\\.erofs"));
    if (sfsRx.match(utilCmd.getOut(
            QStringLiteral("xorriso -indev '%1' -find /arch -name airootfs.sfs -prune  2>/dev/null").arg(selected),
            Cmd::QuietMode::Yes)).hasMatch()) {
        return true;
    }
    return erofsRx.match(utilCmd.getOut(
        QStringLiteral("xorriso -indev '%1' -find /arch -name airootfs.erofs -prune  2>/dev/null").arg(selected),
        Cmd::QuietMode::Yes)).hasMatch();
}

void MainWindow::pushLumLogFileClicked()
{
    const QString logFileName = QStringLiteral("live-usb-maker.log");
    const QString logFilePath = AppPaths::LOG_FILE;
    const QString tempLogFilePath = SystemPaths::TMP_DIR + QStringLiteral("/live-usb-maker.log");
    qDebug() << "lumlog" << tempLogFilePath;

    if (!QFileInfo::exists(logFilePath)) {
        QMessageBox::information(this, QApplication::applicationName(),
                                 tr("Could not find a log file at: ") + logFilePath);
        return;
    }

    // Generate temporary log file by reversing the log file until the delimiter, then reversing it back
    const QString cmdStr = QString("tac %1 | sed '/^={60}=$/q' | tac > %2").arg(logFilePath, tempLogFilePath);
    Cmd utilCmd(this);
    utilCmd.run(cmdStr);
    displayDoc(tempLogFilePath, QStringLiteral("live-usb-maker"));
}

void MainWindow::spinBoxSizeValueChanged(int value)
{
    // Don't re-enable controls if update mode is checked
    if (ui->checkUpdate->isChecked()) {
        return;
    }
    const int max = ui->spinBoxSize->maximum();
    ui->checkDataFirst->setEnabled(value == max);
    ui->spinBoxDataSize->setEnabled(value == max);
    ui->comboBoxDataFormat->setEnabled(value == max);
    ui->labelFormat->setEnabled(value == max);
}

void MainWindow::checkDataFirstClicked(bool checked)
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

QString MainWindow::getLinuxfsPath(const QString &sourceFilename)
{
    if (ui->checkCloneLive->isChecked()) {
        // Clone live system mode: use current live system
        return LivePaths::LINUX;
    }

    // Clone mode: check for linuxfs file in standard locations
    QString linuxfsPath = sourceFilename + "/antiX/linuxfs";
    if (QFileInfo::exists(linuxfsPath)) {
        return linuxfsPath;
    }

    linuxfsPath = sourceFilename + "/linuxfs";
    if (QFileInfo::exists(linuxfsPath)) {
        return linuxfsPath;
    }

    return QString();  // Not found
}

quint64 MainWindow::calculateLinuxfsSize(const QString &linuxfsPath)
{
    const QFileInfo info(linuxfsPath);
    if (info.isDir()) {
        // For directories, get used space via df command
        const QString cmdStr = QString("df --output=used -B1 \"%1\"").arg(linuxfsPath);
        const QStringList dfLines = cmd.getOut(cmdStr, Cmd::QuietMode::Yes).split('\n', Qt::SkipEmptyParts);
        const QString usedStr = dfLines.isEmpty() ? QString() : dfLines.last().trimmed();
        bool ok = false;
        const quint64 sourceSizeBytes = usedStr.toULongLong(&ok);
        return ok ? sourceSizeBytes : 0;
    }
    // For files, get size directly
    return info.size();
}

quint64 MainWindow::calculateIsoSize(const QString &isoFilename)
{
    if (isoFilename.isEmpty() || !QFileInfo::exists(isoFilename)) {
        qDebug() << "ISO file does not exist or not specified:" << isoFilename;
        return 0;
    }

    const quint64 sourceSizeBytes = QFileInfo(isoFilename).size();
    qDebug() << "ISO file size (bytes):" << sourceSizeBytes;
    return sourceSizeBytes;
}

quint64 MainWindow::calculateSourceSize()
{
    const QString sourceFilename = ui->pushSelectSource->property("filename").toString();
    qDebug() << "Source filename property:" << sourceFilename;

    // Handle source size calculation based on operation mode
    if (ui->checkCloneMode->isChecked() || ui->checkCloneLive->isChecked()) {
        const QString linuxfsPath = getLinuxfsPath(sourceFilename);

        if (linuxfsPath.isEmpty()) {
            // Show warning only in clone mode (not for live cloning)
            if (ui->checkCloneMode->isChecked()) {
                QMessageBox::warning(this, tr("Source Error"),
                    tr("Could not find the source linuxfs file."));
            }
            return 0;
        }

        return calculateLinuxfsSize(linuxfsPath);
    }

    // Standard ISO file mode
    return calculateIsoSize(sourceFilename);
}

bool MainWindow::validateSizeCompatibility(const quint64 sourceSize, const quint64 diskSize)
{
    // Check if source is larger than destination
    if (sourceSize > 0 && diskSize < sourceSize) {
        const QString msg = tr("Warning: The target device (%1) is smaller than the source (%2). The data might not fit. Do you want to continue?")
                        .arg(device)
                        .arg(ui->pushSelectSource->text());
        return QMessageBox::Yes == QMessageBox::warning(this, tr("Size Warning"), msg, QMessageBox::Yes, QMessageBox::No);
    }
    return true;
}

bool MainWindow::confirmLargeDeviceWarning(const quint64 diskSizeGB)
{
    if (diskSizeGB > sizeCheck) { // Warn user when writing to large drives (potentially unintended)
        const QString msg = tr("The target device %1 is larger than %2 GB.\n\n"
                         "This may indicate you have selected the wrong device.\n"
                         "Are you sure you want to proceed?")
                          .arg(device)
                          .arg(sizeCheck);
        const int ret = QMessageBox::warning(
            this,
            tr("Large Target Device Warning"),
            msg,
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        return (ret == QMessageBox::Yes);
    }
    return true;
}
