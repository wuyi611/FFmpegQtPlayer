#include <QDebug>

#include "audiodecoder.h"

/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

AudioDecoder::AudioDecoder(QObject *parent) :
    QObject(parent),
    isStop(false),
    isPause(false),
    isreadFinished(false),
    totalTime(0),
    clock(0),
    volume(SDL_MIX_MAXVOLUME),
    audioDeviceFormat(AUDIO_F32SYS),
    aCovertCtx(NULL),
    sendReturn(0)
{

}

/**
 * @brief 初始化并打开音频设备
 * @param pFormatCtx 封装格式上下文，用于获取流信息
 * @param index 音频流的索引
 * @return 0 成功，-1 失败
 */
int AudioDecoder::openAudio(AVFormatContext *pFormatCtx, int index)
{
    AVCodec *codec;
    SDL_AudioSpec wantedSpec; // 我们期望的硬件参数
    int wantedNbChannels;
    const char *env;

    /* 降级备选方案：当硬件不支持原始参数时，按此数组顺序尝试降低通道数和采样率 */
    int nextNbChannels[]   = {0, 0, 1, 6, 2, 6, 4, 6};
    int nextSampleRates[]  = {0, 44100, 48000, 96000, 192000};
    int nextSampleRateIdx = FF_ARRAY_ELEMS(nextSampleRates) - 1;

    // 重置播放控制状态
    isStop = false;
    isPause = false;
    isreadFinished = false;

    // 重置重采样源参数（由解码出的数据包决定）
    audioSrcFmt = AV_SAMPLE_FMT_NONE;
    audioSrcChannelLayout = 0;
    audioSrcFreq = 0;

    // 允许该流被正常处理
    pFormatCtx->streams[index]->discard = AVDISCARD_DEFAULT;
    stream = pFormatCtx->streams[index];

    // 1. 初始化解码器上下文并拷贝流参数
    codecCtx = avcodec_alloc_context3(NULL);
    avcodec_parameters_to_context(codecCtx, pFormatCtx->streams[index]->codecpar);

    // 2. 查找并打开对应的音频解码器（如 AAC, MP3 等）
    if ((codec = avcodec_find_decoder(codecCtx->codec_id)) == NULL) {
        avcodec_free_context(&codecCtx);
        qDebug() << "Audio decoder not found.";
        return -1;
    }

    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        avcodec_free_context(&codecCtx);
        qDebug() << "Could not open audio decoder.";
        return -1;
    }

    totalTime = pFormatCtx->duration;

    wantedNbChannels = codecCtx->channels;

    // 3. 确定声道布局：优先检查环境变量设置，否则使用解码器默认值
    env = SDL_getenv("SDL_AUDIO_CHANNELS");
    if (env) {
        wantedNbChannels = atoi(env);
        audioDstChannelLayout = av_get_default_channel_layout(wantedNbChannels);
    }

    if (!audioDstChannelLayout ||
        (wantedNbChannels != av_get_channel_layout_nb_channels(audioDstChannelLayout))) {
        audioDstChannelLayout = av_get_default_channel_layout(wantedNbChannels);
        audioDstChannelLayout &= ~AV_CH_LAYOUT_STEREO_DOWNMIX; // 排除特殊的混音布局
    }

    // 4. 配置 SDL 音频参数
    wantedSpec.channels    = av_get_channel_layout_nb_channels(audioDstChannelLayout);
    wantedSpec.freq        = codecCtx->sample_rate;
    if (wantedSpec.freq <= 0 || wantedSpec.channels <= 0) {
        avcodec_free_context(&codecCtx);
        return -1;
    }

    // 寻找最接近且不高于原始频率的备选采样率
    while (nextSampleRateIdx && nextSampleRates[nextSampleRateIdx] >= wantedSpec.freq) {
        nextSampleRateIdx--;
    }

    wantedSpec.format      = audioDeviceFormat; // 目标采样格式（如 S16）
    wantedSpec.silence     = 0;
    // 计算合理的缓冲区大小：FFMAX(最小尺寸, 根据采样率计算的动态尺寸)
    wantedSpec.samples      = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(wantedSpec.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));

    // 【核心关键】：绑定 SDL 的回调函数，SDL 会在需要音频数据时自动调用它
    wantedSpec.callback    = &AudioDecoder::audioCallback;
    wantedSpec.userdata    = this; // 将当前类指针传入回调，以便访问内部成员

    // 5. 【硬件协商循环】：如果声卡不支持当前参数，则不断尝试降低规格
    while (1) {
        while (SDL_OpenAudio(&wantedSpec, &spec) < 0) {
            // 打开失败，尝试切换到下一个声道方案

            // int nextNbChannels[]   = {0, 0, 1, 6, 2, 6, 4, 6};
            // 如果声道数为3，5，7等奇数非标准声道数，则直接跳到6声道，紧接着开始6，4，2挨个试
            // 如果声道数为2则尝试单声道，若单声道也失败则直接降低采样率
            wantedSpec.channels = nextNbChannels[FFMIN(7, wantedSpec.channels)];
            if (!wantedSpec.channels) {
                // 如果声道降到底了还没成功，则降低采样率重新尝试
                wantedSpec.freq = nextSampleRates[nextSampleRateIdx--];
                // 声道数恢复为最初想要的状态
                wantedSpec.channels = wantedNbChannels;
                if (!wantedSpec.freq) {
                    // 如果采样率也试完了，说明所有组合都试过了
                    avcodec_free_context(&codecCtx);
                    qDebug() << "No more combinations to try, audio open failed";
                    return -1;
                }
            }
            // 再次获取声道布局
            audioDstChannelLayout = av_get_default_channel_layout(wantedSpec.channels);
        }

        // 检查 SDL 实际给予的格式是否与我们要求的一致
        if (spec.format != audioDeviceFormat) {
            wantedSpec.format = spec.format;
            audioDeviceFormat = spec.format;
            SDL_CloseAudio(); // 格式不符，关闭后按建议格式重开
        } else {
            break; // 格式匹配，成功打开设备
        }
    }

    // 记录最终硬件确定的声道布局（即使打开成功，声道数也不一定和我们期望的一样）
    if (spec.channels != wantedSpec.channels) {
        audioDstChannelLayout = av_get_default_channel_layout(spec.channels);
    }

    // 6. 映射 SDL 格式到 FFmpeg 采样格式，为后续重采样（Resample）做准备
    switch (audioDeviceFormat) {
    case AUDIO_U8:     audioDstFmt = AV_SAMPLE_FMT_U8;  audioDepth = 1; break;
    case AUDIO_S16SYS: audioDstFmt = AV_SAMPLE_FMT_S16; audioDepth = 2; break;
    case AUDIO_S32SYS: audioDstFmt = AV_SAMPLE_FMT_S32; audioDepth = 4; break;
    case AUDIO_F32SYS: audioDstFmt = AV_SAMPLE_FMT_FLT; audioDepth = 4; break;
    default:           audioDstFmt = AV_SAMPLE_FMT_S16; audioDepth = 2; break;
    }

    // 7. 取消静音，音频设备正式开始工作（触发 Callback）
    SDL_PauseAudio(0);

    return 0;
}

