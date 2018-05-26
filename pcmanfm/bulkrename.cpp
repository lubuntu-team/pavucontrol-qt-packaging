/*
    Copyright (C) 2017 Pedram Pourang (Tsu Jan) <tsujan2000@gmail.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "bulkrename.h"
#include <QTimer>
#include <QPushButton>
#include <QMessageBox>
#include <QProgressDialog>

#include <libfm-qt/utilities.h>

namespace PCManFM {

BulkRenameDialog::BulkRenameDialog(QWidget* parent, Qt::WindowFlags flags) :
    QDialog(parent, flags) {
    ui.setupUi(this);
    ui.lineEdit->setFocus();
    connect(ui.buttonBox->button(QDialogButtonBox::Ok), &QAbstractButton::clicked, this, &QDialog::accept);
    connect(ui.buttonBox->button(QDialogButtonBox::Cancel), &QAbstractButton::clicked, this, &QDialog::reject);
    resize(minimumSize());
    setMaximumHeight(minimumHeight()); // no vertical resizing
}

void BulkRenameDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if(ui.lineEdit->text().endsWith(QLatin1Char('#'))) { // select what's before "#"
        QTimer::singleShot(0, [this]() {
            ui.lineEdit->setSelection(0, ui.lineEdit->text().size() - 1);
        });
    }
}

BulkRenamer::BulkRenamer(const Fm::FileInfoList& files, QWidget* parent) {
    if(files.size() <= 1) { // no bulk rename with just one file
        return;
    }
    QString baseName;
    int start = 0;
    BulkRenameDialog dlg(parent);
    switch(dlg.exec()) {
    case QDialog::Accepted:
        baseName = dlg.getBaseName();
        start = dlg.getStart();
        break;
    default:
        return;
    }

    if(!baseName.contains(QLatin1Char('#'))) {
        // insert "#" before the last dot
        int end = baseName.lastIndexOf(QLatin1Char('.'));
        if(end == -1) {
            end = baseName.size();
        }
        baseName.insert(end, QLatin1Char('#'));
    }
    QProgressDialog progress(QObject::tr("Renaming files..."), QObject::tr("Abort"), 0, files.size(), parent);
    progress.setWindowModality(Qt::WindowModal);
    int i = 0, failed = 0;
    for(auto& file: files) {
        progress.setValue(i);
        if(progress.wasCanceled()) {
            progress.close();
            QMessageBox::warning(parent, QObject::tr("Warning"), QObject::tr("Renaming is aborted."));
            return;
        }
        auto fileName = QString::fromStdString(file->name());
        QString newName = baseName;
        newName.replace(QLatin1Char('#'), QString::number(start + i));
        if (newName == fileName || !Fm::changeFileName(file->path(), newName, nullptr, false)) {
            ++failed;
        }
        ++i;
    }
    progress.setValue(i);
    if(failed == i) {
        QMessageBox::critical(parent, QObject::tr("Error"), QObject::tr("No file could be renamed."));
    }
    else if(failed > 0) {
        QMessageBox::critical(parent, QObject::tr("Error"), QObject::tr("Some files could not be renamed."));
    }
}

BulkRenamer::~BulkRenamer() {

}

} //namespace PCManFM
