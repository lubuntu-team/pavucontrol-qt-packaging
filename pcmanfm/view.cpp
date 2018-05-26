/*

    Copyright (C) 2013  Hong Jen Yee (PCMan) <pcman.tw@gmail.com>

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


#include "view.h"
#include <libfm-qt/filemenu.h>
#include <libfm-qt/foldermenu.h>
#include "application.h"
#include "settings.h"
#include "application.h"
#include "mainwindow.h"
#include "launcher.h"
#include <QAction>

namespace PCManFM {

View::View(Fm::FolderView::ViewMode _mode, QWidget* parent):
    Fm::FolderView(_mode, parent) {

    Settings& settings = static_cast<Application*>(qApp)->settings();
    updateFromSettings(settings);
}

View::~View() {
}

void View::onFileClicked(int type, const std::shared_ptr<const Fm::FileInfo>& fileInfo) {
    if(type == MiddleClick) {
        if(fileInfo->isDir()) {
            Q_EMIT openDirRequested(fileInfo->path(), OpenInNewTab);
        }
    }
    else {
        Fm::FolderView::onFileClicked(type, fileInfo);
    }
}

void View::onNewWindow() {
    Fm::FileMenu* menu = static_cast<Fm::FileMenu*>(sender()->parent());
    // FIXME: open the files in a new window
    Application* app = static_cast<Application*>(qApp);
    app->openFolders(menu->files());
}

void View::onNewTab() {
    Fm::FileMenu* menu = static_cast<Fm::FileMenu*>(sender()->parent());
    auto files = menu->files();
    for(auto& file: files) {
        Q_EMIT openDirRequested(file->path(), OpenInNewTab);
    }
}

void View::onOpenInTerminal() {
    Application* app = static_cast<Application*>(qApp);
    Fm::FileMenu* menu = static_cast<Fm::FileMenu*>(sender()->parent());
    auto files = menu->files();
    for(auto& file: files) {
        app->openFolderInTerminal(file->path());
    }
}

void View::onSearch() {

}

void View::prepareFileMenu(Fm::FileMenu* menu) {
    Application* app = static_cast<Application*>(qApp);
    menu->setConfirmDelete(app->settings().confirmDelete());
    menu->setConfirmTrash(app->settings().confirmTrash());
    menu->setUseTrash(app->settings().useTrash());

    // add some more menu items for dirs
    bool all_native = true;
    bool all_directory = true;
    auto files = menu->files();
    for(auto& fi: files) {
        if(!fi->isDir()) {
            all_directory = false;
        }
        else if(fi->isDir() && !fi->isNative()) {
            all_native = false;
        }
    }

    if(all_directory) {
        QAction* action = new QAction(QIcon::fromTheme("window-new"), tr("Open in New T&ab"), menu);
        connect(action, &QAction::triggered, this, &View::onNewTab);
        menu->insertAction(menu->separator1(), action);

        action = new QAction(QIcon::fromTheme("window-new"), tr("Open in New Win&dow"), menu);
        connect(action, &QAction::triggered, this, &View::onNewWindow);
        menu->insertAction(menu->separator1(), action);

        // TODO: add search
        // action = menu->addAction(_("Search"));

        if(all_native) {
            action = new QAction(QIcon::fromTheme("utilities-terminal"), tr("Open in Termina&l"), menu);
            connect(action, &QAction::triggered, this, &View::onOpenInTerminal);
            menu->insertAction(menu->separator1(), action);
        }
    }
    else {
        if(menu->pasteAction()) { // nullptr for trash
            menu->pasteAction()->setVisible(false);
        }
        if(menu->createAction()) {
            menu->createAction()->setVisible(false);
        }
    }
}

void View::prepareFolderMenu(Fm::FolderMenu* /*menu*/) {
}

void View::updateFromSettings(Settings& settings) {

    setIconSize(Fm::FolderView::IconMode, QSize(settings.bigIconSize(), settings.bigIconSize()));
    setIconSize(Fm::FolderView::CompactMode, QSize(settings.smallIconSize(), settings.smallIconSize()));
    setIconSize(Fm::FolderView::ThumbnailMode, QSize(settings.thumbnailIconSize(), settings.thumbnailIconSize()));
    setIconSize(Fm::FolderView::DetailedListMode, QSize(settings.smallIconSize(), settings.smallIconSize()));

    setMargins(settings.folderViewCellMargins());

    setAutoSelectionDelay(settings.autoSelectionDelay());

    Fm::ProxyFolderModel* proxyModel = model();
    if(proxyModel) {
        proxyModel->setShowThumbnails(settings.showThumbnails());
        proxyModel->setBackupAsHidden(settings.backupAsHidden());
    }
}

} // namespace PCManFM
