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

#include "desktopwindow.h"
#include <QWidget>
#include <QDesktopWidget>
#include <QPainter>
#include <QImage>
#include <QImageReader>
#include <QFile>
#include <QPixmap>
#include <QPalette>

#include <QLayout>
#include <QDebug>
#include <QTimer>
#include <QTime>
#include <QSettings>
#include <QStringBuilder>
#include <QDir>
#include <QShortcut>
#include <QDropEvent>
#include <QMimeData>
#include <QPaintEvent>
#include <QStandardPaths>

#include "./application.h"
#include "mainwindow.h"
#include <libfm-qt/foldermenu.h>
#include <libfm-qt/filemenu.h>
#include <libfm-qt/folderitemdelegate.h>
#include <libfm-qt/cachedfoldermodel.h>
#include <libfm-qt/folderview_p.h>
#include <libfm-qt/fileoperation.h>
#include <libfm-qt/filepropsdialog.h>
#include <libfm-qt/utilities.h>
#include <libfm-qt/core/fileinfo.h>
#include "xdgdir.h"
#include "bulkrename.h"

#include <QX11Info>
#include <QScreen>
#include <xcb/xcb.h>
#include <X11/Xlib.h>

#define MIN_SLIDE_INTERVAL 5*60000 // 5 min
#define MAX_SLIDE_INTERVAL (24*60+55)*60000 // 24 h and 55 min

namespace PCManFM {

DesktopWindow::DesktopWindow(int screenNum):
    View(Fm::FolderView::IconMode),
    proxyModel_(nullptr),
    model_(nullptr),
    wallpaperMode_(WallpaperNone),
    slideShowInterval_(0),
    wallpaperTimer_(nullptr),
    wallpaperRandomize_(false),
    fileLauncher_(nullptr),
    showWmMenu_(false),
    desktopHideItems_(false),
    screenNum_(screenNum),
    relayoutTimer_(nullptr),
    selectionTimer_(nullptr) {

    QDesktopWidget* desktopWidget = QApplication::desktop();
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_X11NetWmWindowTypeDesktop);
    setAttribute(Qt::WA_DeleteOnClose);

    // set our custom file launcher
    View::setFileLauncher(&fileLauncher_);

    listView_ = static_cast<Fm::FolderViewListView*>(childView());
    listView_->setMovement(QListView::Snap);
    listView_->setResizeMode(QListView::Adjust);
    listView_->setFlow(QListView::TopToBottom);

    // This is to workaround Qt bug 54384 which affects Qt >= 5.6
    // https://bugreports.qt.io/browse/QTBUG-54384
    // Setting a QPixmap larger then the screen resolution to desktop's QPalette won't work.
    // So we make the viewport transparent by preventing its backround from being filled automatically.
    // Then we paint desktop's background ourselves by using its paint event handling method.
    listView_->viewport()->setAutoFillBackground(false);

    // NOTE: When XRnadR is in use, the all screens are actually combined to form a
    // large virtual desktop and only one DesktopWindow needs to be created and screenNum is -1.
    // In some older multihead setups, such as xinerama, every physical screen
    // is treated as a separate desktop so many instances of DesktopWindow may be created.
    // In this case we only want to show desktop icons on the primary screen.
    if(desktopWidget->isVirtualDesktop() || screenNum_ == desktopWidget->primaryScreen()) {
        loadItemPositions();
        Settings& settings = static_cast<Application* >(qApp)->settings();

        auto desktopPath = Fm::FilePath::fromLocalPath(XdgDir::readDesktopDir().toStdString().c_str());
        model_ = Fm::CachedFolderModel::modelFromPath(desktopPath);
        folder_ = model_->folder();
        connect(folder_.get(), &Fm::Folder::startLoading, this, &DesktopWindow::onFolderStartLoading);
        connect(folder_.get(), &Fm::Folder::finishLoading, this, &DesktopWindow::onFolderFinishLoading);

        proxyModel_ = new Fm::ProxyFolderModel();
        proxyModel_->setSourceModel(model_);
        proxyModel_->setShowThumbnails(settings.showThumbnails());
        proxyModel_->sort(settings.desktopSortColumn(), settings.desktopSortOrder());
        proxyModel_->setFolderFirst(settings.desktopSortFolderFirst());
        setModel(proxyModel_);

        connect(proxyModel_, &Fm::ProxyFolderModel::rowsInserted, this, &DesktopWindow::onRowsInserted);
        connect(proxyModel_, &Fm::ProxyFolderModel::rowsAboutToBeRemoved, this, &DesktopWindow::onRowsAboutToBeRemoved);
        connect(proxyModel_, &Fm::ProxyFolderModel::layoutChanged, this, &DesktopWindow::onLayoutChanged);
        connect(proxyModel_, &Fm::ProxyFolderModel::sortFilterChanged, this, &DesktopWindow::onModelSortFilterChanged);
        connect(proxyModel_, &Fm::ProxyFolderModel::dataChanged, this, &DesktopWindow::onDataChanged);
        connect(listView_, &QListView::indexesMoved, this, &DesktopWindow::onIndexesMoved);
    }

    // remove frame
    listView_->setFrameShape(QFrame::NoFrame);
    // inhibit scrollbars FIXME: this should be optional in the future
    listView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    listView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(this, &DesktopWindow::openDirRequested, this, &DesktopWindow::onOpenDirRequested);

    listView_->installEventFilter(this);
    listView_->viewport()->installEventFilter(this);

    // setup shortcuts
    QShortcut* shortcut;
    shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_X), this); // cut
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onCutActivated);

    shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_C), this); // copy
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onCopyActivated);

    shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_V), this); // paste
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onPasteActivated);

    shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_A), this); // select all
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::selectAll);

    shortcut = new QShortcut(QKeySequence(Qt::Key_Delete), this); // delete
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onDeleteActivated);

    shortcut = new QShortcut(QKeySequence(Qt::Key_F2), this); // rename
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onRenameActivated);

    shortcut = new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_F2), this); // bulk rename
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onBulkRenameActivated);

    shortcut = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_Return), this); // properties
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onFilePropertiesActivated);

    shortcut = new QShortcut(QKeySequence(Qt::SHIFT + Qt::Key_Delete), this); // force delete
    connect(shortcut, &QShortcut::activated, this, &DesktopWindow::onDeleteActivated);
}

