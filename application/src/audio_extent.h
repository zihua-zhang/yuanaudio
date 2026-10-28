#pragma once
#include <QByteArray>
#include <QSet>
#include <QtEndian>
#include <functional>

// Recognize complete trailing tags before MP3 demuxing. This is a logical read
// boundary only: the source and its fingerprint are never edited. Unknown bytes,
// oversized tags and malformed lengths keep the original boundary.
namespace AudioExtent {
using Reader = std::function<QByteArray(qint64, int)>;
struct MpegFrame {
    int bytes = 0, version = 0, rate = 0;
};
inline MpegFrame frame(const QByteArray &bytes) {
    if (bytes.size() != 4)
        return {};
    const auto h = qFromBigEndian<quint32>(bytes.constData());
    const int version = (h >> 19) & 3, layer = (h >> 17) & 3;
    const int bitrate = (h >> 12) & 15, rateIndex = (h >> 10) & 3;
    if ((h & 0xffe00000) != 0xffe00000 || version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 ||
        rateIndex == 3 || (h & 3) == 2)
        return {};
    static constexpr int rates[] = {44100, 48000, 32000};
    static constexpr int high[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static constexpr int low[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const int rate = rates[rateIndex] / (version == 3 ? 1 : version == 2 ? 2 : 4);
    return {(version == 3 ? 144000 * high[bitrate] : 72000 * low[bitrate]) / rate + int((h >> 9) & 1),
            version, rate};
}
inline int decimal(const QByteArray &value) {
    int result = 0;
    if (value.isEmpty() || value.size() > 6)
        return -1;
    for (const auto ch : value) {
        if (ch < '0' || ch > '9')
            return -1;
        result = result * 10 + ch - '0';
    }
    return result;
}
inline bool apeItems(const QByteArray &data, quint32 count) {
    if (count > 4096)
        return false;
    qsizetype at = 0;
    QSet<QByteArray> names;
    for (quint32 item = 0; item < count; ++item) {
        if (data.size() - at < 11)
            return false;
        const auto size = qFromLittleEndian<quint32>(data.constData() + at);
        const auto flags = qFromLittleEndian<quint32>(data.constData() + at + 4);
        if ((flags & ~quint32(7)) || (flags & 6) == 6)
            return false;
        at += 8;
        const auto end = data.indexOf('\0', at);
        if (end < at + 2 || end > at + 255)
            return false;
        const auto name = data.mid(at, end - at).toLower();
        for (const auto ch : name)
            if (ch < 0x20 || ch > 0x7e)
                return false;
        if (names.contains(name))
            return false;
        names.insert(name);
        at = end + 1;
        if (size > quint64(data.size() - at))
            return false;
        at += size;
    }
    return at == data.size();
}
inline bool lyricsItems(const QByteArray &data) {
    if (!data.startsWith("LYRICSBEGIN"))
        return false;
    qsizetype at = 11;
    while (at < data.size()) {
        if (data.size() - at < 8)
            return false;
        for (int i = 0; i < 3; ++i)
            if (data[at + i] < 'A' || data[at + i] > 'Z')
                return false;
        const auto size = decimal(data.mid(at + 3, 5));
        at += 8;
        if (size < 0 || size > data.size() - at)
            return false;
        at += size;
    }
    return at == data.size();
}
inline qint64 mp3End(qint64 fileSize, const Reader &reader) {
    constexpr qint64 budget = 1024 * 1024;
    qint64 used = 0;
    auto read = [&](qint64 at, int count) -> QByteArray {
        if (at < 0 || count < 0 || at > fileSize - count || used + count > budget)
            return {};
        used += count;
        const auto bytes = reader(at, count);
        return bytes.size() == count ? bytes : QByteArray();
    };
    qint64 start = 0;
    const auto head = read(0, 10);
    if (head.startsWith("ID3")) {
        const auto version = quint8(head[3]);
        if (version < 2 || version > 4 || quint8(head[4]) == 255)
            return fileSize;
        quint32 size = 0;
        for (int i = 6; i < 10; ++i) {
            if (quint8(head[i]) & 0x80)
                return fileSize;
            size = (size << 7) | quint8(head[i]);
        }
        start = 10 + qint64(size) + (version == 4 && (quint8(head[5]) & 0x10) ? 10 : 0);
    }
    // Two consecutive MPEG Layer III frames distinguish MP3 from containers,
    // FLAC with ID3 wrappers, and misleading file extensions.
    const auto first = frame(read(start, 4));
    const auto second = frame(read(start + first.bytes, 4));
    if (!first.bytes || !second.bytes || first.version != second.version || first.rate != second.rate)
        return fileSize;
    const qint64 minimumEnd = start + first.bytes + second.bytes;
    if (minimumEnd > fileSize)
        return fileSize;
    auto frameBoundary = [&](qint64 candidate) {
        const auto at = qMax(start, candidate - 8192);
        const auto tail = read(at, int(candidate - at));
        if (tail.size() != candidate - at)
            return false;
        // A tag-shaped sequence inside the final MPEG frame is still audio data.
        // Require a chain of at least two compatible complete frames ending exactly
        // at the proposed tag start. Ambiguous chains crossing that position mean
        // tag-shaped bytes could be audio: retain the original extent in that case.
        // This adds at most 8 KiB overall, without a full scan or many NAS seeks.
        bool endsAtBoundary = false, crossesBoundary = false;
        for (qsizetype offset = 0; offset + 4 <= tail.size(); ++offset) {
            qsizetype cursor = offset;
            int count = 0;
            MpegFrame chain;
            while (cursor + 4 <= tail.size()) {
                const auto parsed = frame(tail.mid(cursor, 4));
                if (!parsed.bytes ||
                    (count && (parsed.version != chain.version || parsed.rate != chain.rate)))
                    break;
                if (!count)
                    chain = parsed;
                cursor += parsed.bytes;
                ++count;
                if (count >= 2) {
                    endsAtBoundary |=
                        cursor == tail.size() && chain.version == first.version && chain.rate == first.rate;
                    crossesBoundary |= cursor > tail.size();
                }
            }
        }
        return endsAtBoundary && !crossesBoundary;
    };
    qint64 end = fileSize;
    for (int pass = 0; pass < 8; ++pass) {
        if (end - minimumEnd >= 128 && read(end - 128, 3) == "TAG") {
            end -= 128;
            continue;
        }
        const auto footer = read(end - 32, 32);
        if (footer.startsWith("APETAGEX")) {
            const auto version = qFromLittleEndian<quint32>(footer.constData() + 8);
            const auto size = qFromLittleEndian<quint32>(footer.constData() + 12);
            const auto count = qFromLittleEndian<quint32>(footer.constData() + 16);
            const auto flags = qFromLittleEndian<quint32>(footer.constData() + 20);
            if (version != 2000 || size < 32 || size > budget || (flags & ~quint32(0x80000001)) ||
                footer.mid(24) != QByteArray(8, '\0') || end - size < minimumEnd)
                break;
            qint64 tagStart = end - size;
            const auto data = read(tagStart, int(size) - 32);
            if (data.size() != int(size) - 32 || !apeItems(data, count))
                break;
            if (flags & 0x80000000) {
                if (tagStart - 32 < minimumEnd)
                    break;
                const auto header = read(tagStart - 32, 32);
                auto expected = footer;
                qToLittleEndian<quint32>(flags | 0x20000000, expected.data() + 20);
                if (header != expected)
                    break;
                tagStart -= 32;
            }
            end = tagStart;
            continue;
        }
        const auto lyrics = read(end - 15, 15);
        if (lyrics.endsWith("LYRICS200")) {
            const auto size = decimal(lyrics.first(6));
            if (size < 11 || end - 15 - size < minimumEnd)
                break;
            const auto data = read(end - 15 - size, size);
            if (data.size() != size || !lyricsItems(data))
                break;
            end -= 15 + size;
            continue;
        }
        break;
    }
    return end < fileSize && frameBoundary(end) ? end : fileSize;
}
} // namespace AudioExtent
