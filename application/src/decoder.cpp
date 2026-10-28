#include "decoder.h"
#include "audio_extent.h"
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <cstring>
#include <memory>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace {
struct DecodeResources {
    QFile file;
    QElapsedTimer readClock;
    qint64 readBytes = 0, readLimit = 0, logicalSize = 0;
    QString ioError;
    QElapsedTimer progressClock;
    std::function<void(qint64)> readProgress;
    AVIOContext *io = nullptr;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    ~DecodeResources() {
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};
int readAudio(void *opaque, uint8_t *data, int size) {
    auto *r = static_cast<DecodeResources *>(opaque);
    // Demuxers can request an entire large cover at once. Small partial reads
    // keep pacing, cancellation and actual-progress reporting responsive.
    const auto remaining = r->logicalSize - r->file.pos();
    if (remaining <= 0)
        return AVERROR_EOF;
    size = int(qMin(qint64(qMin(size, 65536)), remaining));
    if (size <= 0)
        return AVERROR_EOF;
    if (r->readLimit > 0) {
        // One small initial burst permits format probing; repeated seeks also consume budget.
        const auto due = qMax(qint64(0), r->readBytes + size - 262144) * 1000 / r->readLimit;
        const auto delay = due - r->readClock.elapsed();
        if (delay > 0)
            QThread::msleep(static_cast<unsigned long>(delay));
    }
    const auto read = r->file.read(reinterpret_cast<char *>(data), size);
    if (read < 0 || (read == 0 && r->file.pos() < r->logicalSize)) {
        r->ioError = read < 0 ? r->file.errorString() : QString("原文件在预期结尾之前停止返回数据");
        return AVERROR(EIO);
    }
    if (read > 0) {
        r->readBytes += read;
        if (r->readProgress && r->progressClock.elapsed() >= 500) {
            r->readProgress(r->readBytes);
            r->progressClock.restart();
        }
    }
    return read > 0 ? int(read) : read == 0 ? AVERROR_EOF : AVERROR(EIO);
}
int64_t seekAudio(void *opaque, int64_t offset, int whence) {
    auto *r = static_cast<DecodeResources *>(opaque);
    auto *file = &r->file;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE)
        return r->logicalSize;
    qint64 position = offset;
    if (whence == SEEK_CUR)
        position += file->pos();
    else if (whence == SEEK_END)
        position += r->logicalSize;
    else if (whence != SEEK_SET)
        return AVERROR(EINVAL);
    if (position < 0)
        return AVERROR(EINVAL);
    if (file->seek(position))
        return file->pos();
    r->ioError = file->errorString();
    return AVERROR(EIO);
}
template <typename T> T sampleAt(const uint8_t *data) {
    T value;
    std::memcpy(&value, data, sizeof(T));
    return value;
}
float sampleFloat(const uint8_t *data, AVSampleFormat format) {
    switch (format) {
    case AV_SAMPLE_FMT_U8:
        return (int(*data) - 128) / 128.0f;
    case AV_SAMPLE_FMT_S16:
        return sampleAt<int16_t>(data) / 32768.0f;
    case AV_SAMPLE_FMT_S32:
        return float(sampleAt<int32_t>(data) / 2147483648.0);
    case AV_SAMPLE_FMT_S64:
        return float(sampleAt<int64_t>(data) / 9223372036854775808.0);
    case AV_SAMPLE_FMT_FLT:
        return sampleAt<float>(data);
    case AV_SAMPLE_FMT_DBL:
        return float(sampleAt<double>(data));
    default:
        return 0;
    }
}

// FLAC defines its STREAMINFO MD5 over signed, little-endian, interleaved PCM
// at the encoded bit depth, without FFmpeg's S16/S32 left-alignment padding.
struct FlacIntegrity {
    qint64 expectedFrames = 0;
    int rate = 0, channels = 0, bits = 0;
    QByteArray expectedMd5;
    QCryptographicHash md5{QCryptographicHash::Md5};
    bool valid = false, hashable = true;

