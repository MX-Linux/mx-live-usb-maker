/**********************************************************************
 *  liveusbmaker_backend.cpp
 **********************************************************************
 * Copyright (C) 2025 MX Authors
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

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTextStream>
#include <QSet>

#include "common.h"
#include "liveusbmaker_backend.h"

namespace
{
constexpr int kDefaultBiosSizeMiB = 150;
constexpr int kDefaultUefiSizeMiB = 50;
constexpr int kGptBiosBootSizeMiB = 2;
constexpr int kBiosMarginMiB = 20;
constexpr int kMainMarginMiB = 20;
constexpr int kUefiMarginMiB = 5;
constexpr int kExtOverheadMaxMiB = 44;
constexpr int kExtOverheadScale = 26;
constexpr int kExtOverheadDivisor = 10000;
constexpr int kExtOverheadOffset = 2;
constexpr int kPartitionStartMiB = 1;
const QString kMainFsType = QStringLiteral("ext4");
const QString kUefiFsType = QStringLiteral("vfat");
const QString kBiosLabel = QStringLiteral("Live-usb");
const QString kEspLabel = QStringLiteral("LIVE-UEFI");
const QString kLuksName = QStringLiteral("live-usb-maker");
const QString kEncryptEnable = QStringLiteral("enable");
const QString kDefaultBootDir = QStringLiteral("antiX");
const QString kLinuxfsName = QStringLiteral("linuxfs");
const QString kArchIsoDir = QStringLiteral("arch");
const QString kArchIsoSfsName = QStringLiteral("airootfs.sfs");
const QString kArchIsoErofsName = QStringLiteral("airootfs.erofs");
const QString kArchIsoGrubCfg = QStringLiteral("boot/grub/grub.cfg");

const QString kUefiFilesSpec = QStringLiteral("[Ee][Ff][Ii] boot/{grub,uefi-mt} version");
const QString kUefiFilesSpec2 = QStringLiteral("[Ee][Ff][Ii] version");
const QString kUefiFilesSpecArch = QStringLiteral("efi/boot");
const QString kBiosFilesSpec = QStringLiteral("[Ee][Ff][Ii] boot/{syslinux,grub,memtest} antiX/{vmlinuz,initrd}* version");
const QString kCloneDirsSpec = QStringLiteral("boot EFI efi");
const QString kCloneFilesSpec = QStringLiteral("cdrom.ico version");
const QString kCloneLinuxfsSpec = QStringLiteral("linuxfs{,.md5}");
const QString kCloneBootSpec = QStringLiteral("{vmlinuz{,{1..9}},initrd{,{1..9}}.gz}{,.md5,.ver}");
const QString kSyslinuxFiles = QStringLiteral("chain.c32 gfxboot.c32 vesamenu.c32 ldlinux.c32 libcom32.c32 "
                                              "libmenu.c32 libutil.c32 linux.c32 menu.c32");
const QString kSyslinuxRemove = QStringLiteral("*.c32 ldlinux.sys syslinux.bin isolinux.bin version");
} // namespace

LiveUsbMakerBackend::LiveUsbMakerBackend(const LiveUsbMakerConfig &config)
    : config(config)
{
    paths.workDir = AppPaths::WORK_DIR;
    paths.isoDir = QDir(paths.workDir).filePath(QStringLiteral("iso"));
    paths.mainDir = QDir(paths.workDir).filePath(QStringLiteral("main"));
    paths.biosDir = QDir(paths.workDir).filePath(QStringLiteral("bios"));
    paths.uefiDir = QDir(paths.workDir).filePath(QStringLiteral("uefi"));
    paths.dataDir = QDir(paths.workDir).filePath(QStringLiteral("data"));
    paths.initrdDir = QDir(paths.workDir).filePath(QStringLiteral("initrd"));
    paths.linuxDir = QDir(paths.workDir).filePath(QStringLiteral("linux"));
}

bool LiveUsbMakerBackend::run(QString *error)
{
    if (!prepareWorkDirs(error)) {
        return false;
    }
    if (!prepareSource(error)) {
        cleanup();
        return false;
    }
    bool ok = false;
    if (config.mode == LiveUsbMakerConfig::Mode::Dd) {
        ok = runDd(error);
    } else {
        ok = runNormal(error);
    }
    cleanup();
    return ok;
}

bool LiveUsbMakerBackend::runDd(QString *error)
{
    if (!checkTargetDevice(error)) {
        return false;
    }
    if (config.pretend) {
        logLine(QStringLiteral("Pretend mode: skipping dd write."));
        return true;
    }
    if (!clearPartitionTable(error)) {
        return false;
    }
    if (config.sourcePath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Missing source ISO for dd mode.");
        }
        return false;
    }
    logLine(QStringLiteral("Writing ISO to device with dd."));
    return runCommand(QStringLiteral("dd"),
                      {QStringLiteral("bs=1M"),
                       QStringLiteral("if=%1").arg(config.sourcePath),
                       QStringLiteral("of=%1").arg(config.targetDevice)},
                      error);
}

bool LiveUsbMakerBackend::runNormal(QString *error)
{
    if (!checkTargetDevice(error)) {
        return false;
    }
    detectArchIsoLayout();
    if (archIso) {
        logLine(QStringLiteral("Detected archiso layout for full-featured mode."));
    }
    if (!computeLayout(error)) {
        return false;
    }

    if (!config.forceAutomount) {
        suspendAutomount();
    }

    if (!config.update) {
        if (!clearPartitionTable(error)) {
            resumeAutomount();
            return false;
        }
        if (!partitionDevice(error)) {
            resumeAutomount();
            return false;
        }
        if (!makeFileSystems(error)) {
            resumeAutomount();
            return false;
        }
    }

    if (config.pretend) {
        logLine(QStringLiteral("Pretend mode: skipping write steps."));
        resumeAutomount();
        return true;
    }

    bool initrdPrepared = false;
    if (config.encrypt) {
        if (!mountTargets(false, error)) {
            resumeAutomount();
            return false;
        }
        if (!prepareInitrdEncryption(error)) {
            resumeAutomount();
            return false;
        }
        initrdPrepared = true;
        if (!encryptMainPartition(error)) {
            resumeAutomount();
            return false;
        }
    } else {
        if (!mountTargets(true, error)) {
            resumeAutomount();
            return false;
        }
    }

    if (!copyMain(error)) {
        resumeAutomount();
        return false;
    }
    if (!checkUsbMd5(error)) {
        resumeAutomount();
        return false;
    }
    if (!copyBios(error)) {
        resumeAutomount();
        return false;
    }
    if (config.encrypt && initrdPrepared) {
        if (!finalizeInitrdEncryption(error)) {
            resumeAutomount();
            return false;
        }
    }
    if (!copyUefi(error)) {
        resumeAutomount();
        return false;
    }

    if (archIso) {
        if (!setupArchLiveUsbStorage()) {
            logWarn(QStringLiteral("Could not set up live-usb-storage for Arch ISO."));
        }
        if (!updateArchIsoBootConfig(error)) {
            resumeAutomount();
            return false;
        }
        if (!installArchIsoBootloader(error)) {
            resumeAutomount();
            return false;
        }
    } else {
        if (!updateUuids(error)) {
            resumeAutomount();
            return false;
        }
        if (!writeDataUuid(error)) {
            resumeAutomount();
            return false;
        }
        if (!installBootloader(error)) {
            resumeAutomount();
            return false;
        }
    }

    resumeAutomount();
    return true;
}

bool LiveUsbMakerBackend::suspendAutomount()
{
    logLine(QStringLiteral("Suspending automount."));
    bool ok = true;
    ok &= runCommand(QStringLiteral("pkill"), {QStringLiteral("-STOP"), QStringLiteral("udevil")}, nullptr, true);
    ok &= runCommand(QStringLiteral("pkill"), {QStringLiteral("-STOP"), QStringLiteral("devmon")}, nullptr, true);
    return ok;
}

void LiveUsbMakerBackend::resumeAutomount()
{
    logLine(QStringLiteral("Resuming automount."));
    runCommand(QStringLiteral("pkill"), {QStringLiteral("-CONT"), QStringLiteral("devmon")}, nullptr, true);
    runCommand(QStringLiteral("pkill"), {QStringLiteral("-CONT"), QStringLiteral("udevil")}, nullptr, true);
}

bool LiveUsbMakerBackend::prepareWorkDirs(QString *error)
{
    const QStringList dirs{paths.workDir, paths.isoDir, paths.mainDir, paths.biosDir,
                           paths.uefiDir, paths.dataDir, paths.initrdDir, paths.linuxDir};
    for (const QString &dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            if (error) {
                *error = QStringLiteral("Unable to create working directory.");
            }
            return false;
        }
    }
    if (error) {
        error->clear();
    }
    return true;
}

bool LiveUsbMakerBackend::prepareSource(QString *error)
{
    if (config.sourceMode == LiveUsbMakerConfig::SourceMode::Iso) {
        if (config.sourcePath.isEmpty()) {
            if (error) {
                *error = QStringLiteral("Missing ISO source path.");
            }
            return false;
        }
        logLine(QStringLiteral("Mounting ISO."));
        return runCommand(QStringLiteral("mount"),
                          {QStringLiteral("-o"), QStringLiteral("loop,ro"), config.sourcePath, paths.isoDir},
                          error);
    }
    if (config.sourceMode == LiveUsbMakerConfig::SourceMode::Clone) {
        const QString livePath = QFileInfo::exists(LivePaths::DID_TORAM)
                                     ? LivePaths::TO_RAM
                                     : LivePaths::BOOT_DEV;
        paths.isoDir = livePath;
        return true;
    }
    if (!QFileInfo::exists(config.sourcePath)) {
        if (error) {
            *error = QStringLiteral("Clone source path does not exist.");
        }
        return false;
    }
    paths.isoDir = config.sourcePath;
    return true;
}

bool LiveUsbMakerBackend::detectArchIsoLayout()
{
    archIso = false;
    archIsoArch.clear();

    const QDir archDir(QDir(paths.isoDir).filePath(kArchIsoDir));
    if (!archDir.exists()) {
        return false;
    }

    const QStringList archCandidates = archDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &arch : archCandidates) {
        const QString sfsPath = archDir.filePath(arch + QStringLiteral("/") + kArchIsoSfsName);
        const QString erofsPath = archDir.filePath(arch + QStringLiteral("/") + kArchIsoErofsName);
        if (QFileInfo::exists(sfsPath) || QFileInfo::exists(erofsPath)) {
            archIso = true;
            archIsoArch = arch;
            return true;
        }
    }

    return false;
}

bool LiveUsbMakerBackend::cleanup()
{
    unmountTargets();
    if (config.sourceMode == LiveUsbMakerConfig::SourceMode::Iso) {
        runCommandShell(QStringLiteral("umount -l ") + paths.isoDir + QStringLiteral(" 2>/dev/null"), nullptr, true);
    }
    return true;
}

bool LiveUsbMakerBackend::checkTargetDevice(QString *error)
{
    if (config.targetDevice.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Target device is missing.");
        }
        return false;
    }
    if (!QFileInfo::exists(config.targetDevice)) {
        if (error) {
            *error = QStringLiteral("Target device does not exist.");
        }
        return false;
    }
    if (!config.forceUsb && !isUsbOrRemovable(config.targetDevice)) {
        if (error) {
            *error = QStringLiteral("Target device does not appear to be USB or removable.");
        }
        return false;
    }
    layout.drive = config.targetDevice;
    return true;
}

bool LiveUsbMakerBackend::isUsbOrRemovable(const QString &device) const
{
    const QString devPath = devicePath(device);
    if (devPath.isEmpty()) {
        return false;
    }
    const QString driveName = DeviceUtils::baseDriveName(devPath);
    const QString sysBlock = SystemPaths::SYS_BLOCK + QStringLiteral("/") + driveName;
    if (!QFileInfo::exists(sysBlock)) {
        return false;
    }
    QFile removableFile(sysBlock + QStringLiteral("/removable"));
    if (removableFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString flag = QString::fromUtf8(removableFile.readAll()).trimmed();
        if (flag == QLatin1String("1")) {
            return true;
        }
    }
    const QString devPathReal = QFileInfo(sysBlock + QStringLiteral("/device")).canonicalFilePath();
    return devPathReal.contains(QStringLiteral("/usb"));
}

bool LiveUsbMakerBackend::clearPartitionTable(QString *error)
{
    logLine(QStringLiteral("Clearing partition table."));
    QString output;
    if (!runCommandOutput(QStringLiteral("parted"),
                          {QStringLiteral("--script"), layout.drive, QStringLiteral("unit"), QStringLiteral("B"), QStringLiteral("print")},
                          &output, error)) {
        return false;
    }
    static const QRegularExpression re(QStringLiteral("^Disk.*: ([0-9]+)B$"));
    qint64 bytes = 0;
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = re.match(line.trimmed());
        if (match.hasMatch()) {
            bytes = match.captured(1).toLongLong();
            break;
        }
    }
    const int blockSize = SECTOR_SIZE_BYTES;
    const int ptSize = PARTITION_TABLE_SIZE_BYTES;
    const int ptCount = ptSize / blockSize;
    const int sneakyOffset = SNEAKY_OFFSET_BYTES / blockSize;
    const qint64 totalBlocks = bytes / blockSize;

    if (!runCommand(QStringLiteral("dd"),
                    {QStringLiteral("if=/dev/zero"), QStringLiteral("of=%1").arg(layout.drive),
                     QStringLiteral("bs=%1").arg(blockSize),
                     QStringLiteral("count=%1").arg(ptCount)},
                    error)) {
        return false;
    }
    if (!runCommand(QStringLiteral("dd"),
                    {QStringLiteral("if=/dev/zero"), QStringLiteral("of=%1").arg(layout.drive),
                     QStringLiteral("bs=%1").arg(blockSize),
                     QStringLiteral("count=%1").arg(ptCount),
                     QStringLiteral("seek=%1").arg(64)},
                    error)) {
        return false;
    }

    if (bytes > 0) {
        const qint64 offset = totalBlocks - ptCount;
        if (!runCommand(QStringLiteral("dd"),
                        {QStringLiteral("if=/dev/zero"), QStringLiteral("of=%1").arg(layout.drive),
                         QStringLiteral("bs=%1").arg(blockSize),
                         QStringLiteral("count=%1").arg(ptCount),
                         QStringLiteral("seek=%1").arg(offset),
                         QStringLiteral("conv=notrunc")},
                        error)) {
            return false;
        }
    }
    return runCommand(QStringLiteral("partprobe"), {layout.drive}, error, true);
}

bool LiveUsbMakerBackend::computeLayout(QString *error)
{
    const int totalMiB = totalSizeMiB(error);
    if (totalMiB <= 0) {
        return false;
    }

    const int uefiSizeMiB = config.espSizeMiB > 0 ? config.espSizeMiB : kDefaultUefiSizeMiB;
    const int biosSizeMiB = config.encrypt ? kDefaultBiosSizeMiB : 0;
    const int gptBiosBootSizeMiB = config.gpt ? kGptBiosBootSizeMiB : 0;

    int uefiNeeded = duApparentSizeMiB(paths.isoDir, kUefiFilesSpec, error);
    if (archIso) {
        const int archUefiNeeded = duApparentSizeMiB(paths.isoDir, kUefiFilesSpecArch, error);
        if (archUefiNeeded > uefiNeeded) {
            uefiNeeded = archUefiNeeded;
        }
    }
    const int biosNeeded = duApparentSizeMiB(paths.isoDir, kBiosFilesSpec, error);

    int finalUefiSize = uefiSizeMiB;
    if (uefiNeeded > 0 && finalUefiSize < (uefiNeeded + kUefiMarginMiB)) {
        logWarn(QStringLiteral("UEFI size increased to fit content."));
        finalUefiSize = uefiNeeded + kUefiMarginMiB;
    }

    int finalBiosSize = biosSizeMiB;
    if (config.encrypt && finalBiosSize < (biosNeeded + kBiosMarginMiB)) {
        logWarn(QStringLiteral("BIOS size increased to fit content."));
        finalBiosSize = biosNeeded + kBiosMarginMiB;
    }

    const int percent = config.mainPercent > 0 ? config.mainPercent : 100;
    int allocMiB = (totalMiB * percent / 100) - 1;
    if (allocMiB <= 0) {
        if (error) {
            *error = QStringLiteral("Target device is too small.");
        }
        return false;
    }

    int dataMiB = 0;
    int mainMiB = allocMiB - finalUefiSize - finalBiosSize - gptBiosBootSizeMiB;
    if (config.dataFirst) {
        dataMiB = totalMiB - allocMiB;
        if (dataMiB <= 0) {
            if (error) {
                *error = QStringLiteral("Data partition size is too small.");
            }
            return false;
        }
        mainMiB = allocMiB - finalUefiSize - finalBiosSize - gptBiosBootSizeMiB;
    }

    const int mainNeeded = duApparentSizeMiB(paths.isoDir, QStringLiteral("*"), error);
    const int overhead = extOverheadMiB(mainMiB);
    const int mainNeededWithOverhead = mainNeeded + overhead + kMainMarginMiB;
    if (mainMiB < mainNeededWithOverhead) {
        if (error) {
            *error = QStringLiteral("Main partition is too small for the source content.");
        }
        return false;
    }

    // Calculate partition numbers based on layout configuration
    // For GPT, partition 1 is always the BIOS Boot partition (bios_grub)
    // Partition order varies based on GPT/MBR, encryption and data-first settings:
    // GPT:
    //   dataFirst + encrypt:    [1: bios_boot] [2: data] [3: bios] [4: main] [5: uefi]
    //   dataFirst + no encrypt: [1: bios_boot] [2: data] [3: main] [4: uefi]
    //   no dataFirst + encrypt: [1: bios_boot] [2: bios] [3: main] [4: uefi]
    //   no dataFirst + no encrypt: [1: bios_boot] [2: main] [3: uefi]
    // MBR:
    //   dataFirst + encrypt:    [1: data] [2: bios] [3: main] [4: uefi]
    //   dataFirst + no encrypt: [1: data] [2: main] [3: uefi]
    //   no dataFirst + encrypt: [1: bios] [2: main] [3: uefi]
    //   no dataFirst + no encrypt: [1: main] [2: uefi]

    const int gptOffset = config.gpt ? 1 : 0;  // GPT has BIOS Boot partition at position 1

    if (config.encrypt) {
        if (config.dataFirst) {
            layout.biosPart = 2 + gptOffset;
            layout.mainPart = 3 + gptOffset;
            layout.uefiPart = 4 + gptOffset;
        } else {
            layout.biosPart = 1 + gptOffset;
            layout.mainPart = 2 + gptOffset;
            layout.uefiPart = 3 + gptOffset;
        }
    } else {
        layout.biosPart = 0;  // No bios partition when not encrypted
        if (config.dataFirst) {
            layout.mainPart = 2 + gptOffset;
            layout.uefiPart = 3 + gptOffset;
        } else {
            layout.mainPart = 1 + gptOffset;
            layout.uefiPart = 2 + gptOffset;
        }
    }

    layout.biosDev = config.encrypt ? partitionPath(layout.drive, layout.biosPart) : QString();
    layout.mainDev = partitionPath(layout.drive, layout.mainPart);
    layout.uefiDev = partitionPath(layout.drive, layout.uefiPart);
    layout.dataDev = config.dataFirst ? partitionPath(layout.drive, 1 + gptOffset) : QString();
    layout.biosSizeMiB = finalBiosSize;
    layout.mainSizeMiB = mainMiB;
    layout.uefiSizeMiB = finalUefiSize;
    layout.dataSizeMiB = dataMiB;

    return true;
}

bool LiveUsbMakerBackend::partitionDevice(QString *error)
{
    const QString type = config.gpt ? QStringLiteral("gpt") : QStringLiteral("msdos");
    logLine(QStringLiteral("Partitioning device."));

    const QString preamble = QStringLiteral("parted --script --align optimal %1 unit MiB").arg(layout.drive);
    if (!runCommandShell(QStringLiteral("%1 mklabel %2").arg(preamble, type), error)) {
        return false;
    }

    int start = kPartitionStartMiB;
    int partNum = 0;

    auto addPartition = [&](int sizeMiB, const QString &fsType, const QString &name) -> bool {
        const int end = start + sizeMiB - 1;
        const QString partType = fsType.isEmpty() ? QString() : fsType;
        // For GPT use partition name, for MBR use "primary"
        const QString partLabel = config.gpt ? name : QStringLiteral("primary");
        const QString cmd = QStringLiteral("%1 mkpart %2 %3 %4 %5")
                                .arg(preamble, partLabel, partType, QString::number(start), QString::number(end));
        if (!runCommandShell(cmd, error)) {
            logError(QStringLiteral("Partitioning failed at %1 partition.").arg(name));
            return false;
        }
        start = end + 1;  // Next partition starts after this one ends
        partNum += 1;
        return true;
    };

    // For GPT, create a BIOS Boot partition first for BIOS/Legacy boot support
    if (config.gpt) {
        if (!addPartition(kGptBiosBootSizeMiB, QString(), QStringLiteral("bios_boot"))) {
            return false;
        }
        // Set bios_grub flag on the BIOS Boot partition
        if (!runCommandShell(QStringLiteral("%1 set %2 bios_grub on").arg(preamble).arg(partNum), error)) {
            logError(QStringLiteral("Failed to set bios_grub flag on BIOS Boot partition"));
            return false;
        }
    }

    if (config.dataFirst) {
        if (!addPartition(layout.dataSizeMiB, config.dataFs.isEmpty() ? QStringLiteral("fat32") : config.dataFs, QStringLiteral("data"))) {
            return false;
        }

        if (config.gpt) {
            const QString mark = QStringLiteral("msftdata");
            if (!runCommandShell(QStringLiteral("%1 set %2 %3 on").arg(preamble).arg(partNum).arg(mark), error)) {
                return false;
            }
        } else {
            QString mark;
            const QString fs = config.dataFs.toLower();
            if (fs == QLatin1String("vfat") || fs == QLatin1String("fat32")) {
                mark = QStringLiteral("c");
            } else if (fs == QLatin1String("exfat") || fs == QLatin1String("ntfs")) {
                mark = QStringLiteral("7");
            }
            if (!mark.isEmpty()) {
                runCommand(QStringLiteral("sfdisk"),
                           {layout.drive, QString::number(partNum), QStringLiteral("--part-type"), mark},
                           error, true);
            }
        }
    }

    if (config.encrypt) {
        if (!addPartition(layout.biosSizeMiB, QStringLiteral("ext4"), QStringLiteral("bios"))) {
            return false;
        }
        const QString bootFlag = config.gpt ? QStringLiteral("legacy_boot") : QStringLiteral("boot");
        // Boot flag is optional - log but don't fail if it can't be set
        if (!runCommandShell(QStringLiteral("%1 set %2 %3 on").arg(preamble).arg(partNum).arg(bootFlag), error, true)) {
            logLine(QStringLiteral("Warning: Could not set boot flag on bios partition"));
        }
    }

    if (!addPartition(layout.mainSizeMiB, config.encrypt ? QString() : QStringLiteral("ext4"), QStringLiteral("main"))) {
        return false;
    }
    if (!config.encrypt) {
        const QString bootFlag = config.gpt ? QStringLiteral("legacy_boot") : QStringLiteral("boot");
        // Boot flag is optional - log but don't fail if it can't be set
        if (!runCommandShell(QStringLiteral("%1 set %2 %3 on").arg(preamble).arg(partNum).arg(bootFlag), error, true)) {
            logLine(QStringLiteral("Warning: Could not set boot flag on main partition"));
        }
    }

    if (!addPartition(layout.uefiSizeMiB, QStringLiteral("fat32"), QStringLiteral("uefi"))) {
        return false;
    }
    // ESP flag is optional - log but don't fail if it can't be set
    if (!runCommandShell(QStringLiteral("%1 set %2 esp on").arg(preamble).arg(partNum), error, true)) {
        logLine(QStringLiteral("Warning: Could not set ESP flag on UEFI partition"));
    }

    if (config.gpt && config.pmbr) {
        // PMBR boot flag is optional - log but don't fail if it can't be set
        if (!runCommandShell(QStringLiteral("%1 disk_set pmbr_boot on").arg(preamble), error, true)) {
            logLine(QStringLiteral("Warning: Could not set pmbr_boot flag"));
        }
    }

    return runCommand(QStringLiteral("partprobe"), {layout.drive}, error, true);
}

bool LiveUsbMakerBackend::makeFileSystems(QString *error)
{
    logLine(QStringLiteral("Creating filesystems."));
    if (config.dataFirst && !layout.dataDev.isEmpty()) {
        if (!makeFs(layout.dataDev, config.dataFs, QStringLiteral("USB-DATA"), error)) {
            return false;
        }
    }

    if (config.encrypt && !layout.biosDev.isEmpty()) {
        QStringList args;
        if (config.forceMakefs) {
            args << QStringLiteral("-F");
        }
        args << QStringLiteral("-m0") << QStringLiteral("-i65536") << QStringLiteral("-J")
             << QStringLiteral("size=32") << layout.biosDev;
        if (!runCommand(QStringLiteral("mkfs.ext4"), args, error)) {
            return false;
        }
        runCommand(QStringLiteral("tune2fs"), {QStringLiteral("-L"), kBiosLabel.left(16), layout.biosDev}, error, true);
    }

    if (!layout.uefiDev.isEmpty()) {
        if (!runCommand(QStringLiteral("mkfs.vfat"),
                        {QStringLiteral("-F"), QStringLiteral("32"),
                         QStringLiteral("-n"), kEspLabel.toUpper(), layout.uefiDev},
                        error)) {
            return false;
        }
    }

    if (!config.encrypt && !layout.mainDev.isEmpty()) {
        QStringList args{QStringLiteral("-m0"), QStringLiteral("-i65536"), QStringLiteral("-J"), QStringLiteral("size=32")};
        if (config.forceMakefs) {
            args.prepend(QStringLiteral("-F"));
        }
        args.append(layout.mainDev);
        if (!runCommand(QStringLiteral("mkfs.ext4"), args, error)) {
            return false;
        }
        if (!config.label.isEmpty()) {
            runCommand(QStringLiteral("tune2fs"), {QStringLiteral("-L"), config.label.left(16), layout.mainDev}, error, true);
        }
    }
    return true;
}

bool LiveUsbMakerBackend::mountTargets(bool mountMain, QString *error)
{
    logLine(QStringLiteral("Mounting target partitions."));
    if (config.dataFirst && !layout.dataDev.isEmpty()) {
        if (!mountDevice(layout.dataDev, paths.dataDir, config.dataFs, error)) {
            return false;
        }
    }
    if (config.encrypt && !layout.biosDev.isEmpty()) {
        if (!mountDevice(layout.biosDev, paths.biosDir, kMainFsType, error)) {
            return false;
        }
    } else {
        paths.biosDir = paths.mainDir;
    }
    if (mountMain) {
        if (!mountDevice(layout.mainDev, paths.mainDir, kMainFsType, error)) {
            return false;
        }
    }
    if (!mountDevice(layout.uefiDev, paths.uefiDir, kUefiFsType, error)) {
        return false;
    }
    return true;
}

bool LiveUsbMakerBackend::unmountTargets()
{
    unmountPath(paths.dataDir, nullptr);
    if (config.encrypt) {
        unmountPath(paths.biosDir, nullptr);
    }
    unmountPath(paths.mainDir, nullptr);
    unmountPath(paths.uefiDir, nullptr);
    return true;
}

bool LiveUsbMakerBackend::copyMain(QString *error)
{
    logLine(QStringLiteral("Copying main files."));
    const QString antiXDir = QDir(paths.mainDir).filePath(QStringLiteral("antiX"));
    if (QDir(antiXDir).exists()) {
        runCommand(QStringLiteral("rm"), {QStringLiteral("-rf"), antiXDir}, error, true);
    }
    runCommand(QStringLiteral("rm"), {QStringLiteral("-rf"), QDir(paths.mainDir).filePath(QStringLiteral("boot.orig"))},
               error, true);

    if (config.saveBoot && QDir(QDir(paths.mainDir).filePath(QStringLiteral("boot"))).exists()) {
        runCommand(QStringLiteral("mv"),
                   {paths.mainDir + QStringLiteral("/boot"), paths.mainDir + QStringLiteral("/boot.orig")},
                   error, true);
    }

    if (config.sourceMode == LiveUsbMakerConfig::SourceMode::Iso) {
        if (!copyFileTree(paths.isoDir, paths.mainDir, error)) {
            return false;
        }
    } else {
        if (!copyFilesSpec(paths.isoDir, kCloneDirsSpec, paths.mainDir, error)) {
            return false;
        }
        if (!copyFilesSpec(paths.isoDir, kCloneFilesSpec, paths.mainDir, error)) {
            return false;
        }
        const QString bootRoot = QDir(paths.isoDir).filePath(kDefaultBootDir);
        const QString bootDest = QDir(paths.mainDir).filePath(kDefaultBootDir);
        if (!copyFilesSpec(bootRoot, kCloneLinuxfsSpec, bootDest, error)) {
            return false;
        }
        if (!copyFilesSpec(bootRoot, kCloneBootSpec, bootDest, error)) {
            return false;
        }
        if (config.clonePersist) {
            const QString rootfs = QDir(bootRoot).filePath(QStringLiteral("rootfs"));
            if (QFileInfo::exists(rootfs)) {
                const bool skipRoot = (config.sourceMode == LiveUsbMakerConfig::SourceMode::Clone
                                       && QFileInfo::exists(LivePaths::STATIC_ROOT));
                if (!skipRoot) {
                    runCommand(QStringLiteral("cp"), {QStringLiteral("-a"), rootfs, bootDest}, error, true);
                }
            }
            const QString homefs = QDir(bootRoot).filePath(QStringLiteral("homefs"));
            if (QFileInfo::exists(homefs)) {
                const bool skipHome = (config.sourceMode == LiveUsbMakerConfig::SourceMode::Clone
                                       && isMountpoint(QStringLiteral("/home")));
                if (!skipHome) {
                    runCommand(QStringLiteral("cp"), {QStringLiteral("-a"), homefs, bootDest}, error, true);
                }
            }
        }
    }

    if (config.saveBoot && QFileInfo::exists(paths.mainDir + QStringLiteral("/boot.orig"))) {
        runCommand(QStringLiteral("rm"), {QStringLiteral("-rf"), paths.mainDir + QStringLiteral("/boot")}, error, true);
        runCommand(QStringLiteral("mv"),
                   {paths.mainDir + QStringLiteral("/boot.orig"), paths.mainDir + QStringLiteral("/boot")},
                   error, true);
    }
    return true;
}

bool LiveUsbMakerBackend::copyBios(QString *error)
{
    logLine(QStringLiteral("Copying BIOS files."));
    if (config.sourceMode == LiveUsbMakerConfig::SourceMode::Iso) {
        if (config.encrypt || archIso) {
            const QString biosSpec = archIso ?
                QStringLiteral("[Ee][Ff][Ii] boot/syslinux boot/grub boot/memtest version") :
                kBiosFilesSpec;
            if (!copyFilesSpec(paths.isoDir, biosSpec, paths.biosDir, error)) {
                return false;
            }
        }
    } else {
        if (!copyFilesSpec(paths.isoDir, kCloneDirsSpec, paths.biosDir, error)) {
            return false;
        }
        if (!copyFilesSpec(paths.isoDir, kCloneFilesSpec, paths.biosDir, error)) {
            return false;
        }
        const QString bootRoot = QDir(paths.isoDir).filePath(kDefaultBootDir);
        const QString bootDest = QDir(paths.biosDir).filePath(kDefaultBootDir);
        if (!copyFilesSpec(bootRoot, kCloneBootSpec, bootDest, error)) {
            return false;
        }
    }
    return true;
}

bool LiveUsbMakerBackend::copyUefi(QString *error)
{
    logLine(QStringLiteral("Copying UEFI files."));
    if (config.sourceMode == LiveUsbMakerConfig::SourceMode::Iso) {
        if (!copyFilesSpec(paths.isoDir, kUefiFilesSpec, paths.uefiDir, error)) {
            return false;
        }
        if (!copyFilesSpec(paths.isoDir, kUefiFilesSpec2, paths.uefiDir, error)) {
            return false;
        }
        if (archIso) {
            if (!copyFilesSpec(paths.isoDir, kUefiFilesSpecArch, paths.uefiDir, error)) {
                return false;
            }
        }
    } else {
        if (!copyFilesSpec(paths.isoDir, QStringLiteral("[Ee][Ff][Ii] version"), paths.uefiDir, error)) {
            return false;
        }
    }
    return fixUefiMemtest(error);
}

bool LiveUsbMakerBackend::checkUsbMd5(QString *error)
{
    const QString checksumRoot = paths.mainDir;
    logLine(QStringLiteral("Checking checksum files if present."));
    QString output;
    // Only check checksums for critical boot/system files
    if (!runCommandOutput(QStringLiteral("find"),
                          {checksumRoot, QStringLiteral("-maxdepth"), QStringLiteral("4"),
                           QStringLiteral("("),
                           QStringLiteral("-name"), QStringLiteral("vmlinuz*.md5"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("vmlinuz*.sha512"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("initrd*.md5"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("initrd*.sha512"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("initramfs*.md5"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("initramfs*.sha512"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("airootfs*.md5"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("airootfs*.sha512"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("linuxfs*.md5"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("linuxfs*.sha512"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("squashfs*.md5"),
                           QStringLiteral("-o"), QStringLiteral("-name"), QStringLiteral("squashfs*.sha512"),
                           QStringLiteral(")")},
                          &output, error)) {
        return false;
    }
    const QStringList files = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (files.isEmpty()) {
        return true;
    }

    for (const QString &file : files) {
        const QString dir = QFileInfo(file).path();
        const QString name = QFileInfo(file).fileName();
        const bool isSha512 = name.endsWith(QStringLiteral(".sha512"));
        const QString sumCmd = isSha512 ? QStringLiteral("sha512sum") : QStringLiteral("md5sum");
        const QString label = isSha512 ? QStringLiteral("SHA512") : QStringLiteral("MD5");
        QString result;
        if (!runCommandOutput(QStringLiteral("bash"),
                              {QStringLiteral("-c"),
                               QStringLiteral("cd %1 && %2 -c %3").arg(dir, sumCmd, name)},
                              &result, error)) {
            logLine(QStringLiteral("%1 check failed for %2 in %3").arg(label, name, dir));
            logLine(QStringLiteral("Critical %1 failure for %2, continuing anyway").arg(label, name));
        } else {
            logLine(QStringLiteral("%1 check passed for %2").arg(label, name));
        }
    }
    return true;
}

bool LiveUsbMakerBackend::updateArchIsoBootConfig(QString *error) const
{
    logLine(QStringLiteral("Updating Arch ISO boot config"));
    const QString grubPath = QDir(paths.biosDir).filePath(kArchIsoGrubCfg);
    if (!QFileInfo::exists(grubPath)) {
        if (error) {
            *error = QStringLiteral("Missing archiso grub configuration at %1.").arg(grubPath);
        }
        return false;
    }

    QString mainUuid;
    if (!runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.mainDev}, &mainUuid, error)) {
        return false;
    }
    mainUuid = mainUuid.trimmed();
    if (mainUuid.isEmpty() || !ValidationUtils::isValidUuid(mainUuid)) {
        if (error) {
            *error = QStringLiteral("Invalid UUID for main partition: %1").arg(mainUuid);
        }
        return false;
    }

    QString mainLabel;
    if (runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("LABEL"), layout.mainDev}, &mainLabel, nullptr)) {
        mainLabel = mainLabel.trimmed();
    } else {
        mainLabel.clear();
    }

    // For Arch ISOs, if no label, extract from ISO's GRUB config
    if (mainLabel.isEmpty()) {
        const QString isoGrubPath = QDir(paths.isoDir).filePath(kArchIsoGrubCfg);
        if (QFileInfo::exists(isoGrubPath)) {
            QFile isoGrubFile(isoGrubPath);
            if (isoGrubFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QString isoGrubContent = QString::fromUtf8(isoGrubFile.readAll());
                isoGrubFile.close();

                // Try to extract label from archisolabel= kernel parameter
                const QRegularExpression archisoLabelRe(QStringLiteral("\\barchisolabel=([^\\s]+)"));
                QRegularExpressionMatch match = archisoLabelRe.match(isoGrubContent);
                if (match.hasMatch()) {
                    const QString isoLabel = match.captured(1);
                    if (!isoLabel.isEmpty()) {
                        // Set the label on the main partition
                        mainLabel = isoLabel.left(16);
                        runCommand(QStringLiteral("tune2fs"), {QStringLiteral("-L"), mainLabel, layout.mainDev}, nullptr, true);
                    }
                }

                // If not found, try to extract from search --label command
                if (mainLabel.isEmpty()) {
                    const QRegularExpression searchLabelRe(QStringLiteral("search\\s+[^\\n]*--label\\s+[\"']?([^\"'\\s]+)[\"']?"));
                    match = searchLabelRe.match(isoGrubContent);
                    if (match.hasMatch()) {
                        const QString isoLabel = match.captured(1);
                        if (!isoLabel.isEmpty()) {
                            // Set the label on the main partition
                            mainLabel = isoLabel.left(16);
                            runCommand(QStringLiteral("tune2fs"), {QStringLiteral("-L"), mainLabel, layout.mainDev}, nullptr, true);
                            logLine(QStringLiteral("Set partition label to: %1").arg(mainLabel));
                        }
                    }
                }
            }
        }
    }

    QString dataUuid;
    if (config.dataFirst && !layout.dataDev.isEmpty()) {
        if (!runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.dataDev}, &dataUuid, error)) {
            return false;
        }
        dataUuid = dataUuid.trimmed();
        if (dataUuid.isEmpty() || !ValidationUtils::isValidUuid(dataUuid)) {
            if (error) {
                *error = QStringLiteral("Invalid UUID for persistence partition: %1").arg(dataUuid);
            }
            return false;
        }
    }

    QFile grubFile(grubPath);
    if (!grubFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Could not read grub configuration.");
        }
        return false;
    }
    QString grubContent = QString::fromUtf8(grubFile.readAll());
    grubFile.close();

    const QStringList lines = grubContent.split(QLatin1Char('\n'));
    QStringList updated;
    updated.reserve(lines.size());
    bool changed = false;
    const QString archisodeviceArg = QStringLiteral("archisodevice=UUID=%1").arg(mainUuid);
    const QString cowDeviceArg = dataUuid.isEmpty() ? QString() : QStringLiteral("cow_device=UUID=%1").arg(dataUuid);
    const QString archisolabelArg = mainLabel.isEmpty() ? QString() : QStringLiteral("archisolabel=%1").arg(mainLabel);
    const QRegularExpression archisoLabelRe(QStringLiteral("\\barchisolabel=[^\\s]+"));
    const QDir archBootX8664Dir(QDir(paths.mainDir).filePath(QStringLiteral("arch/boot/x86_64")));
    const QDir bootX8664Dir(QDir(paths.mainDir).filePath(QStringLiteral("boot/x86_64")));
    const QStringList bootCandidates{QStringLiteral("vmlinuz*"), QStringLiteral("initrd*"), QStringLiteral("archiso.img")};
    QString bootPrefix;
    if (archBootX8664Dir.exists() && !archBootX8664Dir.entryList(bootCandidates, QDir::Files).isEmpty()) {
        bootPrefix = QStringLiteral("/arch/boot/");
    } else if (bootX8664Dir.exists() && !bootX8664Dir.entryList(bootCandidates, QDir::Files).isEmpty()) {
        bootPrefix = QStringLiteral("/boot/");
    }
    const QRegularExpression archBootPathRe(QStringLiteral("/arch/boot/"));
    const QRegularExpression bootPathRe(QStringLiteral("/boot/"));

    for (const QString &line : lines) {
        QString updatedLine = line;
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(QStringLiteral("linux ")) || trimmed.startsWith(QStringLiteral("linuxefi "))) {
            // Update kernel path to match the detected boot files location.
            if (!bootPrefix.isEmpty()) {
                if (bootPrefix == QLatin1String("/arch/boot/") && !updatedLine.contains(QLatin1String("/arch/boot/"))) {
                    updatedLine.replace(bootPathRe, bootPrefix);
                } else if (bootPrefix == QLatin1String("/boot/") && !updatedLine.contains(QLatin1String("/boot/"))) {
                    updatedLine.replace(archBootPathRe, bootPrefix);
                }
            }
            if (!updatedLine.contains(QStringLiteral("archisodevice="))) {
                updatedLine += QLatin1Char(' ') + archisodeviceArg;
                changed = true;
            }
            if (mainLabel.isEmpty()) {
                if (updatedLine.contains(archisoLabelRe)) {
                    updatedLine.remove(archisoLabelRe);
                    changed = true;
                }
            } else if (updatedLine.contains(archisoLabelRe)) {
                updatedLine.replace(archisoLabelRe, archisolabelArg);
                if (updatedLine != line) {
                    changed = true;
                }
            } else if (!updatedLine.contains(QStringLiteral("archisolabel="))) {
                updatedLine += QLatin1Char(' ') + archisolabelArg;
                changed = true;
            }
            if (!cowDeviceArg.isEmpty() && !updatedLine.contains(QStringLiteral("cow_device="))) {
                updatedLine += QLatin1Char(' ') + cowDeviceArg;
                changed = true;
            }
        } else if (trimmed.startsWith(QStringLiteral("initrd ")) || trimmed.startsWith(QStringLiteral("initrdefi "))) {
            // Update initrd path to match the detected boot files location.
            if (!bootPrefix.isEmpty()) {
                if (bootPrefix == QLatin1String("/arch/boot/") && !updatedLine.contains(QLatin1String("/arch/boot/"))) {
                    updatedLine.replace(bootPathRe, bootPrefix);
                } else if (bootPrefix == QLatin1String("/boot/") && !updatedLine.contains(QLatin1String("/boot/"))) {
                    updatedLine.replace(archBootPathRe, bootPrefix);
                }
                if (updatedLine != line) {
                    changed = true;
                }
            }
        } else if (trimmed.startsWith(QStringLiteral("search "))) {
            // Update search to use UUID instead of label
            if (!trimmed.contains(QStringLiteral("--fs-uuid"))) {
                updatedLine.replace(QRegularExpression(QStringLiteral("--label\\s+\"[^\"]*\"|--label\\s+[^\\s]+")),
                                    QStringLiteral("--fs-uuid ") + mainUuid);
                if (updatedLine != line) {
                    changed = true;
                }
            }
        }
        updated.append(updatedLine);
    }

    if (!changed) {
        return true;
    }

    if (!grubFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Could not update grub configuration.");
        }
        return false;
    }
    grubFile.write(updated.join(QLatin1Char('\n')).toUtf8());
    grubFile.close();
    return true;
}

bool LiveUsbMakerBackend::installArchIsoBootloader(QString *error) const
{
    logLine(QStringLiteral("Installing archiso bootloader."));
    const QString bootDir = QDir(paths.biosDir).filePath(QStringLiteral("boot"));
    if (!QDir(bootDir).exists()) {
        if (error) {
            *error = QStringLiteral("Missing /boot directory for grub install.");
        }
        return false;
    }
    return runCommand(QStringLiteral("grub-install"),
                      {QStringLiteral("--target=i386-pc"),
                       QStringLiteral("--recheck"),
                       QStringLiteral("--boot-directory=%1").arg(bootDir),
                       layout.drive},
                      error);
}

bool LiveUsbMakerBackend::setupArchLiveUsbStorage()
{
    logLine(QStringLiteral("Setting up live-usb-storage for Arch ISO."));
    const QString scriptDir = QStringLiteral("/usr/lib/mx-live-usb-maker/arch-live-usb-storage");
    const QString script = QDir(scriptDir).filePath(QStringLiteral("inject-live-usb-storage.sh"));
    if (!QFileInfo::exists(script)) {
        logWarn(QStringLiteral("inject-live-usb-storage.sh not found at %1").arg(script));
        return false;
    }
    return runCommandShell(QStringLiteral("%1 %2")
                               .arg(ShellUtils::quote(script), ShellUtils::quote(paths.mainDir)),
                           nullptr);
}

bool LiveUsbMakerBackend::installBootloader(QString *error)
{
    logLine(QStringLiteral("Installing bootloader."));
    const QString type = config.gpt ? QStringLiteral("gpt") : QStringLiteral("msdos");
    QString mbrFileName = type == QLatin1String("gpt") ? QStringLiteral("gptmbr.bin") : QStringLiteral("mbr.bin");
    if (!config.gpt && config.dataFirst) {
        mbrFileName = QStringLiteral("altmbr.bin");
    }
    const QString mbrFile = findSyslinuxMbr(mbrFileName, error);
    if (mbrFile.isEmpty()) {
        return false;
    }
    if (mbrFileName == QLatin1String("altmbr.bin")) {
        const QString cmd = QStringLiteral("printf '\\2' | cat %1 - | dd bs=%3 count=1 iflag=fullblock conv=notrunc of=%2")
                                .arg(ShellUtils::quote(mbrFile), ShellUtils::quote(layout.drive))
                                .arg(MBR_BOOT_CODE_SIZE_BYTES);
        if (!runCommandShell(cmd, error)) {
            return false;
        }
    } else {
        if (!runCommand(QStringLiteral("dd"),
                        {QStringLiteral("bs=%1").arg(MBR_BOOT_CODE_SIZE_BYTES), QStringLiteral("conv=notrunc"), QStringLiteral("count=1"),
                         QStringLiteral("if=%1").arg(mbrFile), QStringLiteral("of=%1").arg(layout.drive)},
                        error)) {
            return false;
        }
    }

    const QString syslinuxDir = findSyslinuxDir(paths.biosDir);
    if (syslinuxDir.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Could not find syslinux directory.");
        }
        return false;
    }
    QString finalSyslinuxDir = syslinuxDir;
    if (finalSyslinuxDir.contains(QLatin1String("isolinux"))) {
        finalSyslinuxDir = finalSyslinuxDir;
        finalSyslinuxDir.replace(QStringLiteral("isolinux"), QStringLiteral("syslinux"));
        runCommand(QStringLiteral("cp"), {QStringLiteral("-a"), syslinuxDir, finalSyslinuxDir}, error, true);
        runCommand(QStringLiteral("bash"),
                   {QStringLiteral("-c"),
                    QStringLiteral("cd %1 && for f in isolinux.*; do [ -e \"$f\" ] && mv \"$f\" syslinux${f#isolinux}; done")
                        .arg(finalSyslinuxDir)},
                   error, true);
    }
    if (!config.keepSyslinux) {
        const QString moduleDir = findSyslinuxModuleDir(error);
        if (moduleDir.isEmpty()) {
            return false;
        }
        runCommand(QStringLiteral("bash"),
                   {QStringLiteral("-c"),
                    QStringLiteral("cd %1 && rm -f %2").arg(finalSyslinuxDir, kSyslinuxRemove)},
                   error, true);
        const QStringList files = kSyslinuxFiles.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString &file : files) {
            runCommand(QStringLiteral("cp"), {QDir(moduleDir).filePath(file), finalSyslinuxDir}, error, true);
        }
        QString version;
        if (runCommandOutput(QStringLiteral("extlinux"), {QStringLiteral("--version")}, &version, nullptr)) {
            const QString trimmed = version.trimmed();
            if (!trimmed.isEmpty()) {
                runCommand(QStringLiteral("bash"),
                           {QStringLiteral("-c"),
                            QStringLiteral("echo \"%1\" > \"%2/version\"").arg(trimmed, finalSyslinuxDir)},
                           error, true);
            }
        }
    }
    return runCommand(QStringLiteral("extlinux"), {QStringLiteral("-i"), finalSyslinuxDir}, error);
}

bool LiveUsbMakerBackend::updateUuids(QString *error)
{
    logLine(QStringLiteral("Updating UUID references."));
    QString biosUuid;
    if (!config.encrypt) {
        if (!runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.mainDev}, &biosUuid, error)) {
            return false;
        }
        biosUuid = biosUuid.trimmed();
        if (!biosUuid.isEmpty() && !ValidationUtils::isValidUuid(biosUuid)) {
            if (error) {
                *error = QStringLiteral("Invalid UUID format from lsblk for %1: %2").arg(layout.mainDev, biosUuid);
            }
            return false;
        }
    } else {
        if (!runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.biosDev}, &biosUuid, error)) {
            return false;
        }
        biosUuid = biosUuid.trimmed();
        if (!biosUuid.isEmpty() && !ValidationUtils::isValidUuid(biosUuid)) {
            if (error) {
                *error = QStringLiteral("Invalid UUID format from lsblk for %1: %2").arg(layout.biosDev, biosUuid);
            }
            return false;
        }
    }
    QString uefiUuid;
    if (!runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.uefiDev}, &uefiUuid, error)) {
        return false;
    }
    uefiUuid = uefiUuid.trimmed();
    // Note: UEFI partition UUID is not used in UUID updates, so no validation needed

    if (biosUuid.isEmpty()) {
        return true;
    }

    if (config.encrypt) {
        const QString biosUuidFile = QDir(paths.mainDir).filePath(QStringLiteral("antiX/bios-dev-uuid"));
        QDir().mkpath(QFileInfo(biosUuidFile).path());
        QFile f(biosUuidFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write(biosUuid.toUtf8());
            f.write("\n");
        }
    }

    const QString efiGrubConf = QStringLiteral("boot/grub/grub.cfg");
    const QString efiConf2 = QStringLiteral("boot/grub/config/efi-grub.cfg");
    const QString didEfiFile = QStringLiteral("boot/grub/config/did-efi-grub");

    const QString biosEfiConf = QDir(paths.biosDir).filePath(efiConf2);
    const QString uefiGrubPath = QDir(paths.uefiDir).filePath(efiGrubConf);

    if (QFileInfo::exists(biosEfiConf)) {
        QDir().mkpath(QFileInfo(uefiGrubPath).path());
        runCommand(QStringLiteral("cp"), {biosEfiConf, uefiGrubPath}, error);
        runCommand(QStringLiteral("touch"), {QDir(paths.biosDir).filePath(didEfiFile)}, error, true);
        runCommand(QStringLiteral("sed"),
                   {QStringLiteral("-i"), QStringLiteral("/^\\s*#/! s/%UUID%/%1/").arg(biosUuid), uefiGrubPath},
                   error, true);
        if (runCommand(QStringLiteral("grep"), {QStringLiteral("-q"), QStringLiteral("^[^#]*%ID_FILE%"), uefiGrubPath}, error, true)) {
            // Remove old .id files using QDir (safer than shell wildcards)
            const QDir configDir(QDir(paths.biosDir).filePath(QStringLiteral("boot/grub/config")));
            const QFileInfoList idFiles = configDir.entryInfoList(QStringList() << QStringLiteral("*.id"), QDir::Files);
            for (const QFileInfo &idFile : idFiles) {
                QFile::remove(idFile.absoluteFilePath());
            }
            const QString idFile = QStringLiteral("/boot/grub/config/%1.id").arg(randomString(8));
            runCommand(QStringLiteral("touch"), {QDir(paths.biosDir).filePath(idFile)}, error, true);
            runCommand(QStringLiteral("sed"),
                       {QStringLiteral("-i"),
                        QStringLiteral("/^\\s*#/! s|%ID_FILE%|%1|").arg(idFile),
                        uefiGrubPath},
                       error, true);
        }
        return true;
    }

    const QString searchLine = QStringLiteral("search --no-floppy --set=root --fs-uuid %1").arg(biosUuid);
    QString grubContent;
    QFile grubFile(uefiGrubPath);
    if (!grubFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return true;
    }
    grubContent = QString::fromUtf8(grubFile.readAll());
    grubFile.close();

    static const QRegularExpression searchRe(QStringLiteral("search.*--set=root.*"));
    if (searchRe.match(grubContent).hasMatch()) {
        grubContent.replace(searchRe, searchLine);
    } else {
        static const QRegularExpression menuEntryRe(QStringLiteral("(^\\s*menuentry)"), QRegularExpression::MultilineOption);
        grubContent.replace(menuEntryRe, searchLine + QStringLiteral("\n\n\\1"));
    }
    static const QRegularExpression espCommentRe(QStringLiteral("^#-+esp\\s*"), QRegularExpression::MultilineOption);
    static const QRegularExpression rootPartRe(QStringLiteral("(root=\\(hd0,)[0-9]\\)"));
    grubContent.replace(espCommentRe, QString());
    grubContent.replace(rootPartRe, QStringLiteral("\\1%1)").arg(layout.uefiPart));

    if (grubFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        grubFile.write(grubContent.toUtf8());
        grubFile.close();
    }

    if (!uefiUuid.isEmpty()) {
        const QStringList dirs{paths.biosDir, paths.mainDir};
        for (const QString &dir : dirs) {
            if (dir.isEmpty()) {
                continue;
            }
            const QString espFile = QDir(dir).filePath(QStringLiteral("antiX/esp-uuid"));
            QDir().mkpath(QFileInfo(espFile).path());
            QFile f(espFile);
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                f.write(uefiUuid.toUtf8());
                f.write("\n");
            }
        }
    }
    return true;
}

bool LiveUsbMakerBackend::writeDataUuid(QString *error)
{
    if (!config.dataFirst || layout.dataDev.isEmpty()) {
        return true;
    }
    QString uuid;
    runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.dataDev}, &uuid, error);
    uuid = uuid.trimmed();
    if (uuid.isEmpty()) {
        return true;
    }
    if (!ValidationUtils::isValidUuid(uuid)) {
        if (error) {
            *error = QStringLiteral("Invalid UUID format from lsblk for %1: %2").arg(layout.dataDev, uuid);
        }
        return false;
    }
    const QStringList dirs{paths.biosDir, paths.mainDir};
    for (const QString &dir : dirs) {
        if (dir.isEmpty()) {
            continue;
        }
        const QString uuidFile = QDir(dir).filePath(QStringLiteral("antiX/data-uuid"));
        QDir().mkpath(QFileInfo(uuidFile).path());
        QFile f(uuidFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write(uuid.toUtf8());
            f.write("\n");
        }
    }
    return true;
}

bool LiveUsbMakerBackend::fixUefiMemtest(QString *error)
{
    QString dir = QDir(paths.uefiDir).filePath(QStringLiteral("EFI/BOOT"));
    if (!QDir(dir).exists()) {
        dir = QDir(paths.uefiDir).filePath(QStringLiteral("efi/boot"));
    }
    if (!QDir(dir).exists()) {
        return true;
    }
    const QString grubx64 = QDir(dir).filePath(QStringLiteral("grubx64.efi"));
    if (!QFileInfo::exists(grubx64)) {
        return true;
    }
    const QString fallback = QDir(dir).filePath(QStringLiteral("fallback.efi"));
    if (QFileInfo::exists(fallback)) {
        const QString tempDir = QDir(QFileInfo(dir).path()).filePath(QStringLiteral("tmp-efi"));
        QDir().mkpath(tempDir);
        runCommand(QStringLiteral("cp"), {QStringLiteral("-a"), dir + QStringLiteral("/*"), tempDir}, error, true);
        runCommand(QStringLiteral("rm"), {QStringLiteral("-rf"), dir}, error, true);
        runCommand(QStringLiteral("mv"), {tempDir, dir}, error, true);
    }
    return runCommand(QStringLiteral("cp"), {grubx64, fallback}, error, true);
}

bool LiveUsbMakerBackend::prepareInitrdEncryption(QString *error)
{
    logLine(QStringLiteral("Preparing initrd for encryption."));
    const QString initrdPath = QDir(paths.isoDir).filePath(kDefaultBootDir + QStringLiteral("/initrd.gz"));
    const QString linuxfsPath = QDir(paths.isoDir).filePath(kDefaultBootDir + QStringLiteral("/") + kLinuxfsName);
    if (!runCommand(QStringLiteral("mount"),
                    {QStringLiteral("-t"), QStringLiteral("squashfs"), QStringLiteral("-o"),
                     QStringLiteral("loop,ro"), linuxfsPath, paths.linuxDir},
                    error)) {
        return false;
    }

    // Ensure unmount happens even on error paths
    bool success = true;
    if (!unpackInitrd(initrdPath, paths.initrdDir, error)) {
        success = false;
    } else if (!copyInitrdPrograms(paths.linuxDir, paths.initrdDir, error)) {
        success = false;
    } else if (!copyInitrdModules(paths.linuxDir, paths.initrdDir, error)) {
        success = false;
    } else {
        const QString encryptFile = QDir(paths.initrdDir).filePath(QStringLiteral("etc/encrypt"));
        QDir().mkpath(QFileInfo(encryptFile).path());
        QFile f(encryptFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            f.write(kEncryptEnable.toUtf8());
        }
    }

    // Always unmount before returning
    unmountPath(paths.linuxDir, nullptr);
    return success;
}

bool LiveUsbMakerBackend::finalizeInitrdEncryption(QString *error)
{
    logLine(QStringLiteral("Repacking initrd."));
    const QString initrdPath = QDir(paths.biosDir).filePath(kDefaultBootDir + QStringLiteral("/initrd.gz"));
    return repackInitrd(initrdPath, paths.initrdDir, error);
}

bool LiveUsbMakerBackend::encryptMainPartition(QString *error)
{
    logLine(QStringLiteral("Encrypting main partition."));
    const QString phraseFile = QDir(paths.biosDir).filePath(kDefaultBootDir + QStringLiteral("/passphrase"));
    const QString encryptFile = QDir(paths.biosDir).filePath(kDefaultBootDir + QStringLiteral("/encrypted"));
    QDir().mkpath(QFileInfo(phraseFile).path());

    if (!writePassphraseFile(phraseFile, error)) {
        return false;
    }
    if (!runCommand(QStringLiteral("cryptsetup"),
                    {QStringLiteral("luksFormat"), QStringLiteral("--type"), QStringLiteral("luks2"),
                     QStringLiteral("--key-file"), phraseFile, layout.mainDev},
                    error)) {
        return false;
    }
    if (!runCommand(QStringLiteral("cryptsetup"),
                    {QStringLiteral("open"), QStringLiteral("--type"), QStringLiteral("luks2"),
                     QStringLiteral("--key-file"), phraseFile, layout.mainDev, kLuksName},
                    error)) {
        return false;
    }
    const QString luksDev = SystemPaths::DEV_MAPPER + QStringLiteral("/") + kLuksName;
    if (!runCommand(QStringLiteral("mkfs.ext4"),
                    {QStringLiteral("-m0"), QStringLiteral("-i16384"), QStringLiteral("-J"), QStringLiteral("size=32"),
                     luksDev},
                    error)) {
        return false;
    }
    QString mainUuid;
    runCommandOutput(QStringLiteral("lsblk"), {QStringLiteral("-no"), QStringLiteral("UUID"), layout.mainDev}, &mainUuid, error);
    mainUuid = mainUuid.trimmed();
    if (!mainUuid.isEmpty() && !ValidationUtils::isValidUuid(mainUuid)) {
        if (error) {
            *error = QStringLiteral("Invalid UUID format from lsblk for %1: %2").arg(layout.mainDev, mainUuid);
        }
        return false;
    }
    QFile ef(encryptFile);
    if (ef.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        ef.write(mainUuid.toUtf8());
        ef.write("\n");
    }
    unmountPath(paths.mainDir, nullptr);
    if (!mountDevice(luksDev, paths.mainDir, kMainFsType, error)) {
        return false;
    }
    return true;
}

QString LiveUsbMakerBackend::logPath() const
{
    return AppPaths::LOG_FILE;
}

bool LiveUsbMakerBackend::runCommand(const QString &program, const QStringList &args, QString *error, bool allowFail) const
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(env);
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(-1)) {
        if (error) {
            *error = QStringLiteral("Command failed to finish: %1").arg(program);
        }
        return false;
    }
    const QString output = QString::fromUtf8(process.readAll());
    if (!output.isEmpty()) {
        logLine(output.trimmed());
    }
    if (allowFail) {
        return true;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("Command failed: %1").arg(program);
        }
        return false;
    }
    return true;
}

bool LiveUsbMakerBackend::runCommandShell(const QString &command, QString *error, bool allowFail) const
{
    return runCommand(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), command}, error, allowFail);
}

bool LiveUsbMakerBackend::runCommandOutput(const QString &program, const QStringList &args, QString *output, QString *error) const
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(env);
    process.setProgram(program);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(-1)) {
        if (error) {
            *error = QStringLiteral("Command failed to finish: %1").arg(program);
        }
        return false;
    }
    if (output) {
        *output = QString::fromUtf8(process.readAll());
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("Command failed: %1").arg(program);
        }
        return false;
    }
    return true;
}

void LiveUsbMakerBackend::logLine(const QString &line) const
{
    QFile file(logPath());
    if (file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << " " << line << "\n";
    }
    QTextStream stdoutStream(stdout);
    stdoutStream << line << "\n";
    stdoutStream.flush();
}

void LiveUsbMakerBackend::logWarn(const QString &line) const
{
    logLine(QStringLiteral("WARN: %1").arg(line));
}

void LiveUsbMakerBackend::logError(const QString &line) const
{
    logLine(QStringLiteral("ERROR: %1").arg(line));
}

QString LiveUsbMakerBackend::devicePath(const QString &device) const
{
    return DeviceUtils::normalizePath(device);
}

QString LiveUsbMakerBackend::partitionPath(const QString &drive, int index) const
{
    if (index <= 0) {
        return {};
    }
    const QString name = QFileInfo(drive).fileName();
    if (name.startsWith(QStringLiteral("mmcblk")) || name.startsWith(QStringLiteral("nvme"))) {
        return drive + QStringLiteral("p") + QString::number(index);
    }
    return drive + QString::number(index);
}

int LiveUsbMakerBackend::totalSizeMiB(QString *error) const
{
    QString output;
    if (!runCommandOutput(QStringLiteral("parted"),
                          {QStringLiteral("--script"), layout.drive, QStringLiteral("unit"), QStringLiteral("B"), QStringLiteral("print")},
                          &output, error)) {
        return 0;
    }
    static const QRegularExpression re(QStringLiteral("^Disk.*: ([0-9]+)B$"));
    const QStringList lines = output.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = re.match(line.trimmed());
        if (match.hasMatch()) {
            return match.captured(1).toLongLong() / 1024 / 1024;
        }
    }
    if (error) {
        *error = QStringLiteral("Unable to determine device size.");
    }
    return 0;
}

int LiveUsbMakerBackend::duApparentSizeMiB(const QString &dir, const QString &spec, QString *error) const
{
    QString output;
    const QString cmd = QStringLiteral("cd %1 && du --apparent-size -scm %2 2>/dev/null | tail -n 1 | cut -f1 || true")
                            .arg(dir, spec);
    if (!runCommandOutput(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), cmd}, &output, error)) {
        return 0;
    }
    bool ok = false;
    const int size = output.trimmed().toInt(&ok);
    return ok ? size : 0;
}

int LiveUsbMakerBackend::extOverheadMiB(int sizeMiB) const
{
    int overhead = (sizeMiB * kExtOverheadScale / kExtOverheadDivisor) - kExtOverheadOffset;
    if (overhead < 0) {
        overhead = 0;
    }
    if (overhead > kExtOverheadMaxMiB) {
        overhead = kExtOverheadMaxMiB;
    }
    return overhead;
}

QString LiveUsbMakerBackend::findSyslinuxMbr(const QString &name, QString *error) const
{
    const QStringList dirs{SyslinuxPaths::SHARE, SyslinuxPaths::MBR, SyslinuxPaths::BIOS};
    for (const QString &dir : dirs) {
        const QString candidate = QDir(dir).filePath(name);
        if (QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
    if (error) {
        *error = QStringLiteral("Could not find %1").arg(name);
    }
    return {};
}

QString LiveUsbMakerBackend::findSyslinuxModuleDir(QString *error) const
{
    const QStringList dirs{SyslinuxPaths::SHARE, SyslinuxPaths::LIB,
                           SyslinuxPaths::MODULES_BIOS, SyslinuxPaths::BIOS};
    for (const QString &dir : dirs) {
        if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("gfxboot.c32")))) {
            return dir;
        }
    }
    if (error) {
        *error = QStringLiteral("Could not find syslinux modules directory.");
    }
    return {};
}

QString LiveUsbMakerBackend::findSyslinuxDir(const QString &biosDir) const
{
    const QStringList candidates{QStringLiteral("boot/syslinux"), QStringLiteral("syslinux")};
    for (const QString &candidate : candidates) {
        const QString dir = QDir(biosDir).filePath(candidate);
        if (QFileInfo::exists(dir)) {
            return dir;
        }
    }
    const QStringList isoCandidates{QStringLiteral("boot/isolinux"), QStringLiteral("isolinux")};
    for (const QString &candidate : isoCandidates) {
        const QString dir = QDir(biosDir).filePath(candidate);
        if (QFileInfo::exists(dir)) {
            return dir;
        }
    }
    return {};
}

QString LiveUsbMakerBackend::randomString(int bytes) const
{
    QProcess process;
    process.setProgram(QStringLiteral("dd"));
    process.setArguments({QStringLiteral("if=/dev/urandom"), QStringLiteral("bs=1"),
                          QStringLiteral("count=%1").arg(bytes), QStringLiteral("status=none")});
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    process.waitForFinished(-1);
    const QByteArray data = process.readAll();
    QString hex;
    for (unsigned char c : data) {
        hex += QStringLiteral("%1").arg(c, 2, 16, QLatin1Char('0'));
    }
    return hex.left(bytes * 2);
}

bool LiveUsbMakerBackend::copyFileTree(const QString &source, const QString &destination, QString *error) const
{
    const QString cmd = QStringLiteral("cp -a %1/. %2/")
                            .arg(ShellUtils::quote(source), ShellUtils::quote(destination));
    return runCommandShell(cmd, error);
}

bool LiveUsbMakerBackend::copyFilesSpec(const QString &source, const QString &spec, const QString &destination, QString *error) const
{
    const QString cmd = QStringLiteral("cd %1 && ls -d %2 2>/dev/null || true")
                            .arg(ShellUtils::quote(source), spec);  // Note: spec is a glob pattern, not user input
    QString output;
    if (!runCommandOutput(QStringLiteral("/bin/bash"), {QStringLiteral("-c"), cmd}, &output, error)) {
        return false;
    }
    const QStringList files = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &file : files) {
        const QString destDir = QDir(destination).filePath(QFileInfo(file).path());
        QDir().mkpath(destDir);
        if (!runCommand(QStringLiteral("cp"),
                        {QStringLiteral("--no-dereference"), QStringLiteral("--preserve=mode,links"),
                         QStringLiteral("--recursive"),
                         QDir(source).filePath(file),
                         destDir},
                        error, true)) {
            logWarn(QStringLiteral("Error copying %1").arg(file));
        }
    }
    return true;
}

bool LiveUsbMakerBackend::makeFs(const QString &device, const QString &type, const QString &label, QString *error) const
{
    QString fs = type;
    if (fs == QLatin1String("fat32")) {
        fs = QStringLiteral("vfat");
    }
    QStringList args;
    if (fs == QLatin1String("exfat")) {
        args << QStringLiteral("-n") << label;
    } else if (fs == QLatin1String("vfat")) {
        args << QStringLiteral("-F") << QStringLiteral("32") << QStringLiteral("-n") << label;
    } else if (fs.startsWith(QLatin1String("ext"))) {
        args << QStringLiteral("-L") << label;
    } else if (fs == QLatin1String("ntfs")) {
        args << QStringLiteral("--fast") << QStringLiteral("-L") << label;
    }
    args << device;
    if (!runCommand(QStringLiteral("mkfs.") + fs, args, error)) {
        return false;
    }
    runCommand(QStringLiteral("partprobe"), {device}, error, true);
    if (fs == QLatin1String("vfat") || fs == QLatin1String("ntfs") || fs == QLatin1String("exfat")) {
        return true;
    }

    QString user = QString::fromUtf8(qgetenv("SUDO_USER"));
    if (user.isEmpty() || user == QLatin1String("root")) {
        QString temp;
        runCommandOutput(QStringLiteral("logname"), {}, &temp, error);
        user = temp.trimmed();
    }
    if (user.isEmpty() || user == QLatin1String("root")) {
        return true;
    }
    QString group;
    if (runCommandOutput(QStringLiteral("getent"), {QStringLiteral("group"), QStringLiteral("users")}, &group, error)) {
        group = group.section(QLatin1Char(':'), 0, 0).trimmed();
    }
    if (group.isEmpty()) {
        QString userGroup;
        if (runCommandOutput(QStringLiteral("getent"), {QStringLiteral("group"), user}, &userGroup, error)) {
            group = userGroup.section(QLatin1Char(':'), 0, 0).trimmed();
        }
    }
    if (!group.isEmpty()) {
        user = user + QStringLiteral(":") + group;
    }
    const QString mountPoint = paths.dataDir;
    if (!mountDevice(device, mountPoint, fs, error)) {
        return true;
    }
    runCommand(QStringLiteral("setfacl"), {QStringLiteral("-m"), QStringLiteral("u::rwx,g::rwx,o::r-x"), mountPoint + QStringLiteral("/.")},
               error, true);
    runCommand(QStringLiteral("setfacl"),
               {QStringLiteral("-d"), QStringLiteral("-m"), QStringLiteral("u::rwx,g::rwx,o::r-x"), mountPoint + QStringLiteral("/.")},
               error, true);
    runCommand(QStringLiteral("chown"), {user, mountPoint + QStringLiteral("/.")}, error, true);
    runCommand(QStringLiteral("chmod"), {QStringLiteral("g+rwx"), mountPoint + QStringLiteral("/.")}, error, true);
    unmountPath(mountPoint, error);
    return true;
}

bool LiveUsbMakerBackend::mountDevice(const QString &device, const QString &mountPoint, const QString &fsType, QString *error) const
{
    QDir().mkpath(mountPoint);
    QStringList args;
    if (!fsType.isEmpty()) {
        args << QStringLiteral("-t") << fsType;
    }
    args << device << mountPoint;
    return runCommand(QStringLiteral("mount"), args, error);
}

bool LiveUsbMakerBackend::unmountPath(const QString &mountPoint, QString *error) const
{
    if (!QFileInfo::exists(mountPoint)) {
        return true;
    }
    return runCommandShell(QStringLiteral("umount -l ") + mountPoint + QStringLiteral(" 2>/dev/null"), error, true);
}

bool LiveUsbMakerBackend::isMountpoint(const QString &path) const
{
    QProcess process;
    process.setProgram(QStringLiteral("mountpoint"));
    process.setArguments({QStringLiteral("-q"), path});
    process.start();
    process.waitForFinished(-1);
    return (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0);
}

bool LiveUsbMakerBackend::unpackInitrd(const QString &initrdPath, const QString &destDir, QString *error) const
{
    QDir().mkpath(destDir);
    const QString cmd = QStringLiteral("cd %1 && gzip -dc %2 | cpio -id --no-absolute-filenames")
                            .arg(ShellUtils::quote(destDir), ShellUtils::quote(initrdPath));
    return runCommandShell(cmd, error);
}

bool LiveUsbMakerBackend::repackInitrd(const QString &initrdPath, const QString &srcDir, QString *error) const
{
    const QString cmd = QStringLiteral("cd %1 && find . -print0 | cpio --null -ov --format=newc | gzip -c > %2")
                            .arg(ShellUtils::quote(srcDir), ShellUtils::quote(initrdPath));
    if (!runCommandShell(cmd, error)) {
        return false;
    }
    const QString initrdDir = ShellUtils::quote(QFileInfo(initrdPath).path());
    const QString initrdFile = ShellUtils::quote(QFileInfo(initrdPath).fileName());
    const QString md5Cmd = QStringLiteral("cd %1 && md5sum %2 > %2.md5").arg(initrdDir, initrdFile);
    return runCommandShell(md5Cmd, error);
}

bool LiveUsbMakerBackend::copyInitrdPrograms(const QString &linuxDir, const QString &initrdDir, QString *error) const
{
    const QStringList programs{QStringLiteral("ntfs-3g"), QStringLiteral("eject"), QStringLiteral("kmod"),
                               QStringLiteral("cryptsetup"), QStringLiteral("dmsetup"),
                               QStringLiteral("depmod"), QStringLiteral("insmod"), QStringLiteral("lsmod"),
                               QStringLiteral("modinfo"), QStringLiteral("modprobe")};
    const QStringList searchPaths{QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/sbin"),
                                  QStringLiteral("/usr/bin"), QStringLiteral("/sbin"), QStringLiteral("/bin")};

    const QString binDir = QDir(initrdDir).filePath(QStringLiteral("bin"));
    QDir().mkpath(binDir);

    for (const QString &prog : programs) {
        QString absPath;
        for (const QString &path : searchPaths) {
            const QString candidate = QDir(linuxDir).filePath(path + QStringLiteral("/") + prog);
            if (QFileInfo::exists(candidate)) {
                absPath = candidate;
                break;
            }
        }
        if (absPath.isEmpty()) {
            logWarn(QStringLiteral("Missing program in linuxfs: %1").arg(prog));
            continue;
        }
        runCommand(QStringLiteral("cp"), {absPath, binDir + QStringLiteral("/") + prog}, error, true);

        QString lddOutput;
        QString relPath = absPath;
        if (relPath.startsWith(linuxDir)) {
            relPath = relPath.mid(linuxDir.size());
        }
        if (relPath.isEmpty()) {
            relPath = QStringLiteral("/bin/%1").arg(prog);
        }
        runCommandOutput(QStringLiteral("chroot"),
                         {linuxDir, QStringLiteral("/usr/bin/ldd"), relPath},
                         &lddOutput, error);
        const QStringList lines = lddOutput.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        static const QRegularExpression libPathRe(QStringLiteral("=>\\s*([^ ]+)"));
        for (const QString &line : lines) {
            QString libPath;
            const QRegularExpressionMatch match = libPathRe.match(line);
            if (match.hasMatch()) {
                libPath = match.captured(1);
            } else if (line.startsWith(QLatin1String("/"))) {
                libPath = line.section(' ', 0, 0).trimmed();
            }
            if (libPath.isEmpty()) {
                continue;
            }
            const QString sourcePath = QDir(linuxDir).filePath(libPath);
            const QString destPath = QDir(initrdDir).filePath(libPath);
            QDir().mkpath(QFileInfo(destPath).path());
            runCommand(QStringLiteral("cp"), {QStringLiteral("-a"), sourcePath, destPath}, error, true);
        }
    }
    return true;
}

bool LiveUsbMakerBackend::copyInitrdModules(const QString &linuxDir, const QString &initrdDir, QString *error) const
{
    const QStringList modules{QStringLiteral("aes"), QStringLiteral("async_memcpy"), QStringLiteral("async_pq"),
                              QStringLiteral("async_raid6_recov"), QStringLiteral("async_tx"), QStringLiteral("async_xor"),
                              QStringLiteral("blowfish"), QStringLiteral("dm-crypt"), QStringLiteral("dm-mod"),
                              QStringLiteral("serpent"), QStringLiteral("sha256"), QStringLiteral("xts")};

    const QString modulesDir = QDir(linuxDir).filePath(QStringLiteral("lib/modules"));
    QDir dir(modulesDir);
    const QStringList kernels = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &kernel : kernels) {
        QSet<QString> toCopy;
        for (const QString &mod : modules) {
            toCopy.insert(mod);
        }
        bool added = true;
        while (added) {
            added = false;
            const QStringList current = toCopy.values();
            for (const QString &mod : current) {
                QString deps;
                runCommandOutput(QStringLiteral("modinfo"),
                                 {QStringLiteral("-b"), linuxDir, QStringLiteral("-k"), kernel,
                                  QStringLiteral("-F"), QStringLiteral("depends"), mod},
                                 &deps, error);
                for (const QString &dep : deps.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    const QString trimmed = dep.trimmed();
                    if (!trimmed.isEmpty() && !toCopy.contains(trimmed)) {
                        toCopy.insert(trimmed);
                        added = true;
                    }
                }
            }
        }

        for (const QString &mod : toCopy) {
            QString path;
            runCommandOutput(QStringLiteral("modinfo"),
                             {QStringLiteral("-b"), linuxDir, QStringLiteral("-k"), kernel,
                              QStringLiteral("-n"), mod},
                             &path, error);
            path = path.trimmed();
            if (path.isEmpty()) {
                continue;
            }
            const QString sourcePath = QDir(linuxDir).filePath(path);
            const QString destPath = QDir(initrdDir).filePath(path);
            QDir().mkpath(QFileInfo(destPath).path());
            runCommand(QStringLiteral("cp"), {QStringLiteral("-a"), sourcePath, destPath}, error, true);
        }
        runCommand(QStringLiteral("depmod"), {QStringLiteral("-b"), initrdDir, kernel}, error, true);
    }
    return true;
}

bool LiveUsbMakerBackend::writePassphraseFile(const QString &path, QString *error) const
{
    QFile dict(DictPaths::AMERICAN_ENGLISH);
    if (!dict.exists()) {
        dict.setFileName(DictPaths::WORDS);
    }
    if (!dict.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Unable to open dictionary for passphrase.");
        }
        return false;
    }
    const QStringList words = QString::fromUtf8(dict.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (words.isEmpty()) {
        if (error) {
            *error = QStringLiteral("Dictionary is empty.");
        }
        return false;
    }
    const int firstIndex = QRandomGenerator::global()->bounded(words.size());
    const int secondIndex = QRandomGenerator::global()->bounded(words.size());

    // Validate indices are within bounds (defensive check)
    if (firstIndex < 0 || firstIndex >= words.size() || secondIndex < 0 || secondIndex >= words.size()) {
        if (error) {
            *error = QStringLiteral("Generated invalid word indices.");
        }
        return false;
    }

    const QString phrase = words.at(firstIndex) + QStringLiteral(" ") + words.at(secondIndex);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error) {
            *error = QStringLiteral("Unable to write passphrase file.");
        }
        return false;
    }
    file.write(phrase.toUtf8());
    file.write("\n");
    return true;
}
