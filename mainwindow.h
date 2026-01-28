#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QImage>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVector>
#include <QList>

#include "maindecoder.h"

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void paintEvent(QPaintEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void initUI();
    void initFFmpeg();
    void initSlot();
    void initTray();

    QString fileType(QString file);
    void addPathVideoToList(QString path);
    void playVideo(QString file);
    void showPlayMenu();

    void setHide(QWidget *widget);
    void showControls(bool show);

    inline QString getFilenameFromPath(QString path);

    Ui::MainWindow *ui;

    bool        m_bDrag;//是否拖拽
    QPoint      m_mouseStartPoint;
    QPoint      m_windowTopLeftPoint;

    MainDecoder *m_MainDecoder;
    QList<QString> playList;    // list to stroe video files in same path

    QString currentPlay;        // current playing video file path
    QString currentPlayType;

    QTimer *m_menuTimer;      // menu hide timer
    QTimer *m_progressTimer;  // check play progress timer

    bool menuIsVisible;     // switch to control show/hide menu
    bool isKeepAspectRatio; // switch to control image scale whether keep aspect ratio

    QImage m_video_image;

    bool autoPlay;          // switch to control whether to continue to playing other file
    bool loopPlay;          // switch to control whether to continue to playing same file
    bool closeNotExit;      // switch to control click exit button not exit but hide

    MainDecoder::PlayState playState;

    QVector<QWidget *> m_hideVector;

    qint64 timeTotal;
    int seekInterval;

private slots:
    void buttonClickSlot();
    void trayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void timerSlot();
    void editText();
    void seekProgress(int value);
    void videoTime(qint64 time);
    void playStateChanged(MainDecoder::PlayState state);

    /* right click menu slot */
    void setFullScreen();
    void setKeepRatio();
    void setAutoPlay();
    void setLoopPlay();
    void saveCurrentFrame();

    void showVideo(QImage);

signals:
    void selectedVideoFile(QString file, QString type);
    void stopVideo();
    void pauseVideo();

};

#endif // MAINWINDOW_H