    void read(const AVCodecParameters *parameters) {
        if (parameters->codec_id != AV_CODEC_ID_FLAC || parameters->extradata_size != 34 ||
            !parameters->extradata)
            return;
        const auto *data = parameters->extradata;
        quint64 packed = 0;
        for (int i = 10; i < 18; ++i)
            packed = (packed << 8) | data[i];
        rate = int(packed >> 44);
        channels = int((packed >> 41) & 7) + 1;
        bits = int((packed >> 36) & 31) + 1;
        expectedFrames = qint64(packed & ((quint64(1) << 36) - 1));
        valid = rate > 0 && bits >= 4 && bits <= 32;
        bool present = false;
        for (int i = 18; i < 34; ++i)
            present |= data[i] != 0;
        if (present)
            expectedMd5 = QByteArray(reinterpret_cast<const char *>(data + 18), 16);
    }

    void append(const AVFrame *frame, AVSampleFormat format) {
        if (!valid || expectedMd5.isEmpty() || !hashable)
            return;
        const auto packed = av_get_packed_sample_fmt(format);
        const int storageBits = packed == AV_SAMPLE_FMT_S16 ? 16 : packed == AV_SAMPLE_FMT_S32 ? 32 : 0;
        if (storageBits < bits || frame->sample_rate != rate || frame->ch_layout.nb_channels != channels) {
            hashable = false;
            return;
        }
        const bool planar = av_sample_fmt_is_planar(format);
        const int sourceBytes = storageBits / 8, pcmBytes = (bits + 7) / 8;
        QByteArray pcm(qsizetype(frame->nb_samples) * channels * pcmBytes, Qt::Uninitialized);
        auto *destination = reinterpret_cast<unsigned char *>(pcm.data());
        for (int sample = 0; sample < frame->nb_samples; ++sample) {
            for (int channel = 0; channel < channels; ++channel) {
                const auto *plane = frame->extended_data[planar ? channel : 0];
                const auto offset = (planar ? sample : qsizetype(sample) * channels + channel) * sourceBytes;
                qint32 value = packed == AV_SAMPLE_FMT_S16 ? sampleAt<qint16>(plane + offset)
                                                           : sampleAt<qint32>(plane + offset);
                value >>= storageBits - bits;
                const auto encoded = quint32(value);
                for (int byte = 0; byte < pcmBytes; ++byte)
                    *destination++ = static_cast<unsigned char>(encoded >> (byte * 8));
            }
        }
        md5.addData(pcm);
    }

    void finish(qint64 frames, int decodedRate, int decodedChannels, int *damaged) {
        if (!valid)
            return;
        const bool countMatches = expectedFrames > 0 && frames == expectedFrames;
        const bool formatMatches = rate == decodedRate && channels == decodedChannels;
        const bool md5Present = !expectedMd5.isEmpty();
        const bool md5Matches = md5Present && hashable && md5.result() == expectedMd5;
        if (countMatches && formatMatches && md5Matches) {
            // Metadata/junk can cause a recoverable decoder warning after all original
            // samples were emitted. Only both independent integrity checks can dismiss it.
            *damaged = 0;
        } else if ((expectedFrames > 0 && !countMatches) || !formatMatches || (md5Present && !md5Matches)) {
            // Some truncated streams reach EOF without a decode error. They still cannot
            // be published as a complete waveform when declared audio is missing/changed.
            ++*damaged;
        }
    }
};
} // namespace