// 关闭音频解码
void AudioDecoder::closeAudio()
{
    emptyAudioData();

    SDL_LockAudio();
    SDL_CloseAudio();
    SDL_UnlockAudio();

    avcodec_close(codecCtx);
    avcodec_free_context(&codecCtx);
}

// 文件读取完成
void AudioDecoder::readFileFinished()
{
    isreadFinished = true;
}

// 暂停/开始播放
void AudioDecoder::pauseAudio(bool pause)
{
    isPause = pause;
}

// 停止播放
void AudioDecoder::stopAudio()
{
    isStop = true;
}

// 原始帧入队
void AudioDecoder::packetEnqueue(AVPacket *packet)
{
    packetQueue.enqueue(packet);
}

// 清空音频缓存
void AudioDecoder::emptyAudioData()
{
    audioBuf = nullptr;

    audioBufIndex = 0;
    audioBufSize = 0;
    audioBufSize1 = 0;

    clock = 0;

    sendReturn = 0;

    packetQueue.empty();

    isreadFinished = false;
}

void AudioDecoder::setClock(double clk)
{
    clock = clk;
}

int AudioDecoder::getVolume()
{
    return volume;
}

void AudioDecoder::setVolume(int volume)
{
    this->volume = volume;
}

double AudioDecoder::getAudioClock()
{
    if (codecCtx) {
        /* control audio pts according to audio buffer data size */
        // 缓冲区中还没播放的数据量（字节）
        int hwBufSize   = audioBufSize - audioBufIndex;
        // 每秒消耗的字节数（采样率×通道数×位深）
        int bytesPerSec = codecCtx->sample_rate * codecCtx->channels * audioDepth;
        // 因为clock是缓冲区中全部数据最后的时间，部分数据还没播放，所以需要减去剩下数据播放所需的时间
        clock -= static_cast<double>(hwBufSize) / bytesPerSec;

    }

    return clock;
}

