// Crossfade and gapless handover, measured on real audio.
//
// The claims worth proving are that the next song is already playing before the
// last one stops, that the two volumes trade places on an equal-power curve
// rather than dipping through the middle, that the queue and everything hanging
// off it move with the audio, and that steering by hand takes the head start
// back cleanly.
#include "backend.h"
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>

class CrossfadeTest : public QObject {
  Q_OBJECT
  QTemporaryDir storage;
  QTemporaryDir music;
  QStringList files;

  // Short recordings, so a transition arrives inside a test's patience.
  bool encode(const QString &name, const QString &title, int seconds) {
    QProcess run;
    run.start("ffmpeg", {"-nostdin", "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=44100:cl=mono",
                         "-t", QString::number(seconds), "-metadata", "title=" + title,
                         "-metadata", "artist=Fixture artist", "-metadata", "album=Handover",
                         music.filePath(name)});
    return run.waitForFinished(20000) && run.exitCode() == 0;
  }

  // A backend with a three-song local queue, playing the first.
  std::unique_ptr<Backend> loaded(Backend **out = nullptr) {
    auto b = std::make_unique<Backend>();
    b->setVolume(0.8);
    b->setLyricsFallback(false);
    b->setAutoplay(false);
    b->setWatchMusicFolders(false);
    b->setOnlineArtwork(false);
    b->setPrepareNext(false);
    b->clearQueue();
    QVariantList urls;
    for (const auto &file : files)
      urls.append(QUrl::fromLocalFile(file));
    b->importLocalFiles(urls);
    if (out)
      *out = b.get();
    return b;
  }

private slots:
  void initTestCase() {
    qputenv("XDG_DATA_HOME", storage.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", storage.path().toUtf8());
    qputenv("XDG_CACHE_HOME", storage.path().toUtf8());
    QCoreApplication::setApplicationName("sung-crossfade-test");
    QCoreApplication::setOrganizationName("SungTests");
    const auto helper = QFileInfo(QString::fromUtf8(qgetenv("SUNG_FIXTURE_HELPER")))
                            .dir().absoluteFilePath("../helper/catalog.py");
    qputenv("SUNG_HELPER", helper.toUtf8());
    qputenv("SUNG_PYTHON", "/usr/bin/python3");
    QVERIFY(encode("01 first.flac", "First", 4));
    QVERIFY(encode("02 second.flac", "Second", 6));
    QVERIFY(encode("03 third.flac", "Third", 6));
    for (const auto &name : {"01 first.flac", "02 second.flac", "03 third.flac"})
      files << music.filePath(name);
  }

  // Nothing changes for anyone who has not asked for it.
  void overlapIsOffUntilAskedFor() {
    Backend b;
    QCOMPARE(b.crossfadeSeconds(), 0);
    QVERIFY(b.gapless());
    b.setCrossfadeSeconds(5);
    QCOMPARE(b.crossfadeSeconds(), 5);
    b.setCrossfadeSeconds(99);
    QCOMPARE(b.crossfadeSeconds(), 12);
    b.setCrossfadeSeconds(-3);
    QCOMPARE(b.crossfadeSeconds(), 0);
    {
      Backend restored;
      QCOMPARE(restored.crossfadeSeconds(), 0);
    }
  }

  // With no overlap configured, the next song still starts without the pause
  // the media pipeline would otherwise spend loading it.
  void gaplessKeepsPlayingAcrossTheJoin() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    QCOMPARE(b->results()->count(), 3);
    b->setCrossfadeSeconds(0);
    b->setGapless(true);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QCOMPARE(b->currentIndex(), 0);

    // Watch every 10ms from before the join until after it.
    int silentSamples = 0, samples = 0;
    QElapsedTimer watch;
    watch.start();
    while (b->currentIndex() == 0 && watch.elapsed() < 20000) {
      QTest::qWait(10);
      if (b->position() > 500 || b->currentIndex() > 0) {
        ++samples;
        if (!b->playing())
          ++silentSamples;
      }
    }
    QCOMPARE(b->currentIndex(), 1);
    QVERIFY2(samples > 20, "the join was actually observed");
    QVERIFY2(silentSamples <= 1,
             qPrintable(QString("playback stopped for %1 of %2 samples across the join")
                            .arg(silentSamples).arg(samples)));
    // The rest of the application moved with the audio.
    QCOMPARE(b->current().value("title").toString(), QString("Second"));
    QVERIFY(b->media()->source().toLocalFile().contains("02 second"));
    QTRY_VERIFY(b->duration() > 0);
    b->stop();
  }

