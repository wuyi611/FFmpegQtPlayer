#ifndef AUDIODECODER_H
#define AUDIODECODER_H

#include <QObject>

extern "C"
{
    #include "libswresample/swresample.h"
}

#include "avpacketqueue.h"

class AudioDecoder : public QObject
{
    Q_OBJECT
public:
    explicit AudioDecoder(QObject *parent = nullptr);

    int openAudio(AVFormatContext *pFormatCtx, int index);
    void closeAudio();
    void pauseAudio(bool pause);
    void stopAudio();
    int getVolume();
    void setVolume(int volume);
    double getAudioClock();
    void packetEnqueue(AVPacket *packet);
    void emptyAudioData();
    void setTotalTime(qint64 time);
    void setClock(double clk);

private:
    int decodeAudio();
    static void audioCallback(void *userdata, quint8 *stream, int SDL_AudioBufSize);

    bool isStop;            // 停止标志位
    bool isPause;           // 暂停标志位
    bool isreadFinished;    // 文件读取完成标志位

    qint64 totalTime;       // 音频总时长
    double clock;           // 音频原始时钟
    int volume;

    AVStream *stream;

    quint8 *audioBuf;               // 音频暂存缓冲区
    quint32 audioBufSize;           // 音频缓冲区大小
    // DECLARE_ALIGNED(16, ...)：内存对齐   在内存中给 audioBuf1 分配空间时，其起始地址必须是 16 的倍数。
    // quint8, audioBuf1：数据类型与变量名
    // [192000]：缓冲区容量
    DECLARE_ALIGNED(16, quint8, audioBuf1) [192000];
    quint32 audioBufSize1;
    quint32 audioBufIndex;          // 目前播放到的位置

    SDL_AudioSpec spec;             // 音频硬件参数

    quint32 audioDeviceFormat;  // audio device sample format
    quint8 audioDepth;              // 位深（字节）
    struct SwrContext *aCovertCtx;
    qint64 audioDstChannelLayout;
    enum AVSampleFormat audioDstFmt;   // audio decode sample format

    qint64 audioSrcChannelLayout;
    int audioSrcChannels;
    enum AVSampleFormat audioSrcFmt;    //
    int audioSrcFreq;

    AVCodecContext *codecCtx;          // audio codec context

    AvPacketQueue packetQueue;

    AVPacket packet;

    int sendReturn;

signals:
    void playFinished();

public slots:
    void readFileFinished();

};

#endif // AUDIODECODER_H
