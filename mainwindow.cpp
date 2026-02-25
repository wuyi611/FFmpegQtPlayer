#include <QFileDialog>
#include <QStandardPaths>
#include <QPainter>
#include <QCloseEvent>
#include <QEvent>
#include <QFileInfoList>
#include <QMenu>
#include <QMessageBox>
#include <QDebug>
#include <QMovie>

#include "mainwindow.h"
#include "ui_mainwindow.h"

extern "C"
{
#include "libavformat/avformat.h"
}

#define VOLUME_INT  (13)

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_MainDecoder(new MainDecoder),
    m_menuTimer(new QTimer),
    m_progressTimer(new QTimer),
    menuIsVisible(true),
    isKeepAspectRatio(false),
    m_video_image(QImage(":/image/MUSIC.jpg")),
    autoPlay(true),
    loopPlay(false),
    closeNotExit(false),
    playState(MainDecoder::STOP),
    seekInterval(3),
    m_bDrag(false)
{
    ui->setupUi(this);

    qRegisterMetaType<MainDecoder::PlayState>("MainDecoder::PlayState");

    ///每隔3秒，检测状态，自动隐藏
    m_menuTimer->start(3000);

    m_progressTimer->setInterval(500);

    initUI();
    initTray();
    initSlot();
    initFFmpeg();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::initUI()
{
    // 设置主题及图标
    this->setWindowTitle("QtFFmpegPlayer");
    this->setWindowIcon(QIcon(":/image/player.ico"));
    // 设置鼠标追踪
    this->centralWidget()->setMouseTracking(true);
    this->setMouseTracking(true);
    // 初始化控件属性
    ui->titleLable->setAlignment(Qt::AlignCenter);
    ui->labelTime->setStyleSheet("background-color:#000;font-size:20px;font-weight:bold;color:#fff;");
    ui->labelTime->setText(QString("00.00.00 / 00:00:00"));

    ui->btnStop->setIcon(QIcon(":/image/stop.ico"));
    ui->btnStop->setIconSize(QSize(48, 48));
    ui->btnStop->setStyleSheet("background: transparent;border:none;");

    ui->btnPause->setIcon(QIcon(":/image/play.ico"));
    ui->btnPause->setIconSize(QSize(48, 48));
    ui->btnPause->setStyleSheet("background: transparent;border:none;");
    // 初始化隐藏状态
    setHide(ui->btnOpenLocal);
    setHide(ui->btnOpenUrl);
    setHide(ui->btnStop);
    setHide(ui->btnPause);
    setHide(ui->lineEdit);
    setHide(ui->videoProgressSlider);
    setHide(ui->labelTime);
    // 取消焦点
    ui->btnOpenLocal->setFocusPolicy(Qt::NoFocus);
    ui->btnOpenUrl->setFocusPolicy(Qt::NoFocus);
    ui->btnStop->setFocusPolicy(Qt::NoFocus);
    ui->btnPause->setFocusPolicy(Qt::NoFocus);


    // 安装滚动条事过滤器
    ui->videoProgressSlider->installEventFilter(this);
}

void MainWindow::initFFmpeg()
{
    // 设置ffmpeg内部日志的输出级别
    //av_log_set_level(AV_LOG_INFO);
    // 注册所有滤镜
    avfilter_register_all();

    // 注册所有复用器、解复用器、编码器、解码器（新版本5.x/6.x已不需要）
    av_register_all();

    // 初始化网络组件
    if (avformat_network_init()) {
        qDebug() << "avformat network init failed";
    }

    // 初始化sdl音频与定时器
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER)) {
        qDebug() << "SDL init failed";
    }
}

void MainWindow::initSlot()
{
    // 1. 按钮点击连接 (指向同一个槽函数)
    connect(ui->btnOpenLocal, &QPushButton::clicked, this, &MainWindow::buttonClickSlot);
    connect(ui->btnOpenUrl,   &QPushButton::clicked, this, &MainWindow::buttonClickSlot);
    connect(ui->btnStop,      &QPushButton::clicked, this, &MainWindow::buttonClickSlot);
    connect(ui->btnPause,     &QPushButton::clicked, this, &MainWindow::buttonClickSlot);

    // 2. 输入框位置变化
    connect(ui->lineEdit, &QLineEdit::cursorPositionChanged, this, &MainWindow::editText);

    // 3. 定时器连接
    connect(m_menuTimer,     &QTimer::timeout, this, &MainWindow::timerSlot);
    connect(m_progressTimer, &QTimer::timeout, this, &MainWindow::timerSlot);

    // 4. 进度条拖动
    connect(ui->videoProgressSlider, &QSlider::sliderMoved, this, &MainWindow::seekProgress);

    // 5. MainWindow 发出的指令信号 (连向解码器)
    connect(this, &MainWindow::selectedVideoFile, m_MainDecoder, &MainDecoder::decoderFile);
    connect(this, &MainWindow::stopVideo,         m_MainDecoder, &MainDecoder::stopVideo);
    connect(this, &MainWindow::pauseVideo,        m_MainDecoder, &MainDecoder::pauseVideo);

    // 6. 解码器反馈给 MainWindow 的信号
    connect(m_MainDecoder, &MainDecoder::playStateChanged, this, &MainWindow::playStateChanged);
    connect(m_MainDecoder, &MainDecoder::gotVideoTime,     this, &MainWindow::videoTime);
    connect(m_MainDecoder, &MainDecoder::gotVideo,         this, &MainWindow::showVideo);
}