bool decodeAudioPeaks(const QString &path, WaveData *result, QString *error, int *damagedPackets,
                      const std::function<void(const WaveData &)> &preview, qint64 readBytesPerSecond,
                      const std::function<void(qint64)> &durationReady,
                      const std::function<void(qint64)> &readProgress, QString *failureKind,
                      QJsonObject *diagnostics) {
    if (diagnostics)
        *diagnostics = {};
    DecodeResources r;
    auto fail = [&](const QString &message, int code = 0, const QString &kind = QString("invalid_audio")) {
        const auto actualKind = code == AVERROR(EIO) || !r.ioError.isEmpty()
                                    ? QString("source_unavailable") : kind;
        if (failureKind)
            *failureKind = actualKind;
        if (error) {
            char detail[AV_ERROR_MAX_STRING_SIZE] = {};
            if (code < 0)
                av_strerror(code, detail, sizeof(detail));
            *error = message + (code < 0 ? QString("：") + QString::fromUtf8(detail) : QString());
        }
        if (diagnostics) {
            (*diagnostics)["stopKind"] = actualKind;
            (*diagnostics)["stopReason"] = error ? *error : message;
        }
        return false;
    };
    // Public structs are ABI-compatible within these library major versions.
    if (AV_VERSION_MAJOR(avcodec_version()) != LIBAVCODEC_VERSION_MAJOR ||
        AV_VERSION_MAJOR(avformat_version()) != LIBAVFORMAT_VERSION_MAJOR ||
        AV_VERSION_MAJOR(avutil_version()) != LIBAVUTIL_VERSION_MAJOR)
        return fail("音频组件版本不匹配，请重新解压完整软件包", 0, "resource");
    r.readLimit = qMax(qint64(0), readBytesPerSecond);
    r.readClock.start();
    r.progressClock.start();
    r.readProgress = readProgress;
    r.file.setFileName(path);
    if (!r.file.open(QIODevice::ReadOnly))
        return fail("原音乐无法读取：" + r.file.errorString(), 0, "source_unavailable");
    r.logicalSize = r.file.size();
    r.logicalSize = AudioExtent::mp3End(r.logicalSize, [&r](qint64 offset, int count) {
        if (!r.file.seek(offset))
            return QByteArray();
        QByteArray bytes(count, Qt::Uninitialized);
        int completed = 0;
        while (completed < count) {
            const auto read =
                readAudio(&r, reinterpret_cast<uint8_t *>(bytes.data()) + completed, count - completed);
            if (read < 0)
                return QByteArray();
            completed += read;
        }
        return bytes;
    });
    if (!r.file.seek(0))
        return fail("原音乐无法重新定位", AVERROR(EIO));
    auto *buffer = static_cast<unsigned char *>(av_malloc(65536));
    if (!buffer)
        return fail("音频缓冲分配失败", 0, "resource");
    r.io = avio_alloc_context(buffer, 65536, 0, &r, readAudio, nullptr, seekAudio);
    if (!r.io) {
        av_free(buffer);
        return fail("音频读取器初始化失败", 0, "resource");
    }
    r.format = avformat_alloc_context();
    if (!r.format)
        return fail("音频格式分配失败", 0, "resource");
    r.format->pb = r.io;
    r.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    // The product indexes audio files, not playlists or external network URLs.
    AVDictionary *options = nullptr;
    av_dict_set(&options, "format_whitelist", "wav,mp3,flac,aiff,mov,aac,ogg,asf", 0);
    av_dict_set(&options, "protocol_whitelist", "", 0);
    int status = avformat_open_input(&r.format, nullptr, nullptr, &options);
    av_dict_free(&options);
    if (status < 0)
        return fail("无法识别音频文件", status);
    if ((status = avformat_find_stream_info(r.format, nullptr)) < 0)
        return fail("无法读取音频信息", status);
    const AVCodec *decoder = nullptr;
    const int audioStream = av_find_best_stream(r.format, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (audioStream < 0 || !decoder)
        return fail("文件中没有受支持的音频轨道", audioStream,
                    audioStream == AVERROR_DECODER_NOT_FOUND ? "unsupported_format" : "invalid_audio");
    qint64 declaredMs = 0;
    {
        const auto *stream = r.format->streams[audioStream];
        qint64 milliseconds = 0;
        if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE)
            milliseconds = av_rescale_q(stream->duration, stream->time_base, AVRational{1, 1000});
        else if (r.format->duration > 0 && r.format->duration != AV_NOPTS_VALUE)
            milliseconds = r.format->duration / 1000;
        declaredMs = milliseconds;
        if (durationReady)
            durationReady(milliseconds);
    }
    // Attached pictures and video must never participate in waveform completion.
    for (unsigned i = 0; i < r.format->nb_streams; ++i)
        r.format->streams[i]->discard = int(i) == audioStream ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    r.codec = avcodec_alloc_context3(decoder);
    if (!r.codec)
        return fail("音频解码器分配失败", 0, "resource");
    if ((status = avcodec_parameters_to_context(r.codec, r.format->streams[audioStream]->codecpar)) < 0)
        return fail("音频参数无效", status);
    FlacIntegrity integrity;
    integrity.read(r.format->streams[audioStream]->codecpar);
    if (decoder->id == AV_CODEC_ID_FLAC && (!integrity.valid || integrity.expectedMd5.isEmpty())) {
        // Without a complete-stream digest, a bad frame CRC is the only integrity
        // evidence for some otherwise decodable PCM mutations. FFmpeg merely logs
        // CRC failures without EXPLODE. Keep MD5-backed streams recoverable instead.
        r.codec->err_recognition |= AV_EF_CRCCHECK | AV_EF_EXPLODE;
    }
    r.codec->thread_count = 1;
    if ((status = avcodec_open2(r.codec, decoder, nullptr)) < 0)
        return fail("无法启动音频解码器", status);
    r.packet = av_packet_alloc();
    r.frame = av_frame_alloc();
    if (!r.packet || !r.frame)
        return fail("音频帧分配失败", 0, "resource");
    std::unique_ptr<PeakBuilder> builder;
    QVector<float> samples;
    int rate = 0, channels = 0;
    qint64 totalFrames = 0;
    int damaged = 0;
    int skippedPackets = 0, flaggedFrames = 0, demuxErrors = 0;
    bool reachedEnd = false, stopped = false;
    qint64 firstIssueAtMs = -1;
    auto issue = [&] {
        ++damaged;
        if (firstIssueAtMs < 0)
            firstIssueAtMs = rate > 0 ? totalFrames * 1000 / rate : 0;
    };
    QElapsedTimer sincePreview;
    sincePreview.start();
    auto receive = [&]() -> bool {
        int consecutiveErrors = 0;
        while (true) {
            const int received = avcodec_receive_frame(r.codec, r.frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received == AVERROR_INVALIDDATA) {
                issue();
                // Bound a decoder that repeatedly returns an error without producing
                // output. Errors on independent packets do not consume this budget.
                if (++consecutiveErrors <= 64)
                    continue;
                return fail("解码器连续未取得进展，已停止当前文件", received);
            }
            if (received < 0)
                return fail("音频数据解码失败", received);
            consecutiveErrors = 0;
            const int frameChannels = r.frame->ch_layout.nb_channels;
            if ((r.frame->flags & AV_FRAME_FLAG_CORRUPT) || r.frame->decode_error_flags) {
                issue();
                ++flaggedFrames;
            }
            const auto format = static_cast<AVSampleFormat>(r.frame->format);
            const auto packed = av_get_packed_sample_fmt(format);
            const int bytes = av_get_bytes_per_sample(format);
            if (frameChannels < 1 || frameChannels > 32 || r.frame->sample_rate < 1000 ||
                r.frame->sample_rate > 768000 || r.frame->nb_samples < 0 || r.frame->nb_samples > 1048576 ||
                bytes <= 0 || packed == AV_SAMPLE_FMT_NONE)
                return fail("音频采样格式超出支持范围");
            if (!builder) {
                rate = r.frame->sample_rate;
                channels = frameChannels;
                builder = std::make_unique<PeakBuilder>(rate, channels);
            } else if (rate != r.frame->sample_rate || channels != frameChannels) {
                return fail("音频中途改变采样格式，后续部分未继续解码");
            }
            if (totalFrames + r.frame->nb_samples > qint64(rate) * 86400 * 7)
                return fail("音频超过七天，后续部分未继续解码");
            const bool planar = av_sample_fmt_is_planar(format);
            // Convert one codec frame at a time, preserving every channel independently.
            samples.resize(qsizetype(r.frame->nb_samples) * channels);
            for (int c = 0; c < channels; ++c) {
                const auto *plane = r.frame->extended_data[planar ? c : 0];
                if (!plane)
                    return fail("音频采样数据缺失");
                for (int f = 0; f < r.frame->nb_samples; ++f) {
                    const qsizetype offset = (planar ? f : qsizetype(f) * channels + c) * bytes;
                    samples[qsizetype(f) * channels + c] = sampleFloat(plane + offset, packed);
                }
            }
            builder->append(samples.constData(), r.frame->nb_samples);
            totalFrames += r.frame->nb_samples;
            integrity.append(r.frame, format);
            av_frame_unref(r.frame);
            if (preview && sincePreview.elapsed() >= 250) {
                preview(builder->finish());
                sincePreview.restart();
            }
        }
    };
    int stagnantReads = 0;
    while (true) {
        const auto position = avio_tell(r.io);
        status = av_read_frame(r.format, r.packet);
        if (status == AVERROR_EOF) {
            reachedEnd = true;
            break;
        }
        if (status == AVERROR_INVALIDDATA && r.ioError.isEmpty()) {
            issue();
            ++demuxErrors;
            stagnantReads = avio_tell(r.io) > position ? 0 : stagnantReads + 1;
            av_packet_unref(r.packet);
            if (stagnantReads < 8)
                continue;
        }
        if (status < 0) {
            fail(r.ioError.isEmpty() ? "音频容器解析中断" : "原音乐读取中断：" + r.ioError,
                 status, r.ioError.isEmpty() ? "invalid_audio" : "source_unavailable");
            stopped = true;
            break;
        }
        stagnantReads = 0;
        if (r.packet->stream_index == audioStream) {
            if (r.packet->flags & AV_PKT_FLAG_CORRUPT)
                issue();
            int sent = avcodec_send_packet(r.codec, r.packet);
            if (sent == AVERROR(EAGAIN)) {
                if (!receive()) {
                    stopped = true;
                    break;
                }
                sent = avcodec_send_packet(r.codec, r.packet);
            }
            if (sent == AVERROR_INVALIDDATA) {
                issue();
                ++skippedPackets;
                av_packet_unref(r.packet);
                continue;
            }
            if (sent < 0 || !receive()) {
                if (sent < 0)
                    fail("音频数据包处理停止", sent);
                stopped = true;
                break;
            }
        }
        av_packet_unref(r.packet);
    }
    if (!stopped) {
        status = avcodec_send_packet(r.codec, nullptr);
        if (status == AVERROR(EAGAIN) && receive())
            status = avcodec_send_packet(r.codec, nullptr);
        if (status == AVERROR_INVALIDDATA) {
            issue();
        } else if (status < 0 && status != AVERROR_EOF) {
            fail("音频解码收尾未完成", status);
            stopped = true;
        }
        if (!stopped && !receive())
            stopped = true;
    }
    if (!r.ioError.isEmpty()) {
        fail("原音乐读取中断：" + r.ioError, AVERROR(EIO), "source_unavailable");
        reachedEnd = false;
        stopped = true;
    }
    integrity.finish(totalFrames, rate, channels, &damaged);
    if (stopped)
        ++damaged;
    if (diagnostics) {
        (*diagnostics)["decodedMs"] = rate > 0 ? totalFrames * 1000 / rate : 0;
        (*diagnostics)["declaredMs"] = declaredMs;
        (*diagnostics)["reachedEnd"] = reachedEnd;
        (*diagnostics)["stoppedEarly"] = stopped;
        (*diagnostics)["issues"] = damaged;
        (*diagnostics)["firstIssueAfterDecodedMs"] = firstIssueAtMs;
        (*diagnostics)["skippedPackets"] = skippedPackets;
        (*diagnostics)["flaggedFrames"] = flaggedFrames;
        (*diagnostics)["demuxErrors"] = demuxErrors;
        (*diagnostics)["state"] = totalFrames == 0 ? "no_audio" : stopped ? "partial" :
                                   damaged ? "decoded_with_warnings" : "decoded";
    }
    if (!builder || totalFrames == 0) {
        if (stopped)
            return false;
        return fail("本次解码未取得有效音频；不代表其他播放器一定无法播放");
    }
    *result = builder->finish();
    if (damagedPackets)
        *damagedPackets = damaged;
    return true;
}
