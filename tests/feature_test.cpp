// Live checks for the features added on top of the original interface. Each one
// drives the real application and asserts against what the running window
// reports, then photographs the result for review.
#include "uitest.h"
#include "backend.h"
#include "m3color.h"
#include "rowselection.h"
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QQmlContext>
#include <QQmlExpression>
#include <QQuickItem>
#include <QQuickWindow>
#include <QFont>
#include <QTest>
#include <qpa/qwindowsysteminterface.h>
#include <functional>

namespace {
QQuickItem *shownItem(QQuickItem *root, const QString &name) {
  if (!root->isVisible())
    return nullptr;
  if (root->objectName() == name)
    return root;
  for (auto child : root->childItems())
    if (auto found = shownItem(child, name))
      return found;
  return nullptr;
}
QQuickItem *anyItem(QQuickItem *root, const QString &name) {
  if (root->objectName() == name)
    return root;
  for (auto child : root->childItems())
    if (auto found = anyItem(child, name))
      return found;
  return nullptr;
}

struct Check {
  Backend *backend;
  QQuickWindow *window;
  QString directory;
  int failures = 0;

  void check(bool ok, const QString &label) {
    fprintf(stdout, "%s %s\n", ok ? "PASS" : "FAIL", qPrintable(label));
    fflush(stdout);
    if (!ok)
      ++failures;
  }
  bool until(const std::function<bool()> &predicate, int timeout = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout)
      QTest::qWait(25);
    return predicate();
  }
  QVariant evaluate(const QString &script) {
    QQmlExpression expression(qmlContext(window), window, script);
    return expression.evaluate();
  }
  QColor themeColor(const QString &role) { return evaluate("Theme." + role).value<QColor>(); }
  void shot(const QString &name) {
    QTest::qWait(280);
    shotNow(name);
  }
  // Capturing mid-transition cannot afford to settle first.
  void shotNow(const QString &name) {
    check(window->grabWindow().save(directory + '/' + name + ".png"), "capture " + name);
  }
  void click(const QString &name) {
    tap(name);
    QTest::qWait(320);
  }
  // A click scoped to one part of the window, where the same row names appear
  // in more than one list at once.
  void clickWithin(QQuickItem *parent, const QString &name) {
    auto item = parent ? shownItem(parent, name) : nullptr;
    check(item, "find " + name + " in " + (parent ? parent->objectName() : QString("nothing")));
    if (!item)
      return;
    const auto point = item->mapToScene(item->boundingRect().center()).toPoint();
    QTest::mouseMove(window, point);
    QTest::qWait(60);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
    QTest::qWait(320);
  }
  // A click with no settling wait, for watching what the click sets off.
  void tap(const QString &name) {
    auto item = shownItem(window->contentItem(), name);
    check(item, "find " + name);
    if (!item)
      return;
    const auto point = item->mapToScene(item->boundingRect().center()).toPoint();
    QTest::mouseMove(window, point);
    QTest::qWait(60);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
  }
  QObject *dialog(const QString &name, const QString &method = "open") {
    auto found = window->findChild<QObject *>(name);
    check(found, "reach " + name);
    if (found) {
      QMetaObject::invokeMethod(found, qPrintable(method));
      QTest::qWait(420);
    }
    return found;
  }
  void closeDialog(QObject *target) {
    if (target)
      QMetaObject::invokeMethod(target, "close");
    QTest::qWait(320);
  }
  void finish() {
    fprintf(stdout, "RESULT %d failures\n", failures);
    fflush(stdout);
    QCoreApplication::exit(failures ? 1 : 0);
  }
};

void paintCover(const QString &path, const QColor &base, const QColor &accent) {
  QImage cover(640, 640, QImage::Format_RGB32);
  QPainter paint(&cover);
  paint.fillRect(cover.rect(), base);
  paint.setRenderHint(QPainter::Antialiasing);
  paint.fillRect(0, 0, 640, 240, accent);
  paint.setBrush(accent.lighter(130));
  paint.setPen(Qt::NoPen);
  paint.drawEllipse(QPoint(430, 430), 150, 150);
  paint.end();
  cover.save(path);
}

bool encodeTrack(Check &c, const QString &path, const QString &title, const QString &album,
                 const QString &artist, int track, const QStringList &extra = {}) {
  QStringList arguments{"-nostdin", "-v", "error", "-f", "lavfi", "-i", "anullsrc=r=8000:cl=mono",
                        "-t", "150", "-metadata", "title=" + title, "-metadata", "album=" + album,
                        "-metadata", "artist=" + artist, "-metadata", "album_artist=" + artist,
                        "-metadata", "date=2026", "-metadata", "track=" + QString::number(track)};
  arguments += extra;
  arguments << path;
  QProcess encode;
  encode.start("ffmpeg", arguments);
  const bool ok = encode.waitForFinished(20000) && encode.exitCode() == 0;
  c.check(ok, "generate " + QFileInfo(path).fileName());
  return ok;
}

double contrastOf(const QColor &a, const QColor &b) {
  const auto luminance = [](const QColor &c) {
    const auto channel = [](double v) {
      return v <= 0.040449936 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.redF()) + 0.7152 * channel(c.greenF()) + 0.0722 * channel(c.blueF());
  };
  const double x = luminance(a), y = luminance(b);
  return (std::max(x, y) + 0.05) / (std::min(x, y) + 0.05);
}
} // namespace

void runDynamicColorTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory + "/music/Still Water");
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1320, 860);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(false);
  b->setVolume(0);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  b->setWatchMusicFolders(false);
  b->setOnlineArtwork(false);
  b->setLyricsFallback(false);
  b->setArtworkAccent(false);
  b->setAccentColor("");
  QTest::qWait(200);

  paintCover(c.directory + "/music/Still Water/cover.png", QColor("#1f4f6b"), QColor("#d98324"));
  if (!encodeTrack(c, c.directory + "/music/Still Water/01.flac", "Across the still water",
                   "Still Water", "Rill", 1))
    return c.finish();
  b->importMusicFolder(QUrl::fromLocalFile(c.directory + "/music"));
  c.check(c.until([&] { return !b->importingLocal(); }, 40000), "import the colour fixture");
  b->library("files");
  c.check(c.until([&] { return b->results()->count() == 1; }), "the fixture is in the library");

  // --- Nothing chosen: the built-in palette is untouched ---
  const auto plainBackground = c.themeColor("background");
  const auto plainSurface = c.themeColor("surface");
  c.check(!c.evaluate("Theme.useSource").toBool(), "no source colour by default");
  c.check(plainBackground == QColor("#181211"), "the built-in dark background is unchanged");
  c.shot("01-default-palette");

  // --- A chosen source colour reaches the surfaces, not just the accent ---
  b->setAccentColor("#386a20");
  QTest::qWait(250);
  c.check(c.evaluate("Theme.useSource").toBool(), "the chosen colour drives the scheme");
  const auto greenBackground = c.themeColor("background");
  const auto greenSurface = c.themeColor("surface");
  const auto greenContainer = c.themeColor("container");
  c.check(greenBackground != plainBackground && greenSurface != plainSurface,
          "surfaces follow the source colour, not only the accent");
  // Material tints neutrals with a trace of the source hue; it must stay a hint.
  for (const auto &pair : QList<QPair<QString, QColor>>{{"background", greenBackground},
                                                        {"surface", greenSurface},
                                                        {"container", greenContainer}}) {
    const double chroma = m3::measure(pair.second).chroma;
    c.check(chroma > 0.5 && chroma < 12.0,
            QString("%1 is tinted but stays neutral (chroma %2)").arg(pair.first).arg(chroma, 0, 'f', 1));
    c.check(qAbs(m3::measure(pair.second).hue - m3::measure(QColor("#386a20")).hue) < 12.0,
            QString("%1 carries the source hue").arg(pair.first));
  }
  // The surface ladder still climbs away from the background in a dark theme.
  c.check(m3::toneOf(greenBackground) < m3::toneOf(greenSurface) &&
              m3::toneOf(greenSurface) < m3::toneOf(greenContainer) &&
              m3::toneOf(greenContainer) < m3::toneOf(c.themeColor("high")),
          "dark surfaces lighten as they stack");
  c.shot("02-green-source-dark");

  const auto floors = [&](const QString &where) {
    const QStringList surfaces{"background", "surface", "container", "high"};
    for (const auto &surface : surfaces) {
      c.check(contrastOf(c.themeColor("text"), c.themeColor(surface)) >= 4.5,
              QString("%1: body text keeps 4.5:1 on %2").arg(where, surface));
      c.check(contrastOf(c.themeColor("muted"), c.themeColor(surface)) >= 4.5,
              QString("%1: secondary text keeps 4.5:1 on %2").arg(where, surface));
      c.check(contrastOf(c.themeColor("primary"), c.themeColor(surface)) >= 4.5,
              QString("%1: the accent keeps 4.5:1 on %2").arg(where, surface));
    }
    c.check(contrastOf(c.themeColor("primaryText"), c.themeColor("primary")) >= 4.5,
            where + ": text on the accent keeps 4.5:1");
    c.check(contrastOf(c.themeColor("containerText"), c.themeColor("primaryContainer")) >= 4.5,
            where + ": container text keeps 4.5:1");
    c.check(contrastOf(c.themeColor("outline"), c.themeColor("surface")) >= 1.3,
            where + ": dividers stay visible");
  };
  floors("green dark");

  b->setTheme("light");
  QTest::qWait(300);
  const auto lightBackground = c.themeColor("background");
  c.check(m3::toneOf(lightBackground) > 90, "the light theme starts from a near-white surface");
  c.check(m3::toneOf(c.themeColor("background")) > m3::toneOf(c.themeColor("container")),
          "light surfaces darken as they stack");
  floors("green light");
  c.shot("03-green-source-light");
  b->setTheme("dark");
  QTest::qWait(300);

  // --- Artwork drives it, and a different cover moves it ---
  b->setAccentColor("");
  b->setArtworkAccent(true);
  b->enqueueItems(b->results()->rows);
  b->playAt(0);
  c.check(c.until([&] { return b->playing(); }), "the fixture plays");
  c.check(c.until([&] { return c.evaluate("Theme.useArtwork").toBool(); }),
          "the cover becomes the source colour");
  const auto coverBackground = c.themeColor("background");
  c.check(coverBackground != plainBackground, "the cover re-tints the window");
  floors("cover dark");
  c.shot("04-artwork-source");

  const auto warmHue = m3::measure(c.themeColor("primary")).hue;
  c.evaluate("Theme.artworkSeed=Qt.rgba(0.20,0.36,0.78,1)");
  QTest::qWait(350);
  const auto coolHue = m3::measure(c.themeColor("primary")).hue;
  c.check(qAbs(warmHue - coolHue) > 40, "a different cover moves the whole scheme");
  c.check(c.themeColor("background") != coverBackground, "including its surfaces");
  floors("cool cover");
  c.shot("05-artwork-source-cool");

  // --- Turning it off restores the built-in palette exactly ---
  b->setArtworkAccent(false);
  c.evaluate("Theme.artworkSeed=Qt.rgba(0,0,0,0)");
  QTest::qWait(300);
  c.check(!c.evaluate("Theme.useSource").toBool(), "the scheme is released");
  c.check(c.themeColor("background") == plainBackground && c.themeColor("surface") == plainSurface,
          "the built-in palette returns untouched");
  c.shot("06-released");

  b->stop();
  b->clearQueue();
  c.finish();
}

void runNavigationMotionTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory + "/music");
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1320, 860);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(true);
  b->setVolume(0);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  b->setWatchMusicFolders(false);
  b->setOnlineArtwork(false);

  paintCover(c.directory + "/music/cover.png", QColor("#1f4f6b"), QColor("#d98324"));
  for (int i = 1; i <= 3; ++i)
    if (!encodeTrack(c, QString("%1/music/%2.flac").arg(c.directory).arg(i),
                     QString("Track %1").arg(i), "Motion", "Rill", i))
      return c.finish();
  b->importMusicFolder(QUrl::fromLocalFile(c.directory + "/music"));
  c.check(c.until([&] { return !b->importingLocal(); }, 40000), "import the motion fixture");

  auto column = anyItem(w->contentItem(), "contentColumn");
  auto body = anyItem(w->contentItem(), "contentBody");
  auto destinations = w->findChild<QObject *>("destinationTransition");
  auto tabs = w->findChild<QObject *>("tabTransition");
  c.check(column && body && destinations && tabs, "the motion targets and controllers exist");
  if (!column || !body || !destinations || !tabs)
    return c.finish();

  // --- The durations Material specifies ---
  c.check(destinations->property("totalDuration").toInt() == 300, "a transition lasts 300ms");
  c.check(destinations->property("leaveDuration").toInt() == 90,
          "the outgoing view leaves over the first 30%");
  c.check(destinations->property("arriveDuration").toInt() == 210,
          "the incoming view arrives over the remaining 70%");
  c.check(qAbs(destinations->property("arriveScale").toReal() - 0.92) < 0.001,
          "fading through grows the incoming view from 92%");
  c.check(qAbs(destinations->property("axisTravel").toReal() - 30) < 0.001,
          "sharing an axis travels 30dp");

  // --- Rail destinations fade through ---
  b->home();
  c.check(c.until([&] { return !b->busy(); }), "Home is ready");
  QTest::qWait(400);
  c.check(qAbs(column->opacity() - 1) < 0.01 && qAbs(column->scale() - 1) < 0.01,
          "a settled view sits at full size and opacity");
  c.tap("nav_library");
  // Sample during the outgoing half: the view must be on its way out.
  QTest::qWait(45);
  const double leavingOpacity = column->opacity();
  c.check(leavingOpacity < 0.9 && leavingOpacity > 0.0,
          QString("the outgoing destination is fading (%1)").arg(leavingOpacity, 0, 'f', 2));
  c.check(qAbs(column->property("shift").toReal()) < 0.01,
          "fading through never moves the view sideways");
  c.shotNow("01-fade-through-leaving");
  // Sample during the incoming half: it grows back from 92%.
  c.check(c.until([&] { return column->scale() < 0.999; }, 400), "the arriving destination is scaled");
  const double arrivingScale = column->scale();
  c.check(arrivingScale >= 0.919 && arrivingScale < 1.0,
          QString("the arriving destination grows from 92%% (%1)").arg(arrivingScale, 0, 'f', 3));
  // Let the arriving view become legible before photographing it.
  c.until([&] { return column->scale() > 0.96; }, 300);
  c.shotNow("02-fade-through-arriving");
  c.check(c.until([&] { return !destinations->property("running").toBool(); }, 2000),
          "the destination transition completes");
  c.check(qAbs(column->opacity() - 1) < 0.01 && qAbs(column->scale() - 1) < 0.01 &&
              qAbs(column->property("shift").toReal()) < 0.01,
          "the view is handed back exactly as it was found");
  c.check(b->page() == "library", "and the destination actually changed");

  // --- Library tabs share the X axis ---
  c.check(w->property("libraryTab") == "favorites", "the library opens on Liked songs");
  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("files")));
  QTest::qWait(45);
  const double forwardShift = body->property("shift").toReal();
  c.check(forwardShift < -1,
          QString("moving forward pushes the outgoing tab left (%1)").arg(forwardShift, 0, 'f', 1));
  c.check(qAbs(column->property("shift").toReal()) < 0.01 && qAbs(column->scale() - 1) < 0.01,
          "the tab bar itself stays put");
  c.shotNow("03-shared-axis-forward");
  c.check(c.until([&] { return body->property("shift").toReal() > 1; }, 400),
          "the arriving tab enters from the right");
  c.until([&] { return body->opacity() > 0.5; }, 300);
  c.shotNow("03b-shared-axis-arriving");
  c.check(c.until([&] { return !tabs->property("running").toBool(); }, 2000),
          "the tab transition completes");
  c.check(qAbs(body->property("shift").toReal()) < 0.01 && qAbs(body->opacity() - 1) < 0.01,
          "and settles back in place");
  c.check(w->property("libraryTab") == "files", "the tab actually changed");

  // Travelling back through the tabs reverses the axis.
  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("favorites")));
  QTest::qWait(45);
  const double backShift = body->property("shift").toReal();
  c.check(backShift > 1,
          QString("moving back pushes the outgoing tab right (%1)").arg(backShift, 0, 'f', 1));
  c.shotNow("04-shared-axis-back");
  c.check(c.until([&] { return !tabs->property("running").toBool(); }, 2000), "it completes too");

  // --- Reduced motion is honoured: no animation, and navigation still works ---
  b->setMotion(false);
  QTest::qWait(200);
  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("files")));
  QTest::qWait(30);
  c.check(!tabs->property("running").toBool() && !destinations->property("running").toBool(),
          "reduced motion skips the transition entirely");
  c.check(qAbs(body->opacity() - 1) < 0.01 && qAbs(body->property("shift").toReal()) < 0.01,
          "and leaves nothing half-animated");
  c.check(w->property("libraryTab") == "files", "navigation still arrives");
  c.shot("05-reduced-motion");
  b->setMotion(true);

  // --- A second navigation mid-flight must not strand the view ---
  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("history")));
  QTest::qWait(40);
  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("mixes")));
  c.check(c.until([&] { return !tabs->property("running").toBool(); }, 3000),
          "an interrupted transition still finishes");
  c.check(qAbs(body->opacity() - 1) < 0.01 && qAbs(body->property("shift").toReal()) < 0.01 &&
              qAbs(body->scale() - 1) < 0.01,
          "and the view is left whole");
  c.check(w->property("libraryTab") == "mixes", "the last destination wins");
  c.shot("06-interrupted");

  b->stop();
  b->clearQueue();
  c.finish();
}

void runArtistHeroTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory + "/music/Still Water");
  QDir().mkpath(c.directory + "/music/Night Ferry");
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1320, 860);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(true);
  b->setVolume(0);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  b->setWatchMusicFolders(false);
  b->setOnlineArtwork(false);

  // One artist, two albums, six songs: enough for the hero to have something
  // true to report.
  paintCover(c.directory + "/music/Still Water/cover.png", QColor("#1f4f6b"), QColor("#d98324"));
  paintCover(c.directory + "/music/Night Ferry/cover.png", QColor("#3d2a52"), QColor("#4fa3a5"));
  const QStringList still{"The light arrives", "Across the still water", "A quiet moment"};
  const QStringList ferry{"Harbour lights", "Night ferry", "Coming ashore"};
  for (int i = 0; i < still.size(); ++i)
    if (!encodeTrack(c, QString("%1/music/Still Water/%2.flac").arg(c.directory).arg(i + 1),
                     still[i], "Still Water", "Rill", i + 1))
      return c.finish();
  for (int i = 0; i < ferry.size(); ++i)
    if (!encodeTrack(c, QString("%1/music/Night Ferry/%2.flac").arg(c.directory).arg(i + 1),
                     ferry[i], "Night Ferry", "Rill", i + 1))
      return c.finish();
  b->importMusicFolder(QUrl::fromLocalFile(c.directory + "/music"));
  c.check(c.until([&] { return !b->importingLocal(); }, 40000), "import the artist fixture");

  // --- The hero belongs to artist pages only ---
  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("local-albums")));
  c.check(c.until([&] { return b->results()->count() == 2; }), "two albums group");
  QTest::qWait(400);
  c.check(!shownItem(w->contentItem(), "artistHero"), "an album grid shows no artist hero");
  c.check(shownItem(w->contentItem(), "collectionHeaderTitle"), "it keeps the standard header");
  c.shot("01-albums-standard-header");

  QMetaObject::invokeMethod(w, "chooseLibrary", Q_ARG(QVariant, QVariant("local-artists")));
  c.check(c.until([&] { return b->results()->count() == 1; }), "one artist groups");
  QTest::qWait(400);
  c.check(!shownItem(w->contentItem(), "artistHero"), "the artist grid is not an artist page");

  b->open(b->results()->get(0));
  c.check(c.until([&] { return !b->busy() && b->page() == "local-artist"; }), "the artist opens");
  c.check(c.until([&] { return shownItem(w->contentItem(), "artistHero") != nullptr; }),
          "the artist page raises its hero");
  auto hero = shownItem(w->contentItem(), "artistHero");
  if (!hero)
    return c.finish();
  c.check(!shownItem(w->contentItem(), "collectionHeaderTitle"),
          "and stands in for the standard header rather than doubling it");

  // --- What it says is true ---
  const auto info = b->artistInfo();
  c.check(info.value("tracks").toInt() == 6, "the hero counts every song");
  c.check(info.value("albums").toInt() == 2, "and every album");
  c.check(info.value("seconds").toLongLong() == 900, "and the real running time");
  auto summary = shownItem(w->contentItem(), "artistHeroSummary");
  c.check(summary && summary->property("text").toString() == "2 albums · 6 songs · 15 min",
          "the summary reads back what it counted");
  auto name = shownItem(w->contentItem(), "artistHeroName");
  c.check(name && name->property("text").toString() == "Rill", "the hero names the artist");

  // --- Material's large top app bar proportions ---
  auto portrait = shownItem(w->contentItem(), "artistHeroPortrait");
  c.check(portrait && qAbs(portrait->property("radius").toReal() - portrait->width() / 2) < 1,
          "the portrait is round, as an artist's picture is");
  c.check(hero->height() > 150, "the open band is a hero, not a row");
  c.check(shownItem(w->contentItem(), "artistHeroBackdrop"), "the cover sits behind it");
  // Exactly one Play action is offered at a time.
  auto listPlay = [&] {
    auto row = shownItem(w->contentItem(), "collectionToolsButton");
    return row != nullptr;
  };
  c.check(!listPlay(), "the list's own action row stands down while the hero is open");
  const double expanded = hero->height();
  const double titleExpanded = name->property("font").value<QFont>().pixelSize();
  c.shot("02-artist-hero");

  // --- It collapses on scroll and comes back ---
  auto tracks = shownItem(w->contentItem(), "tracksView");
  c.check(tracks, "the artist's songs are listed");
  if (tracks) {
    tracks->setProperty("contentY", tracks->property("originY").toReal() + 300);
    c.check(c.until([&] { return hero->height() < expanded - 40; }, 2000),
            "scrolling collapses the hero");
    c.check(c.until([&] {
      return name->property("font").value<QFont>().pixelSize() < titleExpanded;
    }, 2000), "and the name shrinks with it");
    auto actions = anyItem(w->contentItem(), "artistHeroActions");
    c.check(actions && actions->opacity() < 0.3, "the actions fade out of the collapsed bar");
    c.check(listPlay(), "and the list's action row takes them back");
    c.shot("03-artist-hero-collapsed");
    tracks->setProperty("contentY", tracks->property("originY").toReal());
    c.check(c.until([&] { return hero->height() > expanded - 5; }, 2000),
            "scrolling back opens it again");
    c.shot("04-artist-hero-restored");
  }

  // --- Its actions work ---
  c.check(b->queue()->count() == 0, "nothing is queued yet");
  c.click("artistHeroPlay");
  c.check(c.until([&] { return b->queue()->count() == 6; }), "Play queues the artist's songs");
  c.check(c.until([&] { return b->playing(); }), "and starts them");
  c.shot("05-artist-hero-playing");
  b->stop();
  b->clearQueue();
  b->setShuffle(false);
  c.click("artistHeroShuffle");
  c.check(c.until([&] { return b->queue()->count() == 6; }), "Shuffle queues them too");
  c.check(b->shuffle(), "and turns shuffling on");
  b->setShuffle(false);

  // Pinning is offered exactly where the library has something to pin. A local
  // artist is a grouping of tags rather than a collection with an id, so the
  // action hides there, the same way the standard header hides it.
  auto pin = anyItem(w->contentItem(), "artistHeroPin");
  c.check(pin, "the hero carries a pin action");
  c.check(pin && pin->isVisible() == !b->collectionItem().isEmpty(),
          "the pin is offered only when there is a collection to pin");
  c.shot("06-artist-hero-actions");

  // --- Leaving the artist puts the standard header back ---
  b->back();
  c.check(c.until([&] { return b->page() != "local-artist"; }), "Back leaves the artist");
  QTest::qWait(500);
  c.check(!shownItem(w->contentItem(), "artistHero"), "the hero is released");
  c.check(shownItem(w->contentItem(), "collectionHeaderTitle"), "the standard header returns");
  c.shot("07-after-artist");

  b->stop();
  b->clearQueue();
  c.finish();
}

void runSingAlongTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory + "/music");
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1320, 860);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(true);
  b->setVolume(0);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  b->setWatchMusicFolders(false);
  b->setOnlineArtwork(false);
  b->setLyricsFallback(false);

  paintCover(c.directory + "/music/cover.png", QColor("#26324f"), QColor("#e2a03f"));
  if (!encodeTrack(c, c.directory + "/music/01.flac", "Across the still water", "Still Water",
                   "Rill", 1))
    return c.finish();
  if (!encodeTrack(c, c.directory + "/music/02.flac", "No words", "Still Water", "Rill", 2))
    return c.finish();
  b->importMusicFolder(QUrl::fromLocalFile(c.directory + "/music"));
  c.check(c.until([&] { return !b->importingLocal(); }, 40000), "import the sing-along fixture");
  b->library("files");
  b->collection()->setSortKey("title");
  QTest::qWait(200);
  c.check(c.until([&] { return b->results()->count() == 2; }), "two songs are available");
  b->enqueueItems(b->results()->rows);
  b->playAt(0);
  c.check(c.until([&] { return b->playing(); }), "playback starts");
  const auto sung = b->current();

  // Lines with explicit ends, and one without, so the fallback is exercised.
  QFile lrc(c.directory + "/sing.lrc");
  c.check(lrc.open(QIODevice::WriteOnly), "write the timed lyrics");
  lrc.write("[00:10.00]The light arrives\n[00:20.00]Across the still water\n"
            "[00:30.00]A quiet moment\n[00:40.00]We move with the tide\n");
  lrc.close();
  b->importLyrics(QUrl::fromLocalFile(lrc.fileName()), sung.value("id").toString());
  c.check(c.until([&] { return b->lyricLines().size() == 4; }), "four timed lines load");

  // --- The progress the fill is drawn from ---
  b->seek(0);
  c.check(c.until([&] { return b->lyricIndex() < 0; }, 3000), "before the first line there is none");
  c.check(b->lyricProgress() < 0, "and no progress to report");
  struct Sample { int position; int line; double progress; const char *what; };
  for (const auto &s : {Sample{10000, 0, 0.0, "the first line starts empty"},
                        Sample{15000, 0, 0.5, "and is half sung halfway through"},
                        Sample{19500, 0, 0.95, "and nearly full at its end"},
                        Sample{20000, 1, 0.0, "the next line starts empty in turn"},
                        Sample{35000, 2, 0.5, "a middle line tracks the same way"},
                        // The last line is held to the end of the audio, so it
                        // fills at the pace the rest of the song is sung at.
                        Sample{42000, 3, 0.2, "and a trailing line fills at the song's pace"},
                        Sample{49000, 3, 0.9, "reaching the end of the line, not of the track"}}) {
    b->seek(s.position);
    c.check(c.until([&] { return b->lyricIndex() == s.line; }, 3000),
            QString("%1 (line %2)").arg(s.what).arg(s.line));
    const double measured = b->lyricProgress();
    c.check(qAbs(measured - s.progress) < 0.12,
            QString("%1: fill is %2, expected about %3")
                .arg(s.what).arg(measured, 0, 'f', 2).arg(s.progress, 0, 'f', 2));
  }
  // Progress is a fraction, always.
  for (int position = 0; position <= 60000; position += 1500) {
    b->seek(position);
    QTest::qWait(20);
    const double measured = b->lyricProgress();
    c.check(measured < 0 || (measured >= 0 && measured <= 1),
            QString("progress stays a fraction at %1ms").arg(position));
  }

  // --- The layout ---
  w->setProperty("immersive", true);
  c.check(c.until([&] { return shownItem(w->contentItem(), "immersivePlayer") != nullptr; }),
          "the immersive player opens");
  auto player = shownItem(w->contentItem(), "immersivePlayer");
  if (!player)
    return c.finish();
  c.check(player->property("hasTimedLyrics").toBool(), "the song offers timed lyrics");
  QMetaObject::invokeMethod(player, "layoutRequested", Q_ARG(QString, QString("singalong")));
  c.check(c.until([&] { return player->property("displayedLayout") == "singalong"; }),
          "sing along can be chosen");
  auto singAlong = shownItem(w->contentItem(), "singAlong");
  c.check(singAlong, "the sing-along surface is on screen");
  c.check(!shownItem(w->contentItem(), "immersiveArtwork"),
          "it takes the whole stage rather than sharing it with the cover");
  c.check(!shownItem(w->contentItem(), "liveLyrics"), "and replaces the reading view");
  if (!singAlong)
    return c.finish();

  // --- The line being sung is the one that is emphasised ---
  b->seek(15000);
  c.check(c.until([&] { return b->lyricIndex() == 0; }, 3000), "the first line is live");
  QTest::qWait(500);
  auto current = shownItem(w->contentItem(), "singAlongCurrent");
  c.check(current && current->property("text").toString() == "The light arrives",
          "the sung line is the one marked current");
  const double activeSize = current ? current->property("font").value<QFont>().pixelSize() : 0;
  c.check(activeSize >= 28, "it is set at display size");
  // Even a four-line lyric brings its live line to where the eye is looking,
  // rather than leaving it stranded at the top of the view.
  if (current) {
    const double centre = current->mapToScene(current->boundingRect().center()).y();
    c.check(centre > w->height() * 0.25 && centre < w->height() * 0.6,
            QString("the sung line sits in the reading band (%1 of %2)")
                .arg(centre, 0, 'f', 0).arg(w->height()));
  }
  c.shot("01-singalong-first-line");

  // The fill is a real measurement, not decoration: it tracks playback.
  auto fill = anyItem(singAlong, "singAlongFill");
  c.check(fill, "the sung line carries a fill");
  const double halfway = fill ? fill->width() : 0;
  b->seek(19000);
  c.check(c.until([&] { return b->lyricProgress() > 0.85; }, 3000), "playback nears the line's end");
  QTest::qWait(300);
  auto laterFill = anyItem(singAlong, "singAlongFill");
  c.check(laterFill && laterFill->width() > halfway,
          "the fill advances with the music, it does not merely appear");
  c.shot("02-singalong-line-filling");

  b->seek(35000);
  c.check(c.until([&] { return b->lyricIndex() == 2; }, 3000), "a later line takes over");
  QTest::qWait(600);
  auto moved = shownItem(w->contentItem(), "singAlongCurrent");
  c.check(moved && moved->property("text").toString() == "A quiet moment",
          "emphasis follows the music to the next line");
  c.shot("03-singalong-later-line");

  // --- Instrumental stretches say so instead of going blank ---
  b->seek(2000);
  c.check(c.until([&] { return b->lyricIndex() < 0; }, 3000), "playback returns before the words");
  QTest::qWait(400);
  auto waiting = shownItem(w->contentItem(), "singAlongWaiting");
  c.check(waiting && waiting->isVisible(), "the wait is acknowledged rather than left blank");
  auto cue = shownItem(w->contentItem(), "singAlongCue");
  c.check(cue && cue->property("text").toString().contains("Lyrics in"),
          "and counts down to the first line");
  c.shot("04-singalong-waiting");

  // --- A song with no timed lyrics cannot be sung along to, and says so ---
  b->next();
  c.check(c.until([&] { return b->currentIndex() == 1 && b->playing(); }, 20000),
          "the next song plays (now " + b->current().value("title").toString() + ")");
  c.check(c.until([&] { return b->lyricLines().isEmpty(); }, 8000), "it has no timed lyrics");
  QTest::qWait(500);
  c.check(player->property("displayedLayout") == "artwork",
          "sing along steps aside when a song cannot drive it");
  c.check(player->property("preferredLayout") == "singalong",
          "without forgetting that it was chosen");
  c.shot("05-singalong-fallback");

  b->previous();
  c.check(c.until([&] { return b->lyricLines().size() == 4; }, 20000), "returning restores the lyrics");
  c.check(c.until([&] { return player->property("displayedLayout") == "singalong"; }, 3000),
          "and sing along returns with them");

  // --- Reduced motion keeps it usable ---
  b->setMotion(false);
  b->seek(25000);
  c.check(c.until([&] { return b->lyricIndex() == 1; }, 3000), "the line changes without motion");
  QTest::qWait(400);
  auto still = shownItem(w->contentItem(), "singAlongCurrent");
  c.check(still && still->property("text").toString() == "Across the still water",
          "the right line is still emphasised");
  c.shot("06-singalong-reduced-motion");
  b->setMotion(true);

  w->setProperty("immersive", false);
  QTest::qWait(300);
  b->stop();
  b->clearQueue();
  c.finish();
}

void runCrossfadeUiTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1320, 860);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(false);
  b->setVolume(0);
  b->setWatchMusicFolders(false);
  b->setCrossfadeSeconds(0);
  b->setGapless(true);

  auto settings = c.dialog("settingsDialog");
  c.check(settings, "Settings opens");
  if (!settings)
    return c.finish();
  settings->setProperty("category", 1);
  QTest::qWait(400);
  c.check(shownItem(w->contentItem(), "crossfadeSetting"), "Playback offers crossfade");
  c.check(shownItem(w->contentItem(), "gaplessSwitch"), "and gapless playback");
  auto value = shownItem(w->contentItem(), "crossfadeValue");
  c.check(value && value->property("text").toString() == "Off",
          "it reads Off until it is asked for");
  c.shot("01-crossfade-off");

  auto slider = shownItem(w->contentItem(), "crossfadeSlider");
  c.check(slider, "the crossfade slider is on screen");
  if (slider) {
    c.check(qAbs(slider->property("from").toReal()) < 0.001 &&
                qAbs(slider->property("to").toReal() - 12) < 0.001,
            "it spans nothing to twelve seconds");
    slider->setProperty("value", 6);
    QMetaObject::invokeMethod(slider, "moved");
    c.check(c.until([&] { return b->crossfadeSeconds() == 6; }), "moving it sets the overlap");
    QTest::qWait(250);
    c.check(value && value->property("text").toString() == "6 s", "and it reads back in seconds");
    // Bring the control itself into view for the capture.
    settings->setProperty("searchQuery", "crossfade");
    QTest::qWait(400);
    c.check(shownItem(w->contentItem(), "crossfadeSlider"), "the control is reachable by name");
    c.shot("02-crossfade-six-seconds");
    settings->setProperty("searchQuery", "");
    QTest::qWait(300);
  }

  // The setting survives the dialog and the session.
  c.closeDialog(settings);
  QTest::qWait(300);
  c.check(b->crossfadeSeconds() == 6, "the overlap is remembered");
  b->setCrossfadeSeconds(0);
  c.check(b->crossfadeSeconds() == 0, "and can be turned off again");

  // Searching Settings finds it by what it does, not only by its name.
  settings = c.dialog("settingsDialog");
  if (settings) {
    settings->setProperty("searchQuery", "overlap");
    QTest::qWait(400);
    c.check(shownItem(w->contentItem(), "crossfadeSetting"), "searching for overlap finds it");
    settings->setProperty("searchQuery", "pause between songs");
    QTest::qWait(400);
    c.check(shownItem(w->contentItem(), "gaplessSwitch"), "and gapless by what it prevents");
    c.shot("03-crossfade-search");
    settings->setProperty("searchQuery", "");
    QTest::qWait(300);
    c.closeDialog(settings);
  }
  c.finish();
}

void runTrackDetailsTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory + "/music");
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1320, 900);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(false);
  b->setVolume(0);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  b->setWatchMusicFolders(false);
  b->setOnlineArtwork(false);
  b->setLyricsFallback(false);

  paintCover(c.directory + "/music/cover.png", QColor("#26324f"), QColor("#e2a03f"));
  // A lossless recording with a full set of tags, so every field has something
  // true to report, and a lossy one, where some of them genuinely do not apply.
  if (!encodeTrack(c, c.directory + "/music/01.flac", "Across the still water", "Still Water",
                   "Rill", 3,
                   {"-metadata", "genre=Ambient", "-metadata", "composer=A Composer",
                    "-metadata", "disc=2", "-metadata", "album_artist=Various Artists",
                    "-ac", "2", "-sample_fmt", "s16"}))
    return c.finish();
  if (!encodeTrack(c, c.directory + "/music/02.mp3", "No tags", "Still Water", "Rill", 4,
                   {"-ac", "1"}))
    return c.finish();
  b->importMusicFolder(QUrl::fromLocalFile(c.directory + "/music"));
  c.check(c.until([&] { return !b->importingLocal(); }, 40000), "import the metadata fixture");
  b->library("files");
  b->collection()->setSortKey("title");
  QTest::qWait(200);
  c.check(c.until([&] { return b->results()->count() == 2; }), "both recordings imported");

  QVariantMap lossless, lossy;
  for (const auto &row : b->results()->rows) {
    const auto t = row.toMap();
    if (t.value("title") == "Across the still water")
      lossless = t;
    else if (t.value("title") == "No tags")
      lossy = t;
  }
  c.check(!lossless.isEmpty() && !lossy.isEmpty(), "both recordings are readable");
  if (lossless.isEmpty())
    return c.finish();

  // --- The tags really were read off the file ---
  c.check(lossless.value("genre").toString() == "Ambient", "genre is read from the file");
  c.check(lossless.value("composer").toString() == "A Composer", "so is the composer");
  c.check(lossless.value("albumArtist").toString() == "Various Artists", "and the album artist");
  c.check(lossless.value("channels").toInt() == 2, "and the channel count");
  c.check(lossless.value("bitDepth").toInt() == 16, "and the bit depth of a lossless recording");
  c.check(lossy.value("bitDepth").toInt() == 0,
          "a lossy recording reports no depth, because it has none");
  c.check(lossy.value("channels").toInt() == 1, "but still reports its channels");

  // --- And they reach the dialog ---
  const auto details = b->trackDetails(lossless);
  QStringList labels;
  QVariantMap byLabel;
  for (const auto &row : details) {
    const auto entry = row.toMap();
    labels << entry.value("label").toString();
    byLabel.insert(entry.value("label").toString(), entry.value("value"));
  }
  for (const auto &expected : {"Title", "Artist", "Album artist", "Album", "Composer", "Genre",
                               "Year", "Track", "Source", "Duration", "File bit depth",
                               "File channels", "File sample rate"})
    c.check(labels.contains(expected), QString("details list %1").arg(expected));
  c.check(byLabel.value("Genre").toString() == "Ambient", "Genre reads back what was tagged");
  c.check(byLabel.value("Track").toString() == "3 on disc 2",
          "a numbered track on a multi-disc album says which disc");
  c.check(byLabel.value("File channels").toString() == "Stereo", "two channels read as Stereo");
  c.check(byLabel.value("File bit depth").toString() == "16-bit", "depth reads in bits");
  c.check(byLabel.value("Album artist").toString() == "Various Artists",
          "the album artist is listed when it differs from the performer");

  // A performer who is also the album artist is not repeated.
  const auto plain = b->trackDetails(lossy);
  QStringList lossyLabels;
  for (const auto &row : plain)
    lossyLabels << row.toMap().value("label").toString();
  c.check(!lossyLabels.contains("Album artist"),
          "an album artist the same as the performer is not repeated");
  c.check(!lossyLabels.contains("File bit depth"),
          "a lossy recording is not given a bit depth it does not have");
  c.check(lossyLabels.contains("File channels"), "but its channels are still reported");
  c.check(!lossyLabels.contains("Genre"), "an untagged genre is left out rather than shown empty");

  // --- The dialog itself ---
  auto dialog = w->findChild<QObject *>("trackDetailsDialog");
  c.check(dialog, "the details dialog exists");
  if (dialog) {
    QMetaObject::invokeMethod(dialog, "inspect", Q_ARG(QVariant, QVariant(lossless)));
    QTest::qWait(500);
    c.check(dialog->property("visible").toBool(), "it opens on a song");
    c.shot("01-track-details-full");
    QMetaObject::invokeMethod(dialog, "close");
    QTest::qWait(300);
    QMetaObject::invokeMethod(dialog, "inspect", Q_ARG(QVariant, QVariant(lossy)));
    QTest::qWait(500);
    c.shot("02-track-details-sparse");
    QMetaObject::invokeMethod(dialog, "close");
    QTest::qWait(300);
  }
  c.finish();
}