void MainWindow::initTray()
{
    // 创建托盘图标对象
    QSystemTrayIcon *trayIcon = new QSystemTrayIcon(this);
    trayIcon->setToolTip(tr("QtPlayer"));
    trayIcon->setIcon(QIcon(":/image/player.ico"));
    trayIcon->show();

    // 定义菜单动作
    QAction *minimizeAction = new QAction(tr("最小化 (&I)"), this);
    connect(minimizeAction, SIGNAL(triggered()), this, SLOT(hide()));

    QAction *restoreAction = new QAction(tr("还原 (&R)"), this);
    connect(restoreAction, SIGNAL(triggered()), this, SLOT(showNormal()));

    QAction *quitAction = new QAction(tr("退出 (&Q)"), this);
    connect(quitAction, SIGNAL(triggered()), qApp, SLOT(quit()));

    // 构建右键菜单
    QMenu *trayIconMenu = new QMenu(this);

    trayIconMenu->addAction(minimizeAction);
    trayIconMenu->addAction(restoreAction);
    trayIconMenu->addSeparator();           // 添加一条分割线
    trayIconMenu->addAction(quitAction);
    trayIcon->setContextMenu(trayIconMenu);
    // 连接点击信号事件
    connect(trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(trayIconActivated(QSystemTrayIcon::ActivationReason)));
}

