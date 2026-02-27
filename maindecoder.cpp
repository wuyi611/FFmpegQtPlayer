#include <QDebug>

#include "maindecoder.h"

MainDecoder::MainDecoder() :
    timeTotal(0),
    playState(STOP),
    isStop(false),
    isPause(false),
    isSeek(false),
    isReadFinished(false),
    audioDecoder(new AudioDecoder),
    filterGraph(NULL)
{
    // 清空解码线程缓存（旧API）
    // 先初始化为默认值
    av_init_packet(&seekPacket);
    seekPacket.data = (uint8_t *)"FLUSH";

    // 连接信号：音频播放结束 -> 通知主解码器
    connect(audioDecoder, &AudioDecoder::playFinished, this, &MainDecoder::audioFinished);
    // 连接信号：文件读取结束 -> 通知音频解码器
    connect(this, &MainDecoder::readFinished, audioDecoder, &AudioDecoder::readFileFinished);
}

MainDecoder::~MainDecoder()
{

}

// 显示img
void MainDecoder::displayVideo(QImage image)
{

    emit gotVideo(image);
}

// 重置播放器状态
void MainDecoder::clearData()
{

    videoIndex = -1,
    audioIndex = -1,
    subtitleIndex = -1,

    timeTotal = 0;

    isStop  = false;
    isPause = false;
    isSeek  = false;
    isReadFinished      = false;
    isDecodeFinished    = false;

    videoQueue.empty();

    audioDecoder->emptyAudioData();

    videoClk = 0;
}

// 更新播放状态
void MainDecoder::setPlayState(MainDecoder::PlayState state)
{

    // 通知主线程状态改变状态
    emit playStateChanged(state);
    playState = state;
}

// 判断是否为实时流
bool MainDecoder::isRealtime(AVFormatContext *pFormatCtx)
{
    // 判断
    if (!strcmp(pFormatCtx->iformat->name, "rtp")
        || !strcmp(pFormatCtx->iformat->name, "rtsp")
        || !strcmp(pFormatCtx->iformat->name, "sdp")) {
         return true;
    }

    if(pFormatCtx->pb && (!strncmp(pFormatCtx->filename, "rtp:", 4)
        || !strncmp(pFormatCtx->filename, "udp:", 4)
        )) {
        return true;
    }

    return false;
}