DesktopWindow::~DesktopWindow() {
    listView_->viewport()->removeEventFilter(this);
    listView_->removeEventFilter(this);

    disconnect(folder_.get(), nullptr, this, nullptr);

    if(relayoutTimer_) {
        relayoutTimer_->stop();
        delete relayoutTimer_;
    }

    if(wallpaperTimer_) {
        wallpaperTimer_->stop();
        delete wallpaperTimer_;
    }

    if(proxyModel_) {
        delete proxyModel_;
    }

    if(model_) {
        disconnect(model_, &Fm::FolderModel::filesAdded, this, &DesktopWindow::onFilesAdded);
        model_->unref();
    }
}

void DesktopWindow::setBackground(const QColor& color) {
    bgColor_ = color;
}

void DesktopWindow::setForeground(const QColor& color) {
    QPalette p = listView_->palette();
    p.setBrush(QPalette::Text, color);
    listView_->setPalette(p);
    fgColor_ = color;
}

void DesktopWindow::setShadow(const QColor& color) {
    shadowColor_ = color;
    auto delegate = static_cast<Fm::FolderItemDelegate*>(listView_->itemDelegateForColumn(Fm::FolderModel::ColumnFileName));
    delegate->setShadowColor(color);
}

void DesktopWindow::onOpenDirRequested(const Fm::FilePath& path, int target) {
    Q_UNUSED(target);
    // open in new window unconditionally.
    Application* app = static_cast<Application*>(qApp);
    MainWindow* newWin = new MainWindow(path);
    // apply window size from app->settings
    newWin->resize(app->settings().windowWidth(), app->settings().windowHeight());
    newWin->show();
}

void DesktopWindow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);

    // resize wall paper if needed
    if(isVisible() && wallpaperMode_ != WallpaperNone && wallpaperMode_ != WallpaperTile) {
        updateWallpaper();
        update();
    }
    queueRelayout(100); // Qt use a 100 msec delay for relayout internally so we use it, too.
}

void DesktopWindow::setDesktopFolder() {
    if(folder_) {
        // free the previous model and folder
        if(model_) {
            disconnect(model_, &Fm::FolderModel::filesAdded, this, &DesktopWindow::onFilesAdded);
            proxyModel_->setSourceModel(nullptr);
            model_->unref(); // unref the cached model
            model_ = nullptr;
        }
        disconnect(folder_.get(), nullptr, this, nullptr);
        folder_ = nullptr;
    }

    auto path = Fm::FilePath::fromLocalPath(XdgDir::readDesktopDir().toStdString().c_str());
    model_ = Fm::CachedFolderModel::modelFromPath(path);
    folder_ = model_->folder();
    connect(folder_.get(), &Fm::Folder::startLoading, this, &DesktopWindow::onFolderStartLoading);
    connect(folder_.get(), &Fm::Folder::finishLoading, this, &DesktopWindow::onFolderFinishLoading);
    proxyModel_->setSourceModel(model_);
    if(folder_->isLoaded()) {
        onFolderStartLoading();
        onFolderFinishLoading();
    }
    else {
        onFolderStartLoading();
    }
}

void DesktopWindow::setWallpaperFile(QString filename) {
    wallpaperFile_ = filename;
}

void DesktopWindow::setWallpaperMode(WallpaperMode mode) {
    wallpaperMode_ = mode;
}

void DesktopWindow::setLastSlide(QString filename) {
    lastSlide_ = filename;
}

void DesktopWindow::setWallpaperDir(QString dirname) {
    wallpaperDir_ = dirname;
}

void DesktopWindow::setSlideShowInterval(int interval) {
    slideShowInterval_ = interval;
}

void DesktopWindow::setWallpaperRandomize(bool randomize) {
    wallpaperRandomize_ = randomize;
}

