/**********************************************************************
 *
 **********************************************************************
 * Copyright (C) 2023-2026 MX Authors
 *
 * Authors: Adrian <adrian@mxlinux.org>
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
#include <QDialog>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QTextBrowser>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include "about.h"

namespace
{
void setupDocDialog(QDialog &dialog, QWidget *content, const QString &title, bool largeWindow)
{
    dialog.setWindowTitle(title);
    if (largeWindow) {
        dialog.setWindowFlags(Qt::Window);
        dialog.resize(1000, 800);
    } else {
        dialog.resize(700, 600);
    }

    auto *btnClose = new QPushButton(QObject::tr("&Close"), &dialog);
    btnClose->setIcon(QIcon::fromTheme("window-close"));
    QObject::connect(btnClose, &QPushButton::clicked, &dialog, &QDialog::close);

    auto *layout = new QVBoxLayout(&dialog);
    layout->addWidget(content);
    layout->addWidget(btnClose);
}

bool isHtmlDoc(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == "html" || suffix == "htm" || suffix == "xhtml";
}

void showHtmlDoc(const QString &path, const QString &title, bool largeWindow)
{
    QDialog dialog;
    auto *browser = new QTextBrowser(&dialog);
    browser->setOpenExternalLinks(true);
    setupDocDialog(dialog, browser, title, largeWindow);

    const QUrl sourceUrl = QUrl::fromLocalFile(path);
    if (QFileInfo::exists(path)) {
        browser->setSource(sourceUrl);
    } else {
        browser->setText(QObject::tr("Could not load %1").arg(path));
    }

    dialog.exec();
}

void showPlainTextDoc(const QString &path, const QString &title, bool largeWindow)
{
    QDialog dialog;
    auto *text = new QTextEdit(&dialog);
    text->setReadOnly(true);
    setupDocDialog(dialog, text, title, largeWindow);

    QFile file(path);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        text->setPlainText(QString::fromUtf8(file.readAll()));
    } else {
        text->setPlainText(QObject::tr("Could not load %1").arg(path));
    }

    dialog.exec();
}
} // namespace

void displayDoc(const QString &path, const QString &title, bool largeWindow)
{
    if (isHtmlDoc(path)) {
        showHtmlDoc(path, title, largeWindow);
        return;
    }

    showPlainTextDoc(path, title, largeWindow);
}

void displayHelpDoc(const QString &path, const QString &title)
{
    displayDoc(path, title, true);
}

void displayAboutMsgBox(const QString &title, const QString &message, const QString &licensePath,
                        const QString &licenseTitle, QWidget *parent)
{
    const int dialogWidth = 600;
    const int dialogHeight = 500;
    QMessageBox msgBox(QMessageBox::NoIcon, title, message);

    auto *btnLicense = msgBox.addButton(QObject::tr("License"), QMessageBox::HelpRole);
    auto *btnChangelog = msgBox.addButton(QObject::tr("Changelog"), QMessageBox::HelpRole);
    auto *btnCancel = msgBox.addButton(QObject::tr("Cancel"), QMessageBox::NoRole);
    btnCancel->setIcon(QIcon::fromTheme("window-close"));

    msgBox.exec();

    if (msgBox.clickedButton() == btnLicense) {
        displayDoc(licensePath, licenseTitle);
    } else if (msgBox.clickedButton() == btnChangelog) {
        QDialog changelogDialog(parent);
        changelogDialog.setWindowTitle(QObject::tr("Changelog"));
        changelogDialog.resize(dialogWidth, dialogHeight);

        auto *textEdit = new QTextEdit(&changelogDialog);
        textEdit->setReadOnly(true);

        QProcess changelogProc;
        changelogProc.start(
            "zcat",
            {"/usr/share/doc/" + QFileInfo(QCoreApplication::applicationFilePath()).fileName() + "/changelog.gz"},
            QIODevice::ReadOnly);
        changelogProc.waitForFinished();
        textEdit->setText(changelogProc.readAllStandardOutput());

        auto *btnClose = new QPushButton(QObject::tr("&Close"), &changelogDialog);
        btnClose->setIcon(QIcon::fromTheme("window-close"));
        QObject::connect(btnClose, &QPushButton::clicked, &changelogDialog, &QDialog::close);

        auto *layout = new QVBoxLayout(&changelogDialog);
        layout->addWidget(textEdit);
        layout->addWidget(btnClose);
        changelogDialog.setLayout(layout);
        changelogDialog.exec();
    }
}