// 初始化滤镜
int MainDecoder::initFilter()
{
    int ret;

    AVFilterInOut *out = avfilter_inout_alloc();
    AVFilterInOut *in = avfilter_inout_alloc();
    // 输出格式为RGB32
    enum AVPixelFormat pixFmts[] = {AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE};     // AV_PIX_FMT_NONE 类似字符串里的\0，结束变量

    // 释放上一个graph
    if (filterGraph) {
        avfilter_graph_free(&filterGraph);
    }
    // 分配新的graph
    filterGraph = avfilter_graph_alloc();

    /* 处理滤镜
     * hb	Horizontal Deblocking	水平去块滤镜。消除水平方向上的块状效应（马赛克）。
     * vb	Vertical Deblocking     垂直去块滤镜。消除垂直方向上的块状效应。
     * dr	Deringing               去环效应。消除物体边缘常见的“重影”或“振铃”噪点。
     * al	Autolevels              自动亮度/对比度。自动拉伸亮度范围，使画面层次感更强。
     */
    QString filter("pp=hb/vb/dr/al");

    /* 输入格式参数
     * video_size   width x height      视频的分辨率。滤镜需要知道画幅大小来分配内存或计算缩放。
     * pix_fmt      pCodecCtx->pix_fmt	像素格式（如 YUV420P, NV12）。这是滤镜最关心的，决定了数据如何排列。
     * time_base	num / den           时间基准。用于将帧的 pts (时间戳) 转换为实际秒数，对时间相关的滤镜（如 fps 或 setpts）至关重要。
     * pixel_aspect	num / den           采样长宽比 (SAR)。告诉滤镜像素是正方形还是长方形，防止画面被拉伸变形。
     */
    QString args = QString("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:pixel_aspect=%6/%7")
            .arg(pCodecCtx->width).arg(pCodecCtx->height).arg(av_get_pix_fmt_name(pCodecCtx->pix_fmt))
            .arg(videoStream->time_base.num).arg(videoStream->time_base.den)
            .arg(pCodecCtx->sample_aspect_ratio.num).arg(pCodecCtx->sample_aspect_ratio.den);

    // 创建源滤镜（输入滤镜），接收原始帧
    ret = avfilter_graph_create_filter(&filterSrcCxt, avfilter_get_by_name("buffer"), "in", args.toLocal8Bit().data(), NULL, filterGraph);
    if (ret < 0) {
        qDebug() << "avfilter graph create filter failed, ret:" << ret;
        avfilter_graph_free(&filterGraph);
        goto out;
    }

    // 创建汇滤镜（输出滤镜），输出处理后的帧
    ret = avfilter_graph_create_filter(&filterSinkCxt, avfilter_get_by_name("buffersink"), "out", NULL, NULL, filterGraph);
    if (ret < 0) {
        qDebug() << "avfilter graph create filter failed, ret:" << ret;
        avfilter_graph_free(&filterGraph);
        goto out;
    }

    // 设置汇滤镜的输出格式为RGB32（显示传入 AV_PIX_FMT_NONE 结束变量）
    ret = av_opt_set_int_list(filterSinkCxt, "pix_fmts", pixFmts, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        qDebug() << "av opt set int list failed, ret:" << ret;
        avfilter_graph_free(&filterGraph);
        goto out;
    }
    // 源滤镜是缓冲区的输出，是滤镜链的输入
    out->name       = av_strdup("in");
    out->filter_ctx = filterSrcCxt;
    out->pad_idx    = 0;
    out->next       = NULL;
    // 汇滤镜是缓冲区的输入，是滤镜链的输出
    in->name       = av_strdup("out");
    in->filter_ctx = filterSinkCxt;
    in->pad_idx    = 0;
    in->next       = NULL;

    if (filter.isEmpty() || filter.isNull()) {
        // 如果没有指定滤镜字符串，直接把源和汇连起来
        ret = avfilter_link(filterSrcCxt, 0, filterSinkCxt, 0);
        if (ret < 0) {
            qDebug() << "avfilter link failed, ret:" << ret;
            avfilter_graph_free(&filterGraph);
            goto out;
        }
    } else {
        // 解析滤镜字符串，构建中间的滤镜链，连接 source -> filter -> sink
        ret = avfilter_graph_parse_ptr(filterGraph, filter.toLatin1().data(), &in, &out, NULL);
        if (ret < 0) {
            qDebug() << "avfilter graph parse ptr failed, ret:" << ret;
            avfilter_graph_free(&filterGraph);
            goto out;
        }
    }

    // 最终检查并配置整个滤镜图
    if ((ret = avfilter_graph_config(filterGraph, NULL)) < 0) {
        qDebug() << "avfilter graph config failed, ret:" << ret;
        avfilter_graph_free(&filterGraph);
    }

out:
    // 释放资源
    avfilter_inout_free(&out);
    avfilter_inout_free(&in);

    return ret;
}

// 开始解码线程
void MainDecoder::decoderFile(QString file, QString type)
{
    // 先暂停旧线程
    qDebug() << "File name:" << file << ", type:" << type;
    if (playState != STOP) {
        isStop = true;
        while (playState != STOP) {
            SDL_Delay(10);
        }
        SDL_Delay(100);
    }
    // 重置数据
    clearData();

    SDL_Delay(100);

    currentFile = file;
    currentType = type;
    // 开始新线程
    this->start();
}

// 音频解码线程通知音频播放完成
void MainDecoder::audioFinished()
{
    // 音频播放完成
    isStop = true;
    if (currentType == "music") {
        // 如果播放的是音频则直接将状态变为finish
        SDL_Delay(100);
        // 通知主线程播放完成
        emit playStateChanged(MainDecoder::FINISH);
    }
}

// 主线程通知停止播放
void MainDecoder::stopVideo()
{
    if (playState == STOP) {
        // 如果已经为停止状态直接设置状态为停止
        setPlayState(MainDecoder::STOP);
        return;
    }

    // gotstop代表主线程主动结束，等待解码线程退出循环后设置为stop
    gotStop = true;
    isStop  = true;
    // 通知音频解码线程停止解码
    audioDecoder->stopAudio();

    if (currentType == "video") {
        // 视频模式：必须等待“读取”和“解码”两个动作都停稳
        while (!isReadFinished || !isDecodeFinished) {
            SDL_Delay(10);
        }
    } else {
        // 音乐模式：只需要等“读取”动作停稳
        while (!isReadFinished) {
            SDL_Delay(10);
        }
    }
}