/**
 * @brief  SDL 音频播放回调函数。
 * @note   此函数由 SDL 内部音频线程异步定时调用。其核心任务是“填满” SDL 提供的缓冲区。
 * 如果解码后的数据多于请求量，则缓存剩余部分；如果少于请求量，则触发解码流程。
 * * @param  userdata  指向 AudioDecoder 实例的指针（在 SDL_OpenAudio 中通过 wantedSpec.userdata 传入）。
 * @param  stream    SDL 硬件缓冲区的起始地址，用于接收 PCM 数据。
 * @param  len       SDL 本次请求填充的字节总数。
 * * @attention 1. 此函数运行在实时音频线程，严禁执行任何阻塞操作（如 I/O、大量 Log、复杂锁申请）。
 * 2. 必须处理解码失败或数据不足的情况，此时应填充静音数据以防产生爆音。
 */
void AudioDecoder::audioCallback(void *userdata, quint8 *stream, int SDL_AudioBufSize)
{
    AudioDecoder *decoder = (AudioDecoder *)userdata;

    // 解码数据大小
    int decodedSize;
    /* SDL_BufSize means audio play buffer left size
     * while it greater than 0, means counld fill data to it
     */
    while (SDL_AudioBufSize > 0) {
        if (decoder->isStop) {
            return ;
        }

        if (decoder->isPause) {
            SDL_Delay(10);
            continue;
        }

        /* no data in buffer */
        // 如果当前索引 (audioBufIndex) 大于等于缓冲区总大小，说明上一帧数据已播完
        if (decoder->audioBufIndex >= decoder->audioBufSize) {

            // 解码
            decodedSize = decoder->decodeAudio();
            /* if error, just output silence */
            if (decodedSize < 0) {
                // 解码失败或没有数据：生成 1024 字节的伪缓冲区并设为 nullptr
                // 这将导致下方逻辑输出“静音”，防止声卡发出爆鸣声
                decoder->audioBufSize = 1024;
                decoder->audioBuf = nullptr;
            } else {
                decoder->audioBufSize = decodedSize;
            }
            // 重置读取进度索引
            decoder->audioBufIndex = 0;
        }

        /* calculate number of data that haven't play */
        // 暂存播放区里还没有播放的数据大小
        int left = decoder->audioBufSize - decoder->audioBufIndex;
        // 如果库存比 SDL 想要的多，则只搬运 SDL 想要的大小
        if (left > SDL_AudioBufSize) {
            left = SDL_AudioBufSize;
        }

        if (decoder->audioBuf) {
            // 先清空目标区域（stream 是待填充的原始硬件内存）
            memset(stream, 0, left);
            // 使用 SDL_MixAudio 进行混音并应用音量控制 (decoder->volume)
            // 相比 memcpy，这能提供更平滑的音量缩放，避免直接修改原始 PCM 导致爆音
            SDL_MixAudio(stream, decoder->audioBuf + decoder->audioBufIndex, left, decoder->volume);
        } else {
            // 明确告诉硬件：这一段没数据，请保持安静
            memset(stream, 0, left);
        }

        SDL_AudioBufSize -= left;
        stream += left;
        decoder->audioBufIndex += left;
    }
}