void MainWindow::paintEvent(QPaintEvent *event)
{
    // 忽略参数
    Q_UNUSED(event);
    // 创建画笔对象，画布为this
    QPainter painter(this);
    // 抗锯齿
    painter.setRenderHint(QPainter::Antialiasing, true);
    // 获取窗口宽高
    int width = this->width();
    int height = this->height();

    // 用黑色填充窗口
    painter.setBrush(Qt::black);
    painter.drawRect(0, 0, width, height);
    // 是否保持纵横比
    if (isKeepAspectRatio) {
        // 保持
        QImage img = m_video_image.scaled(QSize(width, height), Qt::KeepAspectRatio);

        /* calculate display position */
        int x = (this->width() - img.width()) / 2;
        int y = (this->height() - img.height()) / 2;

        painter.drawImage(QPoint(x, y), img);
    } else {
        // 不保持，填满窗口
        QImage img = m_video_image.scaled(QSize(width, height));

        painter.drawImage(QPoint(0, 0), img);
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 退出按钮为最小化
    if (closeNotExit) {
        /* ignore original close event */
        event->ignore();

        /* hide window & not show in task bar */
        this->hide();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    /* judge whether is window change event */
    // if (event->type() == QEvent::WindowStateChange) {
    //     if (this->windowState() == Qt::WindowMinimized) {
    //         /* hide window & not show in task bar */
    //         event->ignore();
    //         this->hide();
    //     } else if (this->windowState() == Qt::WindowMaximized) {

    //     }
    // }
}

bool MainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (object == ui->videoProgressSlider) {
        if (event->type() == QEvent::MouseButtonPress) {
            /*
            QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                int duration = ui->videoProgressSlider->maximum() - ui->videoProgressSlider->minimum();
                int pos = ui->videoProgressSlider->minimum() + duration * (static_cast<double>(mouseEvent->x()) / ui->videoProgressSlider->width());
                if (pos != ui->videoProgressSlider->sliderPosition()) {
                    ui->videoProgressSlider->setValue(pos);
                    decoder->seekProgress(static_cast<qint64>(pos) * 1000000);
                }
            }*/
        }
    }

    return QObject::eventFilter(object, event);
}

void MainWindow::keyReleaseEvent(QKeyEvent *event)
{
    int progressVal;
    int volumnVal = m_MainDecoder->getVolume();


    switch (event->key()) {
    case Qt::Key_Up:
        // 音量+
        if (volumnVal + VOLUME_INT > SDL_MIX_MAXVOLUME) {
            m_MainDecoder->setVolume(SDL_MIX_MAXVOLUME);
        } else {
            m_MainDecoder->setVolume(volumnVal + VOLUME_INT);
        }
        break;

    case Qt::Key_Down:
        // 音量-
        if (volumnVal - VOLUME_INT < 0) {
            m_MainDecoder->setVolume(0);
        } else {
            m_MainDecoder->setVolume(volumnVal - VOLUME_INT);
        }
        break;

    case Qt::Key_Left:
        // 快退
        if (ui->videoProgressSlider->value() > seekInterval) {
            progressVal = ui->videoProgressSlider->value() - seekInterval;
            m_MainDecoder->seekProgress(static_cast<qint64>(progressVal) * 1000000);
        }
        break;

    case Qt::Key_Right:
        // 快进
        if (ui->videoProgressSlider->value() + seekInterval < ui->videoProgressSlider->maximum()) {
            progressVal = ui->videoProgressSlider->value() + seekInterval;
            m_MainDecoder->seekProgress(static_cast<qint64>(progressVal) * 1000000);
        }
        break;

    case Qt::Key_Escape:
        // 退出全屏
        showNormal();
        break;

    case Qt::Key_Space:
        // 开始/暂停
        emit pauseVideo();
        break;

    default:
        QMainWindow::keyPressEvent(event);
        break;
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    Q_UNUSED(event);

    if (currentPlayType == "video") {
        m_menuTimer->stop(); // 只要鼠标在动，就停止倒计时
        if (!menuIsVisible) {
            showControls(true); // 显示控制栏（播放按钮、进度条等）
            menuIsVisible = true;
            QApplication::setOverrideCursor(Qt::ArrowCursor); // 恢复显示鼠标光标
        }
        m_menuTimer->start(); // 重新开始倒计时（例如 3 秒后执行隐藏）
    }

    if(m_bDrag)
    {
        //获得鼠标移动的距离
        QPoint distance = event->globalPos() - m_mouseStartPoint;
        //QPoint distance = event->pos() - mouseStartPoint;
        //改变窗口的位置
        this->move(m_windowTopLeftPoint + distance);
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if(event->button() == Qt::LeftButton)
    {
        m_bDrag = true;
        //获得鼠标的初始位置
        m_mouseStartPoint = event->globalPos();
        //mouseStartPoint = event->pos();
        //获得窗口的初始位置
        m_windowTopLeftPoint = this->frameGeometry().topLeft();
    } else if (event->button() == Qt::RightButton) {
            showPlayMenu();
        }
}

void MainWindow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->buttons() == Qt::LeftButton) {
        if (isFullScreen()) {
            showNormal();
        } else {
            showFullScreen();
        }
    }
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if(event->button() == Qt::LeftButton)
    {
        m_bDrag = false;
    }
}

void MainWindow::showPlayMenu()
{
    // 右键菜单
    QMenu *menu = new QMenu;

    QAction * fullSrcAction = new QAction("全屏", this);
    fullSrcAction->setCheckable(true);
    if (isFullScreen()) {
        fullSrcAction->setChecked(true);
    }

    QAction *keepRatioAction = new QAction("视频长宽比", this);
    keepRatioAction->setCheckable(true);
    if (isKeepAspectRatio) {
        keepRatioAction->setChecked(true);
    }

    QAction *autoPlayAction = new QAction("连续播放", this);
    autoPlayAction->setCheckable(true);
    if (autoPlay) {
        autoPlayAction->setChecked(true);
    }

    QAction *loopPlayAction = new QAction("循环播放", this);
    loopPlayAction->setCheckable(true);
    if (loopPlay) {
        loopPlayAction->setChecked(true);
    }

    QAction *captureAction = new QAction("截图", this);

    connect(fullSrcAction,      SIGNAL(triggered(bool)), this, SLOT(setFullScreen()));
    connect(keepRatioAction,    SIGNAL(triggered(bool)), this, SLOT(setKeepRatio()));
    connect(autoPlayAction,     SIGNAL(triggered(bool)), this, SLOT(setAutoPlay()));
    connect(loopPlayAction,     SIGNAL(triggered(bool)), this, SLOT(setLoopPlay()));
    connect(captureAction,      SIGNAL(triggered(bool)), this, SLOT(saveCurrentFrame()));

    menu->addAction(fullSrcAction);
    menu->addAction(keepRatioAction);
    menu->addAction(autoPlayAction);
    menu->addAction(loopPlayAction);
    menu->addAction(captureAction);

    menu->exec(QCursor::pos());

    disconnect(fullSrcAction,   SIGNAL(triggered(bool)), this, SLOT(setFullScreen()));
    disconnect(keepRatioAction, SIGNAL(triggered(bool)), this, SLOT(setKeepRatio()));
    disconnect(autoPlayAction,  SIGNAL(triggered(bool)), this, SLOT(setAutoPlay()));
    disconnect(loopPlayAction,  SIGNAL(triggered(bool)), this, SLOT(setLoopPlay()));
    disconnect(captureAction,       SIGNAL(triggered(bool)), this, SLOT(saveCurrentFrame()));

    delete fullSrcAction;
    delete keepRatioAction;
    delete autoPlayAction;
    delete loopPlayAction;
    delete captureAction;
    delete menu;
}

void MainWindow::setHide(QWidget *widget)
{
    m_hideVector.push_back(widget);
}

void MainWindow::showControls(bool show)
{
    if (show) {
        for (QWidget *widget : m_hideVector) {
            widget->show();
        }
    } else {
        for (QWidget *widget : m_hideVector) {
            widget->hide();
        }
    }
}

inline QString MainWindow::getFilenameFromPath(QString path)
{
    return path.right(path.size() - path.lastIndexOf("/") - 1);
}

QString MainWindow::fileType(QString file)
{
    // 获取文件类型
    QString type;

    QString suffix = file.right(file.size() - file.lastIndexOf(".") - 1);
    if (suffix == "mp3" || suffix == "aac" || suffix == "ape" || suffix == "flac" || suffix == "wav") {
        type = "music";
    } else {
        type = "video";
    }

    return type;
}

void MainWindow::addPathVideoToList(QString path)
{
    QDir dir(path);
    // 直接设置过滤器，支持通配符
    QStringList filters;
    filters << "*.mp4" << "*.avi" << "*.mkv" << "*.rmvb" << "*.mp3" << "*.flac"; // 按需添加

    // 获取经过过滤的文件信息
    QFileInfoList list = dir.entryInfoList(filters, QDir::Files);

    for (const QFileInfo &fileInfo : list) {
        QString absPath = fileInfo.absoluteFilePath();
        if (!playList.contains(absPath)) { // 建议根据完整路径去重，防止同名不同目录的文件被漏掉
            playList.append(absPath);
        }
    }
}

void MainWindow::playVideo(QString file)
{
    emit stopVideo();

    currentPlay = file;
    currentPlayType = fileType(file);
    if (currentPlayType == "video") {
        m_menuTimer->start();
        ui->titleLable->setText("");
    } else {
        m_menuTimer->stop();
        if (!menuIsVisible) {
            showControls(true);
            menuIsVisible = true;
        }
        ui->titleLable->setStyleSheet("color:rgb(25, 125, 203);font-size:24px;background: transparent;");
        ui->titleLable->setText(QString("当前播放：%1").arg(getFilenameFromPath(file)));
        ui->titleLable->setVisible(true);
    }

    emit selectedVideoFile(file, currentPlayType);
}

/******************* slot ************************/

void MainWindow::buttonClickSlot()
{
    QString filePath;

    if (QObject::sender() == ui->btnOpenLocal) { // open local file
        filePath = QFileDialog::getOpenFileName(
                this, "选择播放文件", "/",
                "(*.264 *.mp4 *.rmvb *.avi *.mov *.flv *.mkv *.ts *.mp3 *.flac *.ape *.wav)");
        if (!filePath.isNull() && !filePath.isEmpty()) {
            playVideo(filePath);

            QString path = filePath.left(filePath.lastIndexOf("/") + 1);
            addPathVideoToList(path);
        }
    } else if (QObject::sender() == ui->btnOpenUrl) {   // open network file
        filePath = ui->lineEdit->text();
        if (!filePath.isNull() && !filePath.isEmpty()) {
            QString type = "video";
            emit selectedVideoFile(filePath, type);
        }
    } else if (QObject::sender() == ui->btnStop) {
        emit stopVideo();
    } else if (QObject::sender() == ui->btnPause) {
        emit pauseVideo();
    }
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    // 系统托盘图标交互
    switch (reason) {
    // 双击
    case QSystemTrayIcon::DoubleClick:
        this->showNormal();         // 显示窗口
        this->raise();              // 将窗口置于最顶层
        this->activateWindow();     // 设置位活动窗口
        break;
    // 单击
    case QSystemTrayIcon::Trigger:
    default:
        break;
    }
}

void MainWindow::setFullScreen()
{
    if (isFullScreen()) {
        showNormal();
    } else {
        showFullScreen();
    }
}

void MainWindow::setKeepRatio()
{
    isKeepAspectRatio = !isKeepAspectRatio;
}

void MainWindow::setAutoPlay()
{
    autoPlay = !autoPlay;
    loopPlay = false;
}

void MainWindow::setLoopPlay()
{
    loopPlay = !loopPlay;
    autoPlay = false;
}

void MainWindow::saveCurrentFrame()
{
    QString filename = QFileDialog::getSaveFileName(this, "保存截图", "/", "(*.jpg)");
    m_video_image.save(filename);
}

void MainWindow::timerSlot()
{
    if (QObject::sender() == m_menuTimer) {
        if (menuIsVisible && playState == MainDecoder::PLAYING) {
            if (isFullScreen()) {
                QApplication::setOverrideCursor(Qt::BlankCursor);
            }
            showControls(false);
            menuIsVisible = false;
        }
    } else if (QObject::sender() == m_progressTimer) {
        if (menuIsVisible && playState == MainDecoder::PAUSE){
            return;
        }
        qint64 currentTime = static_cast<qint64>(m_MainDecoder->getCurrentTime());
        ui->videoProgressSlider->setValue( static_cast<int>(currentTime) );
        ///qDebug() << "currentTime::" << currentTime;
        int hourCurrent = currentTime / 60 / 60;
        int minCurrent  = (currentTime / 60) % 60;
        int secCurrent  = currentTime % 60;

        int hourTotal = timeTotal / 60 / 60;
        int minTotal  = (timeTotal / 60) % 60;
        int secTotal  = timeTotal % 60;

        ui->labelTime->setText(QString("%1.%2.%3 / %4:%5:%6")
                               .arg(hourCurrent, 2, 10, QLatin1Char('0'))
                               .arg(minCurrent, 2, 10, QLatin1Char('0'))
                               .arg(secCurrent, 2, 10, QLatin1Char('0'))
                               .arg(hourTotal, 2, 10, QLatin1Char('0'))
                               .arg(minTotal, 2, 10, QLatin1Char('0'))
                               .arg(secTotal, 2, 10, QLatin1Char('0')));
    }
}

void MainWindow::seekProgress(int value)
{
    m_MainDecoder->seekProgress(static_cast<qint64>(value) * 1000000);
}

void MainWindow::editText()
{
    /* forbid control hide while inputting */
    m_menuTimer->stop();
    m_menuTimer->start();
}

void MainWindow::videoTime(qint64 time)
{
    timeTotal = time / 1000000;

    ui->videoProgressSlider->setRange(0, timeTotal);

    int hour = timeTotal / 60 / 60;
    int min  = (timeTotal / 60 ) % 60;
    int sec  = timeTotal % 60;

    ui->labelTime->setText(QString("00.00.00 / %1:%2:%3")
                           .arg(hour, 2, 10, QLatin1Char('0'))
                           .arg(min,  2, 10, QLatin1Char('0'))
                           .arg(sec,  2, 10, QLatin1Char('0')));
}

void MainWindow::showVideo(QImage image)
{
    this->m_video_image = image;
    update();
}

// 播放状态机
void MainWindow::playStateChanged(MainDecoder::PlayState state)
{
    switch (state) {
    case MainDecoder::PLAYING:
        ui->btnPause->setIcon(QIcon(":/image/pause.ico"));
        playState = MainDecoder::PLAYING;
        m_progressTimer->start();
        break;

    case MainDecoder::STOP:
        m_video_image = QImage(":/image/MUSIC.jpg");
        ui->btnPause->setIcon(QIcon(":/image/play.ico"));
        playState = MainDecoder::STOP;
        m_progressTimer->stop();
        ui->labelTime->setText(QString("00.00.00 / 00:00:00"));
        ui->videoProgressSlider->setValue(0);
        timeTotal = 0;
        update();
        break;

    case MainDecoder::PAUSE:
        ui->btnPause->setIcon(QIcon(":/image/play.ico"));
        playState = MainDecoder::PAUSE;
        break;

    case MainDecoder::FINISH:
        qDebug() << "...MainDecoder::FINISH...";
        emit stopVideo();

        if (autoPlay && 0) {
            /////playNext();
        } else if (loopPlay) {
            emit selectedVideoFile(currentPlay, currentPlayType);
        }else {
            m_video_image = QImage(":/image/MUSIC.jpg");
            playState = MainDecoder::STOP;

            m_progressTimer->stop();
            ui->labelTime->setText(QString("00.00.00 / 00:00:00"));
            ui->videoProgressSlider->setValue(0);
            timeTotal = 0;
        }

        break;

    default:

        break;
    }
}