// 主线程通知暂停或回复播放
void MainDecoder::pauseVideo()
{
    qDebug() << "leftbutton.Decoder::pauseVideo()";
    if (playState == STOP) {
        qDebug() << "leftbutton.Decoder::pauseVideo()...stop";
        return;
    }

    isPause = !isPause;
    // 通知音频解码线程暂停或者恢复播放
    audioDecoder->pauseAudio(isPause);
    if (isPause) {
        // 通知数据源暂停
        av_read_pause(pFormatCtx);
        // 改变本线程状态
        setPlayState(PAUSE);
    } else {
        // 通知数据源继续
        av_read_play(pFormatCtx);
        // 改变本线程状态
        setPlayState(PLAYING);
    }
}

// 主线程获取音量
int MainDecoder::getVolume()
{
    return audioDecoder->getVolume();
}

// 主线程设置音量
void MainDecoder::setVolume(int volume)
{
    audioDecoder->setVolume(volume);
}

// 主线程获取当前时间（音频作为主时钟）
double MainDecoder::getCurrentTime()
{
    if (audioIndex >= 0) {
        return audioDecoder->getAudioClock();
    }

    return 0;
}

// 主线程跳转请求拦截
void MainDecoder::seekProgress(qint64 pos)
{
    if (!isSeek) {
        seekPos = pos;
        isSeek = true;
    }
}


double MainDecoder::synchronize(AVFrame *frame, double pts)
{
    double delay;
    if (pts != 0) {
        videoClk = pts; // 如果当前帧自带 PTS，则用它更新视频时钟
    } else {
        pts = videoClk; // 如果当前帧没有 PTS，则沿用上一次计算出的视频时钟
    }

    // 使用时间基预测下次时间戳
    delay = av_q2d(pCodecCtx->time_base);
    delay += frame->repeat_pict * (delay * 0.5);

    videoClk += delay;

    return pts;
}

int MainDecoder::videoThread(void *arg)
{
    int ret;
    double pts;
    AVPacket packet;
    // 将this指针强转为MainDecoder来访问类的公有变量
    MainDecoder *decoder = (MainDecoder *)arg;
    AVFrame *pFrame  = av_frame_alloc();

    while (true) {
        if (decoder->isStop) {
            break;
        }

        if (decoder->isPause) {
            SDL_Delay(10);
            continue;
        }

        if (decoder->videoQueue.queueSize() <= 0) {
            // 如果队列是空的，且 isReadFinished 标志为真（表示文件读取线程已经读完了所有数据）
            // 说明视频已经播放完了，跳出主循环，结束线程
            if (decoder->isReadFinished) {
                break;
            }
            // 如果队列为空但文件还没读完（数据还没送来），让线程休眠 1 毫秒，防止空转消耗 CPU
            // 跳过本次循环，回到开头等待数据到来
            SDL_Delay(1);
            continue;
        }

        // 从视频队列中取出一个数据包（Packet）存入 packet 变量中。参数 true 通常表示这是一个阻塞操作
        decoder->videoQueue.dequeue(&packet, true);

        // 检查取出的包的数据内容是不是字符串 "FLUSH"。这通常是自定义的特殊包，用于在用户**拖动进度条（Seek）**时清空缓存。
        if (!strcmp((char *)packet.data, "FLUSH")) {
            qDebug() << "Seek video";
            // 调用 FFmpeg API 清空解码器上下文中的内部缓存。这是 Seek 操作必须的，否则画面会花屏。
            avcodec_flush_buffers(decoder->pCodecCtx);
            av_packet_unref(&packet);
            continue;
        }

        ret = avcodec_send_packet(decoder->pCodecCtx, &packet);
        // 检查返回值。如果返回值小于0，且错误不是“需要更多数据(EAGAIN)”或“文件结束(EOF)”，则表示发生了真正的错误
        if ((ret < 0) && (ret != AVERROR(EAGAIN)) && (ret != AVERROR_EOF)) {
            qDebug() << "Video send to decoder failed, error code: " << ret;
            av_packet_unref(&packet);
            continue;
        }

        /// raw yuv
        ret = avcodec_receive_frame(decoder->pCodecCtx, pFrame);
        if ((ret < 0) && (ret != AVERROR_EOF)) {
            qDebug() << "Video frame decode failed, error code: " << ret;
            av_packet_unref(&packet);
            continue;
        }

        // 获取当前帧的显示时间戳 pts。如果该帧没有标记时间戳（等于 AV_NOPTS_VALUE）
        // 将 pts 强制置为 0，防止无效值参与计算
        if ((pts = pFrame->pts) == AV_NOPTS_VALUE) {
            pts = 0;
        }

        /// 音视频同步:关键
        pts *= av_q2d(decoder->videoStream->time_base);
        pts =  decoder->synchronize(pFrame, pts);

        // 判断是否存在音频流（audioIndex >= 0）。
        // 只有有音频时，才需要视频去追音频
        if (decoder->audioIndex >= 0) {
            // 视频同步音频循环
            while (1) {
                if (decoder->isStop || decoder->isPause) {
                    break;
                }

                // 获取当前音频播放到的时间点（秒），作为同步的基准时钟。
                double audioClk = decoder->audioDecoder->getAudioClock();

                // 若追求高精度同步则注释此行代码
                // // 用预测的下一帧视频和音频同步
                // pts = decoder->videoClk;

                // 若视频时间戳等于音频时间戳则退出同步循环，立即渲染此帧画面
                if (pts <= audioClk) {
                     break;
                }

                // 如果视频快了（pts > audioClk），计算两者的时间差，并转换为毫秒
                int delayTime = (pts - audioClk) * 1000;

                // 限制最大休眠时间为 5 毫秒。这是为了防止休眠太久导致无法响应操作，采用“小步快跑”的策略
                delayTime = delayTime > 5 ? 5 : delayTime;

                SDL_Delay(delayTime);
            }
        }

        // 将解码出来的原始帧 pFrame 添加到滤镜图的输入端（filterSrcCxt）。
        // 这个滤镜图通常用于将 YUV 格式转换为 RGB 格式
        if (av_buffersrc_add_frame(decoder->filterSrcCxt, pFrame) < 0) {
            qDebug() << "av buffersrc add frame failed.";
            av_packet_unref(&packet);
            continue;
        }

        // 从滤镜图的输出端（filterSinkCxt）获取处理好（已转为 RGB）的帧，覆盖写入 pFrame
        if (av_buffersink_get_frame(decoder->filterSinkCxt, pFrame) < 0) {
            qDebug() << "av buffersrc get frame failed.";
            av_packet_unref(&packet);
            continue;
        } else {
            // 使用 pFrame 中的数据（data[0] 指向像素数组）构造一个 Qt 的 QImage 对象。
            // 这里假设滤镜已经转成了 RGB32 格式，大小为宽 x 高。
            // 注意这里没有发生数据拷贝，只是引用。
            QImage tmpImage(pFrame->data[0], decoder->pCodecCtx->width, decoder->pCodecCtx->height, QImage::Format_RGB32);
            /* deep copy, otherwise when tmpImage data change, this image cannot display */
            QImage image = tmpImage.copy();
            decoder->displayVideo(image);
        }

        av_frame_unref(pFrame);
        av_packet_unref(&packet);
    }

    av_frame_free(&pFrame);

    if (!decoder->isStop) {
        decoder->isStop = true;
    }

    qDebug() << "Video decoder finished.";

    SDL_Delay(100);

    // 解码完成
    decoder->isDecodeFinished = true;

    // 如果是主线程通知结束则是stop否则为finish
    if (decoder->gotStop) {
        decoder->setPlayState(MainDecoder::STOP);
    } else {
        decoder->setPlayState(MainDecoder::FINISH);
    }

    return 0;
}