int AudioDecoder::decodeAudio()
{
    int ret;
    AVFrame *frame = av_frame_alloc();
    int resampledDataSize;

    if (!frame) {
        qDebug() << "Decode audio frame alloc failed.";
        return -1;
    }

    if (isStop) {
        return -1;
    }

    if (packetQueue.queueSize() <= 0) {
        if (isreadFinished) {
            // 队列中没有帧且文件已读取完则停止
            isStop = true;
            SDL_Delay(100);
            // 通知解码器音频已播放完
            emit playFinished();
        }
        // 没有数据直接返回
        return -1;
    }

    /* get new packet whiel last packet all has been resolved */
    if (sendReturn != AVERROR(EAGAIN)) {
        // 如果解码缓冲区没有满则取出一个packet
        packetQueue.dequeue(&packet, true);
    }

    if (packet.size == 5 && memcmp(packet.data, "FLUSH", 5) == 0) {
        // 清空缓冲区
        avcodec_flush_buffers(codecCtx);
        av_packet_unref(&packet);
        av_frame_free(&frame);
        sendReturn = 0;
        qDebug() << "seek audio";
        return -1;
    }

    /* while return -11 means packet have data not resolved,
     * this packet cannot be unref
     */
    sendReturn = avcodec_send_packet(codecCtx, &packet);
    if ((sendReturn < 0) && (sendReturn != AVERROR(EAGAIN)) && (sendReturn != AVERROR_EOF)) {
        av_packet_unref(&packet);
        av_frame_free(&frame);
        qDebug() << "Audio send to decoder failed, error code: " << sendReturn;
        return sendReturn;
    }

    ret = avcodec_receive_frame(codecCtx, frame);
    if ((ret < 0) && (ret != AVERROR(EAGAIN))) {
        av_packet_unref(&packet);
        av_frame_free(&frame);
        qDebug() << "Audio frame decode failed, error code: " << ret;
        return ret;
    }

    if (frame->pts != AV_NOPTS_VALUE) {
        // 如果时间戳有效
        // 转为秒数
        clock = av_q2d(stream->time_base) * frame->pts;
//        qDebug() << "no pts";
    }

    /* get audio channels */
    // 如果原始数据里有明确的声道信息，就用原有的；如果没有，就根据声道数量强行推算一个。
    qint64 inChannelLayout = (frame->channel_layout && frame->channels == av_get_channel_layout_nb_channels(frame->channel_layout)) ?
                frame->channel_layout : av_get_default_channel_layout(frame->channels);

    // 开始音频重采样

    if (frame->format       != audioSrcFmt              ||
        inChannelLayout     != audioSrcChannelLayout    ||
        frame->sample_rate  != audioSrcFreq             ||
        !aCovertCtx) {
        // aCovertCtx初始化检查，第一次必须先初始化
        if (aCovertCtx) {
            // 若不是第一次则释放旧的上下文
            swr_free(&aCovertCtx);
        }

        /* init swr audio convert context */
        // 初始化音频重采样器，设置配置
        aCovertCtx = swr_alloc_set_opts(nullptr, audioDstChannelLayout, audioDstFmt, spec.freq,
                inChannelLayout, (AVSampleFormat)frame->format , frame->sample_rate, 0, NULL);
        // 启动重采样，激活配置
        if (!aCovertCtx || (swr_init(aCovertCtx) < 0)) {
            av_packet_unref(&packet);
            av_frame_free(&frame);
            return -1;
        }

        // 保存当前参数
        audioSrcFmt             = (AVSampleFormat)frame->format;
        audioSrcChannelLayout   = inChannelLayout;
        audioSrcFreq            = frame->sample_rate;
        audioSrcChannels        = frame->channels;
    }

    if (aCovertCtx) {
        // 解码器输出的原始数据
        const quint8 **in   = (const quint8 **)frame->extended_data;
        // 目标缓冲区
        uint8_t *out[] = {audioBuf1};
        // 目标缓冲区能容纳的最大样本数
        int outCount = sizeof(audioBuf1) / spec.channels / av_get_bytes_per_sample(audioDstFmt);
        // 重采样执行（实际转换出来的每声道样本数。）
        int sampleSize = swr_convert(aCovertCtx, out, outCount, in, frame->nb_samples);
        if (sampleSize < 0) {
            ///qDebug() << "swr convert failed";
            av_packet_unref(&packet);
            av_frame_free(&frame);
            return -1;
        }

        if (sampleSize == outCount) {
            // 可能还有剩余的数据留在 SwrContext 内部没吐出来
            qDebug() << "audio buffer is probably too small";
            // 尝试重新 swr_init。虽然这能清空内部缓存，但可能会导致极短的音频丢失（跳音）
            if (swr_init(aCovertCtx) < 0) {
                swr_free(&aCovertCtx);
            }
        }

        audioBuf = audioBuf1;

        resampledDataSize = sampleSize * spec.channels * av_get_bytes_per_sample(audioDstFmt);
    } else {
        // 如果格式一致，直接让指针指向解码帧的数据，避免一次内存拷贝
        audioBuf = frame->data[0];
        // 当前帧数据的总字节数
        resampledDataSize = av_samples_get_buffer_size(NULL, frame->channels, frame->nb_samples, static_cast<AVSampleFormat>(frame->format), 1);
    }

    // 播放时钟更新
    clock += static_cast<double>(resampledDataSize) / (audioDepth * codecCtx->channels * codecCtx->sample_rate);

    if (sendReturn != AVERROR(EAGAIN)) {
        av_packet_unref(&packet);
    }

    av_frame_free(&frame);

    return resampledDataSize;
}