  // Turning the handover off restores the ordinary transition, which does stop.
  void withoutGaplessTheJoinIsAnOrdinaryTransition() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(0);
    b->setGapless(false);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QElapsedTimer watch;
    watch.start();
    while (b->currentIndex() == 0 && watch.elapsed() < 20000)
      QTest::qWait(10);
    QCOMPARE(b->currentIndex(), 1);
    QVERIFY(b->m_handoffIndex < 0);
    b->stop();
  }

  // With an overlap, both decks sound at once and their gains trade places on
  // an equal-power curve.
  void overlapPlaysBothAndKeepsItsLoudness() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setGapless(true);
    b->setCrossfadeSeconds(2);
    b->setVolume(0.8);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    const double settled = b->effectiveVolume();
    QVERIFY(settled > 0.5);

    QTRY_VERIFY_WITH_TIMEOUT(b->crossfading(), 20000);
    // Sample the overlap as it runs.
    int both = 0, samples = 0;
    double worstPower = 1.0, lowestOutgoing = 1.0, highestIncoming = 0.0;
    while (b->crossfading() && samples < 400) {
      const double outgoing = b->activeAudio().volume();
      const double incoming = b->spareAudio().volume();
      if (b->m_media().playbackState() == QMediaPlayer::PlayingState &&
          b->spareDeck().playbackState() == QMediaPlayer::PlayingState)
        ++both;
      lowestOutgoing = std::min(lowestOutgoing, outgoing);
      highestIncoming = std::max(highestIncoming, incoming);
      // Equal power: the two gains squared should add up to the settled one
      // squared throughout, never dipping the way two linear ramps would.
      const double power = std::sqrt(outgoing * outgoing + incoming * incoming);
      if (incoming > 0.02 && outgoing > 0.02)
        worstPower = std::min(worstPower, power / settled);
      ++samples;
      QTest::qWait(10);
    }
    QVERIFY2(both > 5, qPrintable(QString("both decks played together for %1 samples").arg(both)));
    QVERIFY2(lowestOutgoing < settled * 0.2, "the outgoing song faded out");
    QVERIFY2(highestIncoming > settled * 0.8, "the incoming song faded in");
    QVERIFY2(worstPower > 0.9,
             qPrintable(QString("combined power dipped to %1 of the settled level")
                            .arg(worstPower, 0, 'f', 3)));

    // These fixtures are short enough that the next overlap follows straight
    // on, so the handover is measured the moment it happens rather than later.
    // These fixtures are short enough that the next overlap follows straight
    // on, so the handover is measured the moment it happens rather than later.
    QTRY_COMPARE_WITH_TIMEOUT(b->currentIndex(), 1, 10000);
    const double handedBack = b->activeAudio().volume();
    const double released = b->spareAudio().volume();
    QCOMPARE(b->current().value("title").toString(), QString("Second"));
    QVERIFY2(std::abs(handedBack - settled) < 0.02,
             qPrintable(QString("the mixer was handed back at %1, not %2").arg(handedBack).arg(settled)));
    QVERIFY2(released < 0.001,
             qPrintable(QString("the song that left was still at %1").arg(released)));
    QVERIFY(b->playing());
    QVERIFY2(b->m_usingB, "the decks swapped rather than the audio being reloaded");
    QVERIFY(b->media()->source().toLocalFile().contains("02 second"));
    b->stop();
  }

  // A song too short to hold two overlaps is played in full instead.
  void aShortRecordingIsNotMostlyOverlap() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(12);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QVERIFY(b->duration() < 12000 * 2);
    QElapsedTimer watch;
    watch.start();
    while (b->currentIndex() == 0 && watch.elapsed() < 15000) {
      QVERIFY2(!b->crossfading(), "a four second song must not be overlapped by twelve");
      QTest::qWait(20);
    }
    QCOMPARE(b->currentIndex(), 1);
    b->stop();
  }

  // Skipping by hand during an overlap lands on what was asked for, and leaves
  // nothing playing behind it.
  void steeringDuringAnOverlapTakesBackTheHeadStart() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(2);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(b->crossfading(), 20000);
    b->playAt(2);
    QVERIFY(!b->crossfading());
    QCOMPARE(b->m_handoffIndex, -1);
    QCOMPARE(b->spareDeck().playbackState(), QMediaPlayer::StoppedState);
    QVERIFY(b->spareDeck().source().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 10000);
    QCOMPARE(b->currentIndex(), 2);
    QCOMPARE(b->current().value("title").toString(), QString("Third"));
    QTRY_VERIFY(std::abs(b->activeAudio().volume() - b->effectiveVolume()) < 0.02);
    b->stop();
  }

  // Seeking back out of the tail returns the head start too.
  void seekingOutOfTheTailReleasesTheSpare() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(2);
    b->enqueueItems(b->results()->rows);
    b->playAt(1);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 4000, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(b->crossfading(), 20000);
    b->seek(0);
    QVERIFY(!b->crossfading());
    QCOMPARE(b->m_handoffIndex, -1);
    QVERIFY(b->spareDeck().source().isEmpty());
    QCOMPARE(b->currentIndex(), 1);
    QTRY_VERIFY(std::abs(b->activeAudio().volume() - b->effectiveVolume()) < 0.02);
    b->stop();
  }

  // Repeating one song has nothing to fade into.
  void repeatingOneSongNeverOverlaps() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(2);
    b->setRepeat(2);
    b->enqueueItems(b->results()->rows);
    b->playAt(1);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QElapsedTimer watch;
    watch.start();
    while (watch.elapsed() < 9000) {
      QVERIFY(!b->crossfading());
      QCOMPARE(b->currentIndex(), 1);
      QTest::qWait(25);
    }
    b->setRepeat(0);
    b->stop();
  }

  // The last song of a queue has nowhere to go, so it simply ends.
  void theEndOfTheQueueStillEnds() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(2);
    b->setRepeat(0);
    b->setAutoplay(false);
    b->enqueueItems(b->results()->rows);
    b->playAt(2);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QElapsedTimer watch;
    watch.start();
    while (b->playing() && watch.elapsed() < 15000) {
      QVERIFY(!b->crossfading());
      QTest::qWait(25);
    }
    QVERIFY(!b->playing());
    QCOMPARE(b->currentIndex(), 2);
    QVERIFY(b->spareDeck().source().isEmpty());
  }

  // Shuffling decides where it is going before it starts going there, and
  // arrives at that very song.
  void shufflingPicksItsDestinationInAdvance() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(2);
    b->setShuffle(true);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(b->crossfading(), 20000);
    const int decided = b->m_handoffIndex;
    QVERIFY2(decided >= 0 && decided != 0, "a destination was chosen up front");
    // Arrive at the song that was chosen, not at whichever one is next.
    QTRY_VERIFY_WITH_TIMEOUT(b->currentIndex() != 0, 10000);
    QCOMPARE(b->currentIndex(), decided);
    b->setShuffle(false);
    b->stop();
  }

  // Everything that normally rides on a track change still happens when the
  // decks swap instead.
  void theRestOfTheApplicationMovesWithTheAudio() {
    auto b = loaded();
    QTRY_VERIFY_WITH_TIMEOUT(!b->importingLocal(), 20000);
    b->library("files");
    b->setCrossfadeSeconds(2);
    b->enqueueItems(b->results()->rows);
    b->playAt(0);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && b->duration() > 0, 10000);
    const auto leaving = b->current().value("id").toString();
    QSignalSpy tracks(b.get(), &Backend::trackChanged);
    QSignalSpy lyrics(b.get(), &Backend::lyricsChanged);
    const auto token = b->trackToken();
    QTRY_COMPARE_WITH_TIMEOUT(b->currentIndex(), 1, 20000);
    QTRY_VERIFY(!b->crossfading());
    QVERIFY2(tracks.count() >= 1, "the track change was announced");
    QVERIFY2(lyrics.count() >= 1, "the lyrics were released with it");
    QVERIFY2(b->trackToken() != token, "the track token moved on");
    QVERIFY(b->current().value("id").toString() != leaving);
    QVERIFY2(b->lyricLines().isEmpty(), "the old song's lyrics did not linger");
    // History records the song that is now playing.
    b->library("history");
    QVERIFY2(b->results()->count() >= 1, "the change was recorded in history");
    b->stop();
  }
};
QTEST_MAIN(CrossfadeTest)
#include "crossfade_test.moc"