void runQueueHistoryTests(Backend *b, QQuickWindow *w) {
  Check c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory + "/music");
  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1400, 880);
  QTest::qWait(500);
  b->setTheme("dark");
  b->setMotion(false);
  b->setVolume(0);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  b->setWatchMusicFolders(false);
  b->setOnlineArtwork(false);
  b->setLyricsFallback(false);
  b->setCrossfadeSeconds(0);
  b->clearHistory();

  paintCover(c.directory + "/music/cover.png", QColor("#26324f"), QColor("#e2a03f"));
  const QStringList titles{"The light arrives", "Across the still water", "A quiet moment",
                           "We move with the tide"};
  for (int i = 0; i < titles.size(); ++i)
    if (!encodeTrack(c, QString("%1/music/%2.flac").arg(c.directory).arg(i + 1), titles[i],
                     "Still Water", "Rill", i + 1))
      return c.finish();
  b->importMusicFolder(QUrl::fromLocalFile(c.directory + "/music"));
  c.check(c.until([&] { return !b->importingLocal(); }, 40000), "import the history fixture");
  b->library("files");
  c.check(c.until([&] { return b->results()->count() == 4; }), "four songs are available");
  b->enqueueItems(b->results()->rows);

  // --- Nothing has been played, so there is nothing to look back over ---
  w->setProperty("side", "queue");
  QTest::qWait(500);
  c.check(shownItem(w->contentItem(), "queueTabs"), "the queue panel offers both views");
  c.check(w->property("queueTab") == "next", "it opens on what is coming");
  c.check(shownItem(w->contentItem(), "queueView"), "the queue is the one on screen");
  {
    auto title = shownItem(w->contentItem(), "sidePanelTitle");
    c.check(title && title->property("text").toString() == "Up next", "and the panel says so");
  }
  c.check(!shownItem(w->contentItem(), "recentlyPlayedView"), "the look-back is not");
  c.shot("01-queue-up-next");

  w->setProperty("queueTab", "history");
  QTest::qWait(400);
  c.check(shownItem(w->contentItem(), "recentlyPlayedView"), "History shows the look-back");
  c.check(!shownItem(w->contentItem(), "queueView"), "and stands the queue down");
  c.check(b->recentlyPlayed()->count() == 0, "which is empty before anything has played");
  c.shot("02-queue-history-empty");

  // --- Playing songs fills it, newest first, without the song playing now ---
  w->setProperty("queueTab", "next");
  b->playAt(0);
  c.check(c.until([&] { return b->playing(); }), "the first song plays");
  QTest::qWait(400);
  c.check(b->recentlyPlayed()->count() == 0,
          "the song playing now is not something to look back over");
  b->playAt(1);
  c.check(c.until([&] { return b->playing() && b->currentIndex() == 1; }), "a second song plays");
  b->playAt(2);
  c.check(c.until([&] { return b->playing() && b->currentIndex() == 2; }), "and a third");
  QTest::qWait(500);
  c.check(c.until([&] { return b->recentlyPlayed()->count() == 2; }),
          "the two that finished are there to look back over");
  // Compare against the queue rather than the fixture, since the library
  // decides its own order.
  const auto first = b->queue()->get(0).value("title").toString();
  const auto second = b->queue()->get(1).value("title").toString();
  c.check(b->recentlyPlayed()->get(0).value("title").toString() == second,
          "newest first, so the one just before this one is at the top");
  c.check(b->recentlyPlayed()->get(1).value("title").toString() == first,
          "and the one before that next");

  w->setProperty("queueTab", "history");
  QTest::qWait(500);
  auto list = shownItem(w->contentItem(), "recentlyPlayedView");
  c.check(list && list->property("count").toInt() == 2, "the panel lists both of them");
  auto count = shownItem(w->contentItem(), "recentlyPlayedCount");
  c.check(count && count->property("text").toString().contains("2"), "and says how many");
  auto title = shownItem(w->contentItem(), "sidePanelTitle");
  c.check(title && title->property("text").toString() == "Recently played",
          "the panel says which of the two it is showing");
  c.check(!shownItem(w->contentItem(), "revealPlayingButton"),
          "and drops the shortcut that only makes sense for the queue");
  c.shot("03-queue-history-filled");

  // --- Playing from it keeps the queue, the way the history page does ---
  QStringList queuedBefore;
  for (int i = 0; i < b->queue()->count(); ++i)
    queuedBefore << b->queue()->get(i).value("id").toString();
  const auto wanted = b->recentlyPlayed()->get(0);
  c.clickWithin(list, "trackRow_0");
  c.check(c.until([&] { return b->current().value("id") == wanted.value("id"); }, 8000),
          "choosing one plays it");
  // The queue is kept rather than replaced: the chosen song is slotted in to
  // play next, and nothing that was queued is lost.
  QStringList queuedAfter;
  for (int i = 0; i < b->queue()->count(); ++i)
    queuedAfter << b->queue()->get(i).value("id").toString();
  for (const auto &id : queuedBefore)
    c.check(queuedAfter.contains(id), "the queue keeps everything it already held");
  c.check(queuedAfter.contains(wanted.value("id").toString()),
          "and the chosen song joins it rather than replacing it");
  c.shot("04-queue-history-played");

  // The song now playing left the look-back when it started.
  QTest::qWait(400);
  for (int i = 0; i < b->recentlyPlayed()->count(); ++i)
    c.check(b->recentlyPlayed()->get(i).value("id") != b->current().value("id"),
            "what is playing is never also in the look-back");

  // --- It links onward to the full history page ---
  c.click("openFullHistory");
  c.check(c.until([&] { return b->libraryId() == "history"; }, 4000),
          "the panel opens the full history page");
  c.shot("05-full-history");

  // --- Clearing history empties it, and Undo brings it back ---
  const int before = b->recentlyPlayed()->count();
  c.check(before > 0, "there is something to clear");
  b->clearHistory();
  QTest::qWait(300);
  c.check(b->recentlyPlayed()->count() == 0, "clearing history empties the look-back too");
  b->undo();
  QTest::qWait(300);
  c.check(b->recentlyPlayed()->count() == before, "and Undo restores it");

  w->setProperty("queueTab", "next");
  w->setProperty("side", "");
  b->stop();
  b->clearQueue();
  c.finish();
}