void MainDecoder::run()
{
    AVCodec *pCodec;

    AVPacket pkt, *packet = &pkt;        // packet use in decoding

    int seekIndex;          // 跳转的流索引
    bool realTime;

    pFormatCtx = avformat_alloc_context();

    if (avformat_open_input(&pFormatCtx, currentFile.toLocal8Bit().data(), NULL, NULL) != 0) {
        qDebug() << "Open file failed.";
        return ;
    }

    if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
        qDebug() << "Could't find stream infomation.";
        avformat_free_context(pFormatCtx);
        return;
    }

    // 判断是否是实时流
    realTime = isRealtime(pFormatCtx);

    // 主要作用是将多媒体文件的**元数据（Metadata）和流信息（Stream Information）**以格式化的方式直接打印到控制台
    // av_dump_format(pFormatCtx, 0, 0, 0);  // just use in debug output

    /* find video & audio stream index */
    for (unsigned int i = 0; i < pFormatCtx->nb_streams; i++) {
        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            videoIndex = i;
            qDebug() << "Find video stream.";
        }

        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioIndex = i;
            qDebug() << "Find audio stream.";
        }

        if (pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            subtitleIndex = i;
            qDebug() << "Find subtitle stream.";
        }
    }

    if (currentType == "video") {
        if (videoIndex < 0) {
            qDebug() << "Not support this video file, videoIndex: " << videoIndex << ", audioIndex: " << audioIndex;
            avformat_free_context(pFormatCtx);
            return;
        }
    } else {
        if (audioIndex < 0) {
            qDebug() << "Not support this audio file.";
            avformat_free_context(pFormatCtx);
            return;
        }
    }

    if (!realTime) {
        // 给主线程发送时长
        emit gotVideoTime(pFormatCtx->duration);
        timeTotal = pFormatCtx->duration;
    } else {
        emit gotVideoTime(0);
    }

    if (audioIndex >= 0) {
        // 打开音频解码器：入口，注册回调函数
        if (audioDecoder->openAudio(pFormatCtx, audioIndex) < 0) {
            avformat_free_context(pFormatCtx);
            return;
        }
    }

    if (currentType == "video") {
        // 创建解码线程
        /* find video decoder */        
        pCodecCtx = avcodec_alloc_context3(NULL);
        avcodec_parameters_to_context(pCodecCtx, pFormatCtx->streams[videoIndex]->codecpar);

        /* find video decoder */
        if ((pCodec = avcodec_find_decoder(pCodecCtx->codec_id)) == NULL) {
            qDebug() << "Video decoder not found.";
            goto fail;
        }

        // 打开视频解码器
        if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
            qDebug() << "Could not open video decoder.";
            goto fail;
        }

        videoStream = pFormatCtx->streams[videoIndex];

        if (initFilter() < 0) {
            goto fail;
        }

        SDL_CreateThread(&MainDecoder::videoThread, "video_thread", this);
    }

    setPlayState(MainDecoder::PLAYING);

    while (true) {
        // 开启文件读取循环
        if (isStop) {
            // 退出循环，线程停止
            break;
        }

        /* do not read next frame & delay to release cpu utilization */
        if (isPause) {
            // 线程暂停
            SDL_Delay(10);
            continue;
        }

/* this seek just use in playing music, while read finished
 * & have out of loop, then jump back to seek position
 */
seek:
        // 执行跳转操作
        if (isSeek) {
            if (currentType == "video") {
                seekIndex = videoIndex;
            } else {
                seekIndex = audioIndex;
            }

            // 获取FFmpeg 内部时间基准(通常是 1/1000000)
            AVRational aVRational = av_get_time_base_q();
            // 将显示时间转换为视频内部刻度
            seekPos = av_rescale_q(seekPos, aVRational, pFormatCtx->streams[seekIndex]->time_base);

            // 执行跳转
            // AVSEEK_FLAG_BACKWARD：这是一个非常稳妥的标志。它的意思是：如果 seekPos 处没有关键帧，就往**回（前）**找最近的一个 I 帧。
            // 这样能保证跳转后画面能立即正常显示，而不是花屏。
            if (av_seek_frame(pFormatCtx, seekIndex, seekPos, AVSEEK_FLAG_BACKWARD) < 0) {
                qDebug() << "Seek failed.";

            } else {
                // 清空音频解码缓存
                audioDecoder->emptyAudioData();
                audioDecoder->packetEnqueue(&seekPacket);

                if (currentType == "video") {
                    // 清空视频包队列
                    videoQueue.empty();
                    videoQueue.enqueue(&seekPacket);
                    // 先重置时间戳
                    videoClk = 0;
                }
            }
            // 重置标志位
            isSeek = false;
        }

        if (currentType == "video") {
            if (videoQueue.queueSize() > 512) {
                // 若文件缓冲区已满则等待
                SDL_Delay(10);
                continue;
            }
        }

        /* judge haven't reall all frame */
        if (av_read_frame(pFormatCtx, packet) < 0){
            qDebug() << "Read file completed.";
            isReadFinished = true;
            emit readFinished();
            SDL_Delay(10);
            break;
        }

        if (packet->stream_index == videoIndex && currentType == "video") {
            videoQueue.enqueue(packet); // video stream
        } else if (packet->stream_index == audioIndex) {
            audioDecoder->packetEnqueue(packet); // audio stream
        } else if (packet->stream_index == subtitleIndex) {
//            subtitleQueue.enqueue(packet);
            av_packet_unref(packet);    // subtitle stream
        } else {
            av_packet_unref(packet);
        }
    }

//    qDebug() << isStop;
    while (!isStop) {
        /* just use at audio playing */
        if (isSeek) {
            goto seek;
        }

        SDL_Delay(100);
    }

fail:
    /* close audio device */
    if (audioIndex >= 0) {
        audioDecoder->closeAudio();
    }

    if (currentType == "video") {
        avcodec_close(pCodecCtx);
        avcodec_free_context(&pCodecCtx);
    }

    avformat_close_input(&pFormatCtx);
    avformat_free_context(pFormatCtx);

    isReadFinished = true;

    if (currentType == "music") {
        setPlayState(MainDecoder::STOP);
    }

    qDebug() << "Main decoder finished.";
}