QImage DesktopWindow::loadWallpaperFile(QSize requiredSize) {
    // NOTE: for ease of programming, we only use the cache for the primary screen.
    bool useCache = (screenNum_ == -1 || screenNum_ == 0);
    QFile info;
    QString cacheFileName;
    if(useCache) {
        // see if we have a scaled version cached on disk
        cacheFileName = QString::fromLocal8Bit(qgetenv("XDG_CACHE_HOME"));
        if(cacheFileName.isEmpty()) {
            cacheFileName = QDir::homePath() % QLatin1String("/.cache");
        }
        Application* app = static_cast<Application*>(qApp);
        cacheFileName += QLatin1String("/pcmanfm-qt/") % app->profileName();
        QDir().mkpath(cacheFileName); // ensure that the cache dir exists
        cacheFileName += QLatin1String("/wallpaper.cache");

        // read info file
        QString origin;
        info.setFileName(cacheFileName % ".info");
        if(info.open(QIODevice::ReadOnly)) {
            // FIXME: we need to compare mtime to see if the cache is out of date
            origin = QString::fromLocal8Bit(info.readLine());
            info.close();
            if(!origin.isEmpty()) {
                // try to see if we can get the size of the cached image.
                QImageReader reader(cacheFileName);
                reader.setAutoDetectImageFormat(true);
                QSize cachedSize = reader.size();
                qDebug() << "size of cached file" << cachedSize << ", requiredSize:" << requiredSize;
                if(cachedSize.isValid()) {
                    if(cachedSize == requiredSize) { // see if the cached wallpaper has the size we want
                        QImage image = reader.read(); // return the loaded image
                        qDebug() << "origin" << origin;
                        if(origin == wallpaperFile_) {
                            return image;
                        }
                    }
                }
            }
        }
        qDebug() << "no cached wallpaper. generate a new one!";
    }

    // we don't have a cached scaled image, load the original file
    QImage image(wallpaperFile_);
    qDebug() << "size of original image" << image.size();
    if(image.isNull() || image.size() == requiredSize) { // if the original size is what we want
        return image;
    }

    // scale the original image
    QImage scaled = image.scaled(requiredSize.width(), requiredSize.height(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    // FIXME: should we save the scaled image if its size is larger than the original image?

    if(useCache) {
        // write the path of the original image to the .info file
        if(info.open(QIODevice::WriteOnly)) {
            info.write(wallpaperFile_.toLocal8Bit());
            info.close();

            // write the scaled cache image to disk
            const char* format; // we keep jpg format for *.jpg files, and use png format for others.
            if(wallpaperFile_.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive) || wallpaperFile_.endsWith(QLatin1String(".jpeg"), Qt::CaseInsensitive)) {
                format = "JPG";
            }
            else {
                format = "PNG";
            }
            scaled.save(cacheFileName, format);
        }
        qDebug() << "wallpaper cached saved to " << cacheFileName;
        // FIXME: we might delay the write of the cached image?
    }
    return scaled;
}

// really generate the background pixmap according to current settings and apply it.
void DesktopWindow::updateWallpaper() {
    if(wallpaperMode_ != WallpaperNone) {  // use wallpaper
        QPixmap pixmap;
        QImage image;
        if(wallpaperMode_ == WallpaperTile) { // use the original size
            image = QImage(wallpaperFile_);
            // Note: We can't use the QPainter::drawTiledPixmap(), because it doesn't tile
            // correctly for background pixmaps bigger than the current screen size.
            const QSize s = size();
            pixmap = QPixmap{s};
            QPainter painter{&pixmap};
            for (int x = 0; x < s.width(); x += image.width()) {
                for (int y = 0; y < s.height(); y += image.height()) {
                    painter.drawImage(x, y, image);
                }
            }
        }
        else if(wallpaperMode_ == WallpaperStretch) {
            image = loadWallpaperFile(size());
            pixmap = QPixmap::fromImage(image);
        }
        else { // WallpaperCenter || WallpaperFit
            if(wallpaperMode_ == WallpaperCenter) {
                image = QImage(wallpaperFile_); // load original image
            }
            else if(wallpaperMode_ == WallpaperFit || wallpaperMode_ == WallpaperZoom) {
                // calculate the desired size
                QSize origSize = QImageReader(wallpaperFile_).size(); // get the size of the original file
                if(origSize.isValid()) {
                    QSize desiredSize = origSize;
                    Qt::AspectRatioMode mode = (wallpaperMode_ == WallpaperFit ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
                    desiredSize.scale(width(), height(), mode);
                    image = loadWallpaperFile(desiredSize); // load the scaled image
                }
            }
            if(!image.isNull()) {
                pixmap = QPixmap(size());
                QPainter painter(&pixmap);
                pixmap.fill(bgColor_);
                int x = (width() - image.width()) / 2;
                int y = (height() - image.height()) / 2;
                painter.drawImage(x, y, image);
            }
        }
        wallpaperPixmap_ = pixmap;
    }
}

bool DesktopWindow::pickWallpaper() {
    if(slideShowInterval_ <= 0
       || !QFileInfo(wallpaperDir_).isDir()) {
        return false;
    }

    QList<QByteArray> formats = QImageReader::supportedImageFormats();
    QStringList formatsFilters;
    for (const QByteArray& format: formats)
        formatsFilters << QString("*.") + format;
    QDir folder(wallpaperDir_);
    QStringList files = folder.entryList(formatsFilters,
                                         QDir::Files | QDir::NoDotAndDotDot,
                                         QDir::Name);
    if(!files.isEmpty()) {
       QString dir = wallpaperDir_ + QLatin1Char('/');
       if(!wallpaperRandomize_) {
           if(!lastSlide_.startsWith(dir)) { // not in the directory
               wallpaperFile_ = dir + files.first();
           }
           else {
               QString ls = lastSlide_.remove(0, dir.size());
               if(ls.isEmpty() // invalid
                  || ls.contains(QLatin1Char('/'))) { // in a subdirectory or invalid
                   wallpaperFile_ = dir + files.first();
               }
               else {
                   int index = files.indexOf(ls);
                   if(index == -1) { // removed or invalid
                       wallpaperFile_ = dir + files.first();
                   }
                   else {
                       wallpaperFile_ = dir + (index + 1 < files.size()
                                               ? files.at(index + 1)
                                               : files.first());
                   }
               }
           }
       }
       else {
           if(files.size() > 1) {
               if(lastSlide_.startsWith(dir)) {
                   QString ls = lastSlide_.remove(0, dir.size());
                   if(!ls.isEmpty() && !ls.contains(QLatin1Char('/')))
                       files.removeOne(ls); // choose from other images
               }
               // this is needed for the randomness, especially when choosing the first wallpaper
               qsrand((uint)QTime::currentTime().msec());
               int randomValue = qrand() % files.size();
               wallpaperFile_ = dir + files.at(randomValue);
           }
           else {
               wallpaperFile_ = dir + files.first();
           }
       }

       if (lastSlide_ != wallpaperFile_) {
           lastSlide_ = wallpaperFile_;
           Settings& settings = static_cast<Application*>(qApp)->settings();
           settings.setLastSlide(lastSlide_);
           return true;
       }
    }

  return false;
}

void DesktopWindow::nextWallpaper() {
    if(pickWallpaper()) {
        updateWallpaper();
        update();
    }
}

void DesktopWindow::updateFromSettings(Settings& settings, bool changeSlide) {
    setDesktopFolder();
    setWallpaperFile(settings.wallpaper());
    setWallpaperMode(settings.wallpaperMode());
    setLastSlide(settings.lastSlide());
    QString wallpaperDir = settings.wallpaperDir();
    if(wallpaperDir_ != wallpaperDir) {
      changeSlide = true; // another wallpapaer directory; change slide!
    }
    setWallpaperDir(wallpaperDir);
    int interval = settings.slideShowInterval();
    if(interval > 0 && (interval < MIN_SLIDE_INTERVAL || interval > MAX_SLIDE_INTERVAL)) {
        interval = qBound(MIN_SLIDE_INTERVAL, interval, MAX_SLIDE_INTERVAL);
        settings.setSlideShowInterval(interval);
    }
    setSlideShowInterval(interval);
    setWallpaperRandomize(settings.wallpaperRandomize());
    setFont(settings.desktopFont());
    setIconSize(Fm::FolderView::IconMode, QSize(settings.desktopIconSize(), settings.desktopIconSize()));
    setMargins(settings.desktopCellMargins());
    // setIconSize and setMargins may trigger relayout of items by QListView, so we need to do the layout again.
    queueRelayout();
    setForeground(settings.desktopFgColor());
    setBackground(settings.desktopBgColor());
    setShadow(settings.desktopShadowColor());
    showWmMenu_ = settings.showWmMenu();
    desktopHideItems_ = settings.desktopHideItems();
    if(desktopHideItems_) {
        // hide all items by hiding the list view and also
        // prevent the current item from being changed by arrow keys
        listView_->clearFocus();
        listView_->setVisible(false);
    }

    if(slideShowInterval_ > 0
       && QFileInfo(wallpaperDir_).isDir()) {
        if(!wallpaperTimer_) {
            changeSlide = true; // slideshow activated; change slide!
            wallpaperTimer_ = new QTimer();
            connect(wallpaperTimer_, &QTimer::timeout, this, &DesktopWindow::nextWallpaper);
        }
        else {
            wallpaperTimer_->stop(); // restart the timer after updating wallpaper
        }
        if(changeSlide) {
            pickWallpaper();
        }
        else if(QFile::exists(lastSlide_)) {
            /* show the last slide if it still exists,
               otherwise show the wallpaper until timeout */
            wallpaperFile_ = lastSlide_;
        }
    }
    else if(wallpaperTimer_) {
        wallpaperTimer_->stop();
        delete wallpaperTimer_;
        wallpaperTimer_ = nullptr;
    }

    updateWallpaper();
    update();

    if(wallpaperTimer_) {
        wallpaperTimer_->start(slideShowInterval_);
    }
}

void DesktopWindow::onFileClicked(int type, const std::shared_ptr<const Fm::FileInfo>& fileInfo) {
    if(!fileInfo && showWmMenu_) {
        return;    // do not show the popup if we want to use the desktop menu provided by the WM.
    }
    if(desktopHideItems_) { // only a context menu with desktop actions
        if(type == Fm::FolderView::ActivatedClick) {
            return;
        }
        QMenu* menu = new QMenu(this);
        addDesktopActions(menu);
        menu->exec(QCursor::pos());
        delete menu;
    }
    else {
        View::onFileClicked(type, fileInfo);
    }
}

void DesktopWindow::prepareFileMenu(Fm::FileMenu* menu) {
    // qDebug("DesktopWindow::prepareFileMenu");
    PCManFM::View::prepareFileMenu(menu);
    QAction* action = new QAction(tr("Stic&k to Current Position"), menu);
    action->setCheckable(true);
    menu->insertSeparator(menu->separator2());
    menu->insertAction(menu->separator2(), action);

    bool checked(true);
    auto files = menu->files();
    for(const auto& file : files) {
        if(customItemPos_.find(file->name()) == customItemPos_.cend()) {
            checked = false;
            break;
        }
    }
    action->setChecked(checked);
    connect(action, &QAction::toggled, this, &DesktopWindow::onStickToCurrentPos);
}

void DesktopWindow::prepareFolderMenu(Fm::FolderMenu* menu) {
    PCManFM::View::prepareFolderMenu(menu);
    // remove file properties action
    menu->removeAction(menu->propertiesAction());
    // add desktop actions instead
    addDesktopActions(menu);
}

void DesktopWindow::addDesktopActions(QMenu* menu) {
    QAction* action = menu->addAction(tr("Hide Desktop Items"));
    action->setCheckable(true);
    action->setChecked(desktopHideItems_);
    menu->addSeparator();
    connect(action, &QAction::triggered, this, &DesktopWindow::toggleDesktop);
    action = menu->addAction(tr("Desktop Preferences"));
    connect(action, &QAction::triggered, this, &DesktopWindow::onDesktopPreferences);
}

void DesktopWindow::toggleDesktop() {
    desktopHideItems_ = !desktopHideItems_;
    Settings& settings = static_cast<Application*>(qApp)->settings();
    settings.setDesktopHideItems(desktopHideItems_);
    listView_->setVisible(!desktopHideItems_);
    // a relayout is needed on showing the items for the first time
    // because the positions aren't updated while the view is hidden
    if(!desktopHideItems_) {
        listView_->setFocus(); // refocus the view
        queueRelayout();
    }
    else { // prevent the current item from being changed by arrow keys
        listView_->clearFocus();
    }
}

void DesktopWindow::selectAll() {
    if(!desktopHideItems_) {
        FolderView::selectAll();
    }
}

void DesktopWindow::onDesktopPreferences() {
    static_cast<Application* >(qApp)->desktopPrefrences(QString());
}

void DesktopWindow::onRowsInserted(const QModelIndex& parent, int start, int end) {
    Q_UNUSED(parent);
    Q_UNUSED(start);
    Q_UNUSED(end);
    // disable view updates temporarily and delay relayout to prevent items from shaking
    listView_->setUpdatesEnabled(false);
    queueRelayout(100);
}

void DesktopWindow::onRowsAboutToBeRemoved(const QModelIndex& parent, int start, int end) {
    Q_UNUSED(parent);
    Q_UNUSED(start);
    Q_UNUSED(end);
    if(!customItemPos_.empty()) {
        // also delete stored custom item positions for the items currently being removed.
        // Here we can't rely on ProxyFolderModel::fileInfoFromIndex() because, although rows
        // aren't removed yet, files are already removed.
        bool changed = false;
        QString desktopDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        desktopDir += '/';
        for(auto it = customItemPos_.cbegin(); it != customItemPos_.cend();) {
            auto& name = it->first;
            if(!QFile::exists(desktopDir + QString::fromStdString(name))) {
                it = customItemPos_.erase(it);
                changed = true;
            }
            else {
                ++it;
            }
        }
        if(changed) {
            saveItemPositions();
        }
    }
    listView_->setUpdatesEnabled(false);
    queueRelayout(100);
}

void DesktopWindow::onLayoutChanged() {
    queueRelayout();
}

void DesktopWindow::onModelSortFilterChanged() {
    Settings& settings = static_cast<Application*>(qApp)->settings();
    settings.setDesktopSortColumn(static_cast<Fm::FolderModel::ColumnId>(proxyModel_->sortColumn()));
    settings.setDesktopSortOrder(proxyModel_->sortOrder());
    settings.setDesktopSortFolderFirst(proxyModel_->folderFirst());
}

void DesktopWindow::onDataChanged(const QModelIndex& topLeft, const QModelIndex& bottomRight) {
    /****************************************************************************
     NOTE: The display names of desktop entries and shortcuts may change without
     their files being renamed and, on such occasions, a relayout will be needed.
     Since there is no signal for that, we use the signal dataChanged() and the
     QHash displayNames_, which remembers such display names with every relayout.
     ****************************************************************************/
    if(topLeft.column() == 0) {
        bool relayout(false);
        for(int i = topLeft.row(); i <= bottomRight.row(); ++i) {
            QModelIndex index = topLeft.sibling(i, 0);
            if(index.isValid() && displayNames_.contains(index)) {
                auto file = proxyModel_->fileInfoFromIndex(index);
                if(displayNames_[index] != file->displayName()) {
                    relayout = true;
                    break;
                }
            }
        }
        if(relayout) {
            queueRelayout();
            // parts of the old display name might still be visible if it's long
            listView_->viewport()->update();
        }
    }
}

void DesktopWindow::onIndexesMoved(const QModelIndexList& indexes) {
    auto delegate = static_cast<Fm::FolderItemDelegate*>(listView_->itemDelegateForColumn(0));
    auto itemSize = delegate->itemSize();
    // remember the custom position for the items
    for(const QModelIndex& index : indexes) {
        // Under some circumstances, Qt might emit indexMoved for
        // every single cells in the same row. (when QAbstractItemView::SelectItems is set)
        // So indexes list may contain several indixes for the same row.
        // Since we only care about rows, not individual cells,
        // let's handle column 0 of every row here.
        if(index.column() == 0) {
            auto file = proxyModel_->fileInfoFromIndex(index);
            QRect itemRect = listView_->rectForIndex(index);
            QPoint tl = itemRect.topLeft();
            QRect workArea = qApp->desktop()->availableGeometry(screenNum_);
            workArea.adjust(12, 12, -12, -12);

            // check if the position is occupied by another item
            auto existingItem = std::find_if(customItemPos_.cbegin(), customItemPos_.cend(), [tl](const std::pair<std::string, QPoint>& elem){
                return elem.second == tl;
            });

            if(existingItem == customItemPos_.cend() // don't put items on each other
                    && tl.x() >= workArea.x() && tl.y() >= workArea.y()
                    && tl.x() + itemSize.width() <= workArea.right() + 1 // for historical reasons (-> Qt doc)
                    && tl.y() + itemSize.height() <= workArea.bottom() + 1) { // as above
                customItemPos_[file->name()] = tl;
                // qDebug() << "indexMoved:" << name << index << itemRect;
            }
        }
    }
    saveItemPositions();
    queueRelayout();
}

void DesktopWindow::onFolderStartLoading() { // desktop may be reloaded
    if(model_) {
        disconnect(model_, &Fm::FolderModel::filesAdded, this, &DesktopWindow::onFilesAdded);
    }
}

void DesktopWindow::onFolderFinishLoading() {
    QTimer::singleShot(10, [this]() { // Qt delays the UI update (as in TabPage::onFolderFinishLoading)
        if(model_) {
            connect(model_, &Fm::FolderModel::filesAdded, this, &DesktopWindow::onFilesAdded);
        }
    });
}

void DesktopWindow::onFilesAdded(const Fm::FileInfoList files) {
    if(static_cast<Application*>(qApp)->settings().selectNewFiles()) {
        if(!selectionTimer_) {
            selectFiles(files, false);
            selectionTimer_ = new QTimer (this);
            selectionTimer_->setSingleShot(true);
            selectionTimer_->start(200);
        }
        else {
            selectFiles(files, selectionTimer_->isActive());
            selectionTimer_->start(200);
        }
    }
}

void DesktopWindow::removeBottomGap() {
    /************************************************************
     NOTE: Desktop is an area bounded from below while icons snap
     to its grid srarting from above. Therefore, we try to adjust
     the vertical cell margin to prevent relatively large gaps
     from taking shape at the desktop bottom.
     ************************************************************/
    auto delegate = static_cast<Fm::FolderItemDelegate*>(listView_->itemDelegateForColumn(0));
    auto itemSize = delegate->itemSize();
    //qDebug() << "delegate:" << delegate->itemSize();
    QSize cellMargins = getMargins();
    int workAreaHeight = qApp->desktop()->availableGeometry(screenNum_).height()
                         - 24; // a 12-pix margin will be considered everywhere
    int cellHeight = itemSize.height() + listView_->spacing();
    int iconNumber = workAreaHeight / cellHeight;
    int bottomGap = workAreaHeight % cellHeight;
    /*******************************************
     First try to make room for an extra icon...
     *******************************************/
    // If one pixel is subtracted from the vertical margin, cellHeight
    // will decrease by 2 while bottomGap will increase by 2*iconNumber.
    // So, we can add an icon to the bottom once this inequality holds:
    // bottomGap + 2*n*iconNumber >= cellHeight - 2*n
    // From here, we get our "subtrahend":
    qreal exactNumber = ((qreal)cellHeight - (qreal)bottomGap)
                        / (2.0 * (qreal)iconNumber + 2.0);
    int subtrahend = (int)exactNumber + ((int)exactNumber == exactNumber ? 0 : 1);
    Settings& settings = static_cast<Application*>(qApp)->settings();
    int minCellHeight = settings.desktopCellMargins().height();
    if(subtrahend > 0
            && cellMargins.height() - subtrahend >= minCellHeight) {
        cellMargins -= QSize(0, subtrahend);
    }
    /***************************************************
     ... but if that can't be done, try to spread icons!
     ***************************************************/
    else {
        cellMargins += QSize(0, (bottomGap / iconNumber) / 2);
    }
    // set the new margins (if they're changed)
    delegate->setMargins(cellMargins);
    setMargins(cellMargins);
    // in case the text shadow is reset to (0,0,0,0)
    setShadow(settings.desktopShadowColor());
}

void DesktopWindow::paintBackground(QPaintEvent* event) {
    // This is to workaround Qt bug 54384 which affects Qt >= 5.6
    // https://bugreports.qt.io/browse/QTBUG-54384
    QPainter painter(this);
    if(wallpaperMode_ == WallpaperNone || wallpaperPixmap_.isNull()) {
        painter.fillRect(event->rect(), QBrush(bgColor_));
    }
    else {
        painter.drawPixmap(event->rect(), wallpaperPixmap_, event->rect());
    }
}

// QListView does item layout in a very inflexible way, so let's do our custom layout again.
// FIXME: this is very inefficient, but due to the design flaw of QListView, this is currently the only workaround.
void DesktopWindow::relayoutItems() {
    displayNames_.clear();
    loadItemPositions(); // something may have changed
    // qDebug("relayoutItems()");
    if(relayoutTimer_) {
        // this slot might be called from the timer, so we cannot delete it directly here.
        relayoutTimer_->deleteLater();
        relayoutTimer_ = nullptr;
    }

    QDesktopWidget* desktop = qApp->desktop();
    int screen = 0;
    int row = 0;
    int rowCount = proxyModel_->rowCount();

    auto delegate = static_cast<Fm::FolderItemDelegate*>(listView_->itemDelegateForColumn(0));
    auto itemSize = delegate->itemSize();

    for(;;) {
        if(desktop->isVirtualDesktop()) {
            if(screen >= desktop->numScreens()) {
                break;
            }
        }
        else {
            screen = screenNum_;
        }
        QRect workArea = desktop->availableGeometry(screen);
        workArea.adjust(12, 12, -12, -12); // add a 12 pixel margin to the work area
        // qDebug() << "workArea" << screen <<  workArea;
        // FIXME: we use an internal class declared in a private header here, which is pretty bad.
        QPoint pos = workArea.topLeft();
        for(; row < rowCount; ++row) {
            QModelIndex index = proxyModel_->index(row, 0);
            int itemWidth = delegate->sizeHint(listView_->getViewOptions(), index).width();
            auto file = proxyModel_->fileInfoFromIndex(index);
            // remember display names of desktop entries and shortcuts
            if(file->isDesktopEntry() || file->isShortcut()) {
                displayNames_[index] = file->displayName();
            }
            auto name = file->name();
            auto find_it = customItemPos_.find(name);
            if(find_it != customItemPos_.cend()) { // the item has a custom position
                QPoint customPos = find_it->second;
                // center the contents vertically
                listView_->setPositionForIndex(customPos + QPoint((itemSize.width() - itemWidth) / 2, 0), index);
                // qDebug() << "set custom pos:" << name << row << index << customPos;
                continue;
            }
            // check if the current pos is alredy occupied by a custom item
            bool used = false;
            for(auto it = customItemPos_.cbegin(); it != customItemPos_.cend(); ++it) {
                QPoint customPos = it->second;
                if(QRect(customPos, itemSize).contains(pos)) {
                    used = true;
                    break;
                }
            }
            if(used) { // go to next pos
                --row;
            }
            else {
                // center the contents vertically
                listView_->setPositionForIndex(pos + QPoint((itemSize.width() - itemWidth) / 2, 0), index);
                // qDebug() << "set pos" << name << row << index << pos;
            }
            // move to next cell in the column
            pos.setY(pos.y() + itemSize.height() + listView_->spacing());
            if(pos.y() + itemSize.height() > workArea.bottom() + 1) {
                // if the next position may exceed the bottom of work area, go to the top of next column
                pos.setX(pos.x() + itemSize.width() + listView_->spacing());
                pos.setY(workArea.top());

                // check if the new column exceeds the right margin of work area
                if(pos.x() + itemSize.width() > workArea.right() + 1) {
                    if(desktop->isVirtualDesktop()) {
                        // in virtual desktop mode, go to next screen
                        ++screen;
                        break;
                    }
                }
            }
        }
        if(row >= rowCount) {
            break;
        }
    }

    if(!listView_->updatesEnabled()) {
        listView_->setUpdatesEnabled(true);
    }
}

void DesktopWindow::loadItemPositions() {
    // load custom item positions
    customItemPos_.clear();
    Settings& settings = static_cast<Application*>(qApp)->settings();
    QString configFile = QString("%1/desktop-items-%2.conf").arg(settings.profileDir(settings.profileName())).arg(screenNum_);
    QSettings file(configFile, QSettings::IniFormat);

    auto delegate = static_cast<Fm::FolderItemDelegate*>(listView_->itemDelegateForColumn(0));
    auto grid = delegate->itemSize();
    QRect workArea = qApp->desktop()->availableGeometry(screenNum_);
    workArea.adjust(12, 12, -12, -12);
    QString desktopDir = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    desktopDir += '/';
    std::vector<QPoint> usedPos;
    for(auto& item: customItemPos_) {
        usedPos.push_back(item.second);
    }

    // FIXME: this is inefficient
    const auto names = file.childGroups();
    for(const QString& name : names) {
        if(!QFile::exists(desktopDir + name.toUtf8())) {
            // the file may have been removed from outside LXQT
            continue;
        }
        file.beginGroup(name);
        QVariant var = file.value("pos");
        if(var.isValid()) {
            QPoint customPos = var.toPoint();
            if(customPos.x() >= workArea.x() && customPos.y() >= workArea.y()
                    && customPos.x() + grid.width() <= workArea.right() + 1
                    && customPos.y() + grid.height() <= workArea.bottom() + 1) {
                // correct positions that are't aligned to the grid
                alignToGrid(customPos, workArea.topLeft(), grid, listView_->spacing());
                // FIXME: this is very inefficient
                while(std::find(usedPos.cbegin(), usedPos.cend(), customPos) != usedPos.cend()) {
                    customPos.setY(customPos.y() + grid.height() + listView_->spacing());
                    if(customPos.y() + grid.height() > workArea.bottom() + 1) {
                        customPos.setX(customPos.x() + grid.width() + listView_->spacing());
                        customPos.setY(workArea.top());
                    }
                }
                customItemPos_[name.toStdString()] = customPos;
                usedPos.push_back(customPos);
            }
        }
        file.endGroup();
    }
}

void DesktopWindow::saveItemPositions() {
    Settings& settings = static_cast<Application*>(qApp)->settings();
    // store custom item positions
    QString configFile = QString("%1/desktop-items-%2.conf").arg(settings.profileDir(settings.profileName())).arg(screenNum_);
    // FIXME: using QSettings here is inefficient and it's not friendly to UTF-8.
    QSettings file(configFile, QSettings::IniFormat);
    file.clear(); // remove all existing entries

    // FIXME: we have to remove dead entries not associated to any files?
    for(auto it = customItemPos_.cbegin(); it != customItemPos_.cend(); ++it) {
        auto& name = it->first;
        auto& pos = it->second;
        file.beginGroup(QString::fromStdString(name));
        file.setValue("pos", pos);
        file.endGroup();
    }
}

void DesktopWindow::onStickToCurrentPos(bool toggled) {
    QModelIndexList indexes = listView_->selectionModel()->selectedIndexes();
    if(!indexes.isEmpty()) {
        bool relayout(false);
        QModelIndexList::const_iterator it;
        for(it = indexes.constBegin(); it != indexes.constEnd(); ++it) {
            auto file = proxyModel_->fileInfoFromIndex(*it);
            auto name = file->name();
            if(toggled) { // remember the current custom position
                QRect itemRect = listView_->rectForIndex(*it);
                customItemPos_[name] = itemRect.topLeft();
            }
            else { // cancel custom position and perform relayout
                auto item = customItemPos_.find(name);
                if(item != customItemPos_.end()) {
                    customItemPos_.erase(item);
                    relayout = true;
                }
            }
        }
        saveItemPositions();
        if(relayout) {
            relayoutItems();
        }
    }
}

void DesktopWindow::queueRelayout(int delay) {
    // qDebug() << "queueRelayout";
    removeBottomGap();
    if(!relayoutTimer_) {
        relayoutTimer_ = new QTimer();
        relayoutTimer_->setSingleShot(true);
        connect(relayoutTimer_, &QTimer::timeout, this, &DesktopWindow::relayoutItems);
        relayoutTimer_->start(delay);
    }
}

// slots for file operations

void DesktopWindow::onCutActivated() {
    if(desktopHideItems_) {
        return;
    }
    auto paths = selectedFilePaths();
    if(!paths.empty()) {
        Fm::cutFilesToClipboard(paths);
    }
}

void DesktopWindow::onCopyActivated() {
    if(desktopHideItems_) {
        return;
    }
    auto paths = selectedFilePaths();
    if(!paths.empty()) {
        Fm::copyFilesToClipboard(paths);
    }
}

void DesktopWindow::onPasteActivated() {
    if(desktopHideItems_) {
        return;
    }
    Fm::pasteFilesFromClipboard(path());
}

void DesktopWindow::onDeleteActivated() {
    if(desktopHideItems_) {
        return;
    }
    auto paths = selectedFilePaths();
    if(!paths.empty()) {
        Settings& settings = static_cast<Application*>(qApp)->settings();
        bool shiftPressed = (qApp->keyboardModifiers() & Qt::ShiftModifier ? true : false);
        if(settings.useTrash() && !shiftPressed) {
            Fm::FileOperation::trashFiles(paths, settings.confirmTrash());
        }
        else {
            Fm::FileOperation::deleteFiles(paths, settings.confirmDelete());
        }
    }
}

void DesktopWindow::onRenameActivated() {
    if(desktopHideItems_) {
        return;
    }
    // do inline renaming if only one item is selected,
    // otherwise use the renaming dialog
    if(selectedIndexes().size() == 1) {
        QModelIndex cur = listView_->currentIndex();
        if (cur.isValid()) {
            listView_->edit(cur);
            return;
        }
    }
    auto files = selectedFiles();
    if(!files.empty()) {
        for(auto& info: files) {
            if(!Fm::renameFile(info, nullptr)) {
                break;
            }
        }
     }
}

void DesktopWindow::onBulkRenameActivated() {
    if(desktopHideItems_) {
        return;
    }
    BulkRenamer(selectedFiles(), this);
}

void DesktopWindow::onFilePropertiesActivated() {
    if(desktopHideItems_) {
        return;
    }
    auto files = selectedFiles();
    if(!files.empty()) {
        Fm::FilePropsDialog::showForFiles(std::move(files));
    }
}

static void forwardMouseEventToRoot(QMouseEvent* event) {
    xcb_ungrab_pointer(QX11Info::connection(), event->timestamp());
    // forward the event to the root window
    xcb_button_press_event_t xcb_event;
    uint32_t mask = 0;
    xcb_event.state = 0;
    switch(event->type()) {
    case QEvent::MouseButtonPress:
        xcb_event.response_type = XCB_BUTTON_PRESS;
        mask = XCB_EVENT_MASK_BUTTON_PRESS;
        break;
    case QEvent::MouseButtonRelease:
        xcb_event.response_type = XCB_BUTTON_RELEASE;
        mask = XCB_EVENT_MASK_BUTTON_RELEASE;
        break;
    default:
        return;
    }

    // convert Qt button to XCB button
    switch(event->button()) {
    case Qt::LeftButton:
        xcb_event.detail = 1;
        xcb_event.state |= XCB_BUTTON_MASK_1;
        break;
    case Qt::MiddleButton:
        xcb_event.detail = 2;
        xcb_event.state |= XCB_BUTTON_MASK_2;
        break;
    case Qt::RightButton:
        xcb_event.detail = 3;
        xcb_event.state |= XCB_BUTTON_MASK_3;
        break;
    default:
        xcb_event.detail = 0;
    }

    // convert Qt modifiers to XCB states
    if(event->modifiers() & Qt::ShiftModifier) {
        xcb_event.state |= XCB_MOD_MASK_SHIFT;
    }
    if(event->modifiers() & Qt::ControlModifier) {
        xcb_event.state |= XCB_MOD_MASK_SHIFT;
    }
    if(event->modifiers() & Qt::AltModifier) {
        xcb_event.state |= XCB_MOD_MASK_1;
    }

    xcb_event.sequence = 0;
    xcb_event.time = event->timestamp();

    WId root = QX11Info::appRootWindow(QX11Info::appScreen());
    xcb_event.event = root;
    xcb_event.root = root;
    xcb_event.child = 0;

    xcb_event.root_x = event->globalX();
    xcb_event.root_y = event->globalY();
    xcb_event.event_x = event->x();
    xcb_event.event_y = event->y();
    xcb_event.same_screen = 1;

    xcb_send_event(QX11Info::connection(), 0, root, mask, (char*)&xcb_event);
    xcb_flush(QX11Info::connection());
}

bool DesktopWindow::event(QEvent* event) {
    switch(event->type()) {
    case QEvent::WinIdChange: {
        //qDebug() << "winid change:" << effectiveWinId();
        if(effectiveWinId() == 0) {
            break;
        }
        // set freedesktop.org EWMH hints properly
        if(QX11Info::isPlatformX11() && QX11Info::connection()) {
            xcb_connection_t* con = QX11Info::connection();
            const char* atom_name = "_NET_WM_WINDOW_TYPE_DESKTOP";
            xcb_atom_t atom = xcb_intern_atom_reply(con, xcb_intern_atom(con, 0, strlen(atom_name), atom_name), nullptr)->atom;
            const char* prop_atom_name = "_NET_WM_WINDOW_TYPE";
            xcb_atom_t prop_atom = xcb_intern_atom_reply(con, xcb_intern_atom(con, 0, strlen(prop_atom_name), prop_atom_name), nullptr)->atom;
            xcb_atom_t XA_ATOM = 4;
            xcb_change_property(con, XCB_PROP_MODE_REPLACE, effectiveWinId(), prop_atom, XA_ATOM, 32, 1, &atom);
        }
        break;
    }
#undef FontChange // FontChange is defined in the headers of XLib and clashes with Qt, let's undefine it.
    case QEvent::StyleChange:
    case QEvent::FontChange:
        queueRelayout();
        break;

    default:
        break;
    }

    return QWidget::event(event);
}

#undef FontChange // this seems to be defined in Xlib headers as a macro, undef it!

bool DesktopWindow::eventFilter(QObject* watched, QEvent* event) {
    if(watched == listView_) {
        switch(event->type()) {
        case QEvent::StyleChange:
        case QEvent::FontChange:
            if(model_) {
                queueRelayout();
            }
            break;
        default:
            break;
        }
    }
    else if(watched == listView_->viewport()) {
        switch(event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
            if(showWmMenu_) {
                QMouseEvent* e = static_cast<QMouseEvent*>(event);
                // If we want to show the desktop menus provided by the window manager instead of ours,
                // we have to forward the mouse events we received to the root window.
                // check if the user click on blank area
                QModelIndex index = listView_->indexAt(e->pos());
                if(!index.isValid() && e->button() != Qt::LeftButton) {
                    forwardMouseEventToRoot(e);
                }
            }
            break;
        default:
            break;
        }
    }
    return Fm::FolderView::eventFilter(watched, event);
}

void DesktopWindow::childDropEvent(QDropEvent* e) {
    const QMimeData* mimeData = e->mimeData();
    bool moveItem = false;
    if(e->source() == listView_ && e->keyboardModifiers() == Qt::NoModifier) {
        // drag source is our list view, and no other modifier keys are pressed
        // => we're dragging desktop items
        if(mimeData->hasFormat("application/x-qabstractitemmodeldatalist")) {
            QModelIndex dropIndex = listView_->indexAt(e->pos());
            if(dropIndex.isValid()) { // drop on an item
                QModelIndexList selected = selectedIndexes(); // the dragged items
                if(selected.contains(dropIndex)) { // drop on self, ignore
                    moveItem = true;
                }
            }
            else { // drop on a blank area
                moveItem = true;
            }
        }
    }
    if(moveItem) {
        e->accept();
    }
    else {
        auto delegate = static_cast<Fm::FolderItemDelegate*>(listView_->itemDelegateForColumn(0));
        auto grid = delegate->itemSize();
        Fm::FolderView::childDropEvent(e);
        // position dropped items successively, starting with the drop rectangle
        if(mimeData->hasUrls()
           && (e->dropAction() == Qt::CopyAction
               || e->dropAction() == Qt::MoveAction
               || e->dropAction() == Qt::LinkAction)) {
            QList<QUrl> urlList = mimeData->urls();
            for(int i = 0; i < urlList.count(); ++i) {
                std::string name = urlList.at(i).fileName().toUtf8().constData();
                if(!name.empty()) { // respect the positions of existing files
                    QString desktopDir = XdgDir::readDesktopDir() + QString(QLatin1String("/"));
                    if(!QFile::exists(desktopDir + QString::fromStdString(name))) {
                        QRect workArea = qApp->desktop()->availableGeometry(screenNum_);
                        workArea.adjust(12, 12, -12, -12);
                        QPoint pos = mapFromGlobal(e->pos());
                        alignToGrid(pos, workArea.topLeft(), grid, listView_->spacing());
                        if(i > 0)
                            pos.setY(pos.y() + grid.height() + listView_->spacing());
                        if(pos.y() + grid.height() > workArea.bottom() + 1) {
                            pos.setX(pos.x() + grid.width() + listView_->spacing());
                            pos.setY(workArea.top());
                        }
                        customItemPos_[name] = pos;
                    }
                }
            }
            saveItemPositions();
        }
    }
}

void DesktopWindow::alignToGrid(QPoint& pos, const QPoint& topLeft, const QSize& grid, const int spacing) {
    qreal w = qAbs((qreal)pos.x() - (qreal)topLeft.x())
              / (qreal)(grid.width() + spacing);
    qreal h = qAbs(pos.y() - (qreal)topLeft.y())
              / (qreal)(grid.height() + spacing);
    pos.setX(topLeft.x() + qRound(w) * (grid.width() + spacing));
    pos.setY(topLeft.y() + qRound(h) * (grid.height() + spacing));
}

void DesktopWindow::closeEvent(QCloseEvent* event) {
    // prevent the desktop window from being closed.
    event->ignore();
}

void DesktopWindow::paintEvent(QPaintEvent *event) {
    paintBackground(event);
    QWidget::paintEvent(event);
}

void DesktopWindow::setScreenNum(int num) {
    if(screenNum_ != num) {
        screenNum_ = num;
        queueRelayout();
    }
}

} // namespace PCManFM
