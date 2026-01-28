#include <QApplication>
#include <QTextCodec>
#include <QFile>>

#include "mainwindow.h"


int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    QTextCodec *codec = QTextCodec::codecForName("UTF-8");

    QTextCodec::setCodecForLocale(codec);

    a.setFont(QFont("Microsoft YaHei"));

    MainWindow w;
    w.show();

    QFile qss(":/qss/main.qss");
    qss.open(QFile::ReadOnly);

    a.setStyleSheet(qss.readAll());
    qss.close();

    return a.exec();
}




