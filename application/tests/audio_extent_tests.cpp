#include "audio_extent.h"
#include <QtTest>

class AudioExtentTests : public QObject {
    Q_OBJECT
    static QByteArray audio() {
        QByteArray frame = QByteArray::fromHex("fffb9000") + QByteArray(413, '\x55');
        return frame + frame + frame;
    }
    static QByteArray ape(const QByteArray &value = "example", bool header = false) {
        QByteArray item(8, '\0');
        qToLittleEndian<quint32>(value.size(), item.data());
        item += "Title";
        item += '\0';
        item += value;
        QByteArray footer = QByteArray("APETAGEX") + QByteArray(24, '\0');
        qToLittleEndian<quint32>(2000, footer.data() + 8);
        qToLittleEndian<quint32>(item.size() + 32, footer.data() + 12);
        qToLittleEndian<quint32>(1, footer.data() + 16);
        if (!header)
            return item + footer;
        qToLittleEndian<quint32>(0x80000000, footer.data() + 20);
        auto start = footer;
        qToLittleEndian<quint32>(0xa0000000, start.data() + 20);
        return start + item + footer;
    }
    static qint64 end(const QByteArray &bytes, qint64 *readCount = nullptr) {
        qint64 reads = 0;
        const auto result = AudioExtent::mp3End(bytes.size(), [&](qint64 at, int count) {
            reads += count;
            return bytes.mid(at, count);
        });
        if (readCount)
            *readCount = reads;
        return result;
    }
  private slots:
    void validTagsComposeWithoutTouchingAudio() {
        const auto source = audio();
        const auto id3 = QByteArray("TAG") + QByteArray(125, '\0');
        const auto body = QByteArray("LYRICSBEGINETT00005Title");
        const auto lyrics = body + QByteArray::number(body.size()).rightJustified(6, '0') + "LYRICS200";
        for (const auto &tag : {id3, ape(), ape("value", true), lyrics, ape() + lyrics + id3})
            QCOMPARE(end(source + tag), qint64(source.size()));
        QByteArray head = QByteArray::fromHex("49443303000000000005") + QByteArray(5, '\0');
        QCOMPARE(end(head + source + ape()), qint64(head.size() + source.size()));
    }
    void malformedOrUnknownTailsStayVisibleToDecoder() {
        const auto source = audio();
        QCOMPARE(end(source + QByteArray(500, '\0')), qint64(source.size() + 500));
        QList<QByteArray> broken;
        auto tag = ape();
        qToLittleEndian<quint32>(0xffffffff, tag.data() + tag.size() - 20);
        broken << tag << ape().chopped(1);
        tag = ape();
        qToLittleEndian<quint32>(999, tag.data() + tag.size() - 16);
        broken << tag;
        tag = ape("x", true);
        tag[0] = 'X';
        broken << tag << QByteArray("LYRICSBEGINETT99999x000019LYRICS200");
        for (const auto &bytes : broken)
            QCOMPARE(end(source + bytes), qint64(source.size() + bytes.size()));
        QCOMPARE(end(QByteArray("fLaC") + source + ape()), qint64(4 + source.size() + ape().size()));
        auto oneHeader = source;
        oneHeader[417] = 'x';
        QCOMPARE(end(oneHeader + ape()), qint64(oneHeader.size() + ape().size()));
        // A tag-shaped byte sequence within a complete final audio frame is not a trailer.
        auto embedded = source;
        const auto fakeTag = ape();
        embedded.replace(embedded.size() - fakeTag.size(), fakeTag.size(), fakeTag);
        QCOMPARE(end(embedded), qint64(embedded.size()));
        // Valid-looking headers inside real frame payload can form a false chain
        // ending at a tag-shaped payload. The actual chain crosses that boundary.
        auto ambiguous = source;
        const auto header = QByteArray::fromHex("fffb9000");
        ambiguous.replace(289, 4, header);
        ambiguous.replace(706, 4, header);
        ambiguous.replace(ambiguous.size() - 128, 3, "TAG");
        QCOMPARE(end(ambiguous), qint64(ambiguous.size()));
    }
    void readsStayBoundedAndDoNotAcceptShortIo() {
        auto source = audio();
        const auto huge = source + ape(QByteArray(2 * 1024 * 1024, 'a'));
        qint64 reads = 0;
        QCOMPARE(end(huge, &reads), qint64(huge.size()));
        QVERIFY(reads < 1024);
        QByteArray stacked = source;
        for (int i = 0; i < 20; ++i)
            stacked += ape();
        QVERIFY(end(stacked, &reads) > source.size());
        QVERIFY(reads <= 1024 * 1024);
        QCOMPARE(AudioExtent::mp3End(source.size(), [](qint64, int) { return QByteArray(); }),
                 qint64(source.size()));
    }
};
QTEST_GUILESS_MAIN(AudioExtentTests)
#include "audio_extent_tests.moc"
