/*
 * Bittorrent Client using Qt and libtorrent.
 * Dio's build: what's already in the Plex library
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "plexlibrary.h"

#include <algorithm>
#include <limits>

#include <QApplication>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressDialog>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include "base/bittorrent/session.h"
#include "base/bittorrent/torrent.h"
#include "base/global.h"
#include "base/logger.h"
#include "base/path.h"
#include "base/profile.h"
#include "base/settingsstorage.h"

namespace
{
    const QString KEY_CLIENT_ID = u"Search/Dio/Plex/ClientId"_s;
    const QString KEY_SERVER_NAME = u"Search/Dio/Plex/ServerName"_s;
    const QString KEY_SERVER_URL = u"Search/Dio/Plex/ServerUrl"_s;
    const QString KEY_SERVER_TOKEN = u"Search/Dio/Plex/ServerToken"_s;
    const QByteArray PRODUCT = "qBittorrent (Dio)";
    const int STALE_HOURS = 6;

    QString clientId()
    {
        auto *storage = SettingsStorage::instance();
        QString id = storage->loadValue<QString>(KEY_CLIENT_ID);
        if (id.isEmpty())
        {
            id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            storage->storeValue(KEY_CLIENT_ID, id);
        }
        return id;
    }

    Path cachePath()
    {
        return specialFolderLocation(SpecialFolder::Data) / Path(u"dio-plex-index.json"_s);
    }

    QNetworkRequest plexRequest(const QString &url, const QString &token, const QString &client)
    {
        QNetworkRequest request {QUrl(url)};
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("X-Plex-Product", PRODUCT);
        request.setRawHeader("X-Plex-Client-Identifier", client.toUtf8());
        request.setRawHeader("X-Plex-Version", "5.3.1");
        request.setRawHeader("X-Plex-Device-Name", "qBittorrent (Dio)");
        if (!token.isEmpty())
            request.setRawHeader("X-Plex-Token", token.toUtf8());
        return request;
    }

    struct Response
    {
        int status = 0;
        QByteArray body;
        QString error;
    };

    // Waits for one request (works on the GUI thread inside a modal dialog, and on the worker thread)
    Response fetch(QNetworkAccessManager &manager, const QNetworkRequest &request, const bool post = false, const int timeoutMs = 20000)
    {
        QNetworkReply *reply = post ? manager.post(request, QByteArray()) : manager.get(request);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        if (!reply->isFinished())
            loop.exec();

        Response response;
        if (!reply->isFinished())
        {
            reply->abort();
            response.error = PlexLibrary::tr("timed out");
        }
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.body = reply->readAll();
        if (response.error.isEmpty() && (reply->error() != QNetworkReply::NoError))
            response.error = reply->errorString();
        delete reply;
        return response;
    }

    int resolutionFromPlex(const QString &value)
    {
        const QString v = value.toLower();
        if (v == u"4k")
            return 2160;
        if (v == u"1080")
            return 1080;
        if (v == u"720")
            return 720;
        if (!v.isEmpty())
            return 480;
        return 0;
    }

    int hdrFromStreams(const QJsonObject &metadata)
    {
        int hdr = 0;
        for (const QJsonValue &media : metadata.value(u"Media").toArray())
        {
            for (const QJsonValue &part : media.toObject().value(u"Part").toArray())
            {
                for (const QJsonValue &streamValue : part.toObject().value(u"Stream").toArray())
                {
                    const QJsonObject stream = streamValue.toObject();
                    if (stream.value(u"streamType").toInt() != 1)
                        continue;
                    const QString colorTrc = stream.value(u"colorTrc").toString().toLower();
                    const QString display = stream.value(u"displayTitle").toString().toLower()
                        + u' ' + stream.value(u"extendedDisplayTitle").toString().toLower();
                    if (stream.value(u"DOVIPresent").toBool() || stream.contains(u"DOVIProfile") || display.contains(u"dovi")
                        || display.contains(u"dolby vision"))
                        hdr = std::max(hdr, 3);
                    else if (display.contains(u"hdr10+"))
                        hdr = std::max(hdr, 2);
                    else if ((colorTrc == u"smpte2084") || (colorTrc == u"arib-std-b67") || display.contains(u"hdr"))
                        hdr = std::max(hdr, 1);
                }
            }
        }
        return hdr;
    }

    PlexLibrary::Item itemFrom(const QJsonObject &o, const bool episode)
    {
        PlexLibrary::Item item;
        item.episode = episode;
        item.ratingKey = o.value(u"ratingKey").toString();
        item.updatedAt = o.value(u"updatedAt").toVariant().toLongLong();
        item.year = o.value(u"year").toInt();
        if (episode)
        {
            const QString show = o.value(u"grandparentTitle").toString();
            item.title = normalizeTitle(show);
            item.season = o.value(u"parentIndex").toInt(-1);
            item.number = o.value(u"index").toInt(-1);
            item.displayTitle = u"%1 S%2E%3"_s.arg(show).arg(item.season, 2, 10, QChar(u'0')).arg(item.number, 2, 10, QChar(u'0'));
        }
        else
        {
            const QString title = o.value(u"title").toString();
            item.title = normalizeTitle(title);
            item.originalTitle = normalizeTitle(o.value(u"originalTitle").toString());
            item.displayTitle = (item.year > 0) ? u"%1 (%2)"_s.arg(title).arg(item.year) : title;
        }

        // the best version, when there are several
        bool first = true;
        for (const QJsonValue &mediaValue : o.value(u"Media").toArray())
        {
            const QJsonObject media = mediaValue.toObject();
            PlexLibrary::Quality quality;
            quality.resolution = resolutionFromPlex(media.value(u"videoResolution").toString());
            const int bitrate = media.value(u"bitrate").toInt();

            int nameSource = -1;
            for (const QJsonValue &partValue : media.value(u"Part").toArray())
            {
                const QString file = partValue.toObject().value(u"file").toString();
                if (file.isEmpty())
                    continue;
                const QString base = QFileInfo(file).completeBaseName();
                item.files.append(normalizeTitle(base));
                const ReleaseInfo fromName = parseRelease(base);
                if (fromName.source >= 0)
                    nameSource = fromName.sourceTier();
                quality.hdr = std::max(quality.hdr, fromName.hdr);
            }
            if (nameSource >= 0)
                quality.sourceTier = nameSource;
            else if (((quality.resolution >= 2160) && (bitrate >= 35000)) || ((quality.resolution == 1080) && (bitrate >= 18000)))
                quality.sourceTier = 3;  // disc-level bitrate: a remux
            else
                quality.sourceTier = 2;

            if (first || quality.isAtLeast(item.quality))
                item.quality = quality;
            first = false;
        }
        return item;
    }

    QList<PlexLibrary::Item> readLibrary(const QString &baseUrl, const QString &token, const QString &client
        , const QHash<QString, PlexLibrary::Item> &previous, QString &error)
    {
        QNetworkAccessManager manager;
        QList<PlexLibrary::Item> items;
        const auto interrupted = []() { return QThread::currentThread()->isInterruptionRequested(); };
        const auto get = [&](const QString &path) -> QJsonObject
        {
            const Response response = fetch(manager, plexRequest(baseUrl + path, token, client), false, 30000);
            if (response.status != 200)
            {
                error = response.error.isEmpty() ? PlexLibrary::tr("Plex answered %1").arg(response.status) : response.error;
                return {};
            }
            return QJsonDocument::fromJson(response.body).object().value(u"MediaContainer").toObject();
        };

        const QJsonObject sections = get(u"/library/sections"_s);
        if (!error.isEmpty())
            return {};

        for (const QJsonValue &directoryValue : sections.value(u"Directory").toArray())
        {
            const QJsonObject directory = directoryValue.toObject();
            const QString type = directory.value(u"type").toString();
            if ((type != u"movie") && (type != u"show"))
                continue;
            const bool episodes = (type == u"show");
            const QString key = directory.value(u"key").toString();

            int start = 0;
            while (true)
            {
                if (interrupted())
                    return {};
                const QJsonObject page = get(u"/library/sections/%1/all?type=%2&X-Plex-Container-Start=%3&X-Plex-Container-Size=500"_s
                    .arg(key).arg(episodes ? 4 : 1).arg(start));
                if (!error.isEmpty())
                    return {};
                const QJsonArray metadata = page.value(u"Metadata").toArray();
                for (const QJsonValue &value : metadata)
                    items.append(itemFrom(value.toObject(), episodes));
                start += metadata.size();
                const int total = page.value(u"totalSize").toInt(page.value(u"size").toInt());
                if (metadata.isEmpty() || (start >= total))
                    break;
            }
        }

        // Dolby Vision / HDR for 4K items: reuse what's known, read the rest 40 at a time
        QStringList toCheck;
        QHash<QString, int> indexByKey;
        for (int i = 0; i < items.size(); ++i)
        {
            PlexLibrary::Item &item = items[i];
            if ((item.quality.resolution < 2160) || item.ratingKey.isEmpty())
                continue;
            if (const auto it = previous.constFind(item.ratingKey); (it != previous.cend()) && (it->updatedAt == item.updatedAt))
            {
                item.quality.hdr = std::max(item.quality.hdr, it->quality.hdr);
                item.hdrChecked = true;
                continue;
            }
            indexByKey.insert(item.ratingKey, i);
            toCheck.append(item.ratingKey);
        }
        for (qsizetype i = 0; i < toCheck.size(); i += 40)
        {
            if (interrupted())
                return {};
            QString batchError;
            const Response response = fetch(manager, plexRequest(baseUrl + u"/library/metadata/"_s + toCheck.mid(i, 40).join(u','), token, client), false, 30000);
            if (response.status != 200)
                continue;  // HDR details are a bonus
            const QJsonObject container = QJsonDocument::fromJson(response.body).object().value(u"MediaContainer").toObject();
            for (const QJsonValue &value : container.value(u"Metadata").toArray())
            {
                const QJsonObject metadata = value.toObject();
                const int index = indexByKey.value(metadata.value(u"ratingKey").toString(), -1);
                if (index < 0)
                    continue;
                items[index].quality.hdr = std::max(items[index].quality.hdr, hdrFromStreams(metadata));
                items[index].hdrChecked = true;
            }
        }
        return items;
    }

    QString key2(const QString &a, const int b)
    {
        return a + u'|' + QString::number(b);
    }

    QString key3(const QString &a, const int b, const int c)
    {
        return a + u'|' + QString::number(b) + u'|' + QString::number(c);
    }
}

QString PlexLibrary::Quality::text() const
{
    QStringList parts;
    parts.append((resolution >= 2160) ? u"4K"_s : resolutionText(resolution));
    if (const QString hdrText = hdrTierText(hdr); !hdrText.isEmpty())
        parts.append(hdrText);
    if (sourceTier >= 3)
        parts.append(u"REMUX"_s);
    parts.removeAll(QString());
    return parts.join(u' ');
}

bool PlexLibrary::Quality::isAtLeast(const Quality &other) const
{
    if (resolution != other.resolution)
        return resolution > other.resolution;
    if (hdr != other.hdr)
        return hdr > other.hdr;
    return sourceTier >= other.sourceTier;
}

PlexLibrary *PlexLibrary::instance()
{
    static PlexLibrary *library = nullptr;
    if (!library)
        library = new PlexLibrary(qApp);
    return library;
}

PlexLibrary::PlexLibrary(QObject *parent)
    : QObject(parent)
{
    loadCache();

    // torrents in qBittorrent count too ("Downloading")
    m_downloadsTimer = new QTimer(this);
    m_downloadsTimer->setSingleShot(true);
    m_downloadsTimer->setInterval(800);
    connect(m_downloadsTimer, &QTimer::timeout, this, &PlexLibrary::rebuildDownloads);
    if (auto *session = BitTorrent::Session::instance())
    {
        connect(session, &BitTorrent::Session::torrentsLoaded, this, &PlexLibrary::scheduleDownloadsRebuild);
        connect(session, &BitTorrent::Session::torrentAdded, this, &PlexLibrary::scheduleDownloadsRebuild);
        connect(session, &BitTorrent::Session::torrentAboutToBeRemoved, this, &PlexLibrary::scheduleDownloadsRebuild);
        connect(session, &BitTorrent::Session::torrentMetadataReceived, this, &PlexLibrary::scheduleDownloadsRebuild);
    }
    rebuildDownloads();

    // don't leave the reader running while the app closes
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]()
    {
        if (m_worker && m_worker->isRunning())
        {
            m_worker->requestInterruption();
            m_worker->wait(8000);
        }
    });
}

bool PlexLibrary::isConnected() const
{
    const auto *storage = SettingsStorage::instance();
    return !storage->loadValue<QString>(KEY_SERVER_URL).isEmpty() && !storage->loadValue<QString>(KEY_SERVER_TOKEN).isEmpty();
}

QString PlexLibrary::serverName() const
{
    return SettingsStorage::instance()->loadValue<QString>(KEY_SERVER_NAME);
}

bool PlexLibrary::isLoading() const
{
    return m_worker && m_worker->isRunning();
}

QString PlexLibrary::statusText() const
{
    if (!isConnected())
        return tr("Not connected");
    if (isLoading() && m_items.isEmpty())
        return tr("Reading your Plex library...");

    int movies = 0;
    int episodes = 0;
    for (const Item &item : m_items)
        ++(item.episode ? episodes : movies);
    QString text = tr("%1: %2 movies, %3 episodes").arg(serverName(), QLocale().toString(movies), QLocale().toString(episodes));
    if (m_updated.isValid())
    {
        const qint64 minutes = m_updated.secsTo(QDateTime::currentDateTime()) / 60;
        text += (minutes < 60) ? tr(" (updated %1 min ago)").arg(minutes) : tr(" (updated %1 h ago)").arg(minutes / 60);
    }
    if (isLoading())
        text += tr(", refreshing...");
    else if (!m_lastError.isEmpty())
        text += tr(". Last refresh failed: %1").arg(m_lastError);
    return text;
}

void PlexLibrary::connectAccount(QWidget *parent)
{
    QNetworkAccessManager manager;
    const QString client = clientId();

    // a modal window while plex.tv is contacted, so nothing else can be clicked meanwhile
    QProgressDialog progress(tr("Contacting plex.tv..."), tr("Cancel"), 0, 0, parent);
    progress.setWindowTitle(tr("Connect to Plex"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.show();

    // 1. Ask plex.tv for a sign-in code, and let Dio approve it in the browser
    const Response pin = fetch(manager, plexRequest(u"https://plex.tv/api/v2/pins?strong=true"_s, {}, client), true);
    const QJsonObject pinObject = QJsonDocument::fromJson(pin.body).object();
    const qint64 pinId = pinObject.value(u"id").toVariant().toLongLong();
    const QString code = pinObject.value(u"code").toString();
    if ((pin.status >= 300) || (pinId == 0) || code.isEmpty())
    {
        progress.close();
        QMessageBox::warning(parent, tr("Connect to Plex"), tr("Couldn't reach plex.tv (%1).")
            .arg(pin.error.isEmpty() ? QString::number(pin.status) : pin.error));
        return;
    }

    const QString authUrl = u"https://app.plex.tv/auth#?clientID=%1&code=%2&context%5Bdevice%5D%5Bproduct%5D=qBittorrent%20(Dio)"_s
        .arg(client, code);
    QDesktopServices::openUrl(QUrl(authUrl, QUrl::TolerantMode));

    progress.setLabelText(tr("Approve qBittorrent on the Plex page that just opened in your browser.\n"
        "This window closes by itself once you do."));

    QString accountToken;
    QElapsedTimer elapsed;
    elapsed.start();
    while (!progress.wasCanceled() && (elapsed.elapsed() < (5 * 60 * 1000)))
    {
        QEventLoop wait;
        QTimer::singleShot(2000, &wait, &QEventLoop::quit);
        wait.exec();
        if (progress.wasCanceled())
            break;
        const Response check = fetch(manager, plexRequest(u"https://plex.tv/api/v2/pins/%1?code=%2"_s.arg(pinId).arg(code), {}, client));
        accountToken = QJsonDocument::fromJson(check.body).object().value(u"authToken").toString();
        if (!accountToken.isEmpty())
            break;
    }
    if (accountToken.isEmpty())
    {
        const bool cancelled = progress.wasCanceled();
        progress.close();
        if (!cancelled)
            QMessageBox::warning(parent, tr("Connect to Plex"), tr("Plex didn't confirm the sign-in in time. Try again."));
        return;
    }
    progress.setLabelText(tr("Finding your Plex server..."));

    // 2. Find the server
    const Response resources = fetch(manager, plexRequest(u"https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1"_s, accountToken, client));
    struct Server
    {
        QString name;
        QString token;
        QStringList urls;
        bool owned = false;
    };
    QList<Server> servers;
    for (const QJsonValue &value : QJsonDocument::fromJson(resources.body).array())
    {
        const QJsonObject resource = value.toObject();
        if (!resource.value(u"provides").toString().contains(u"server"))
            continue;
        Server server;
        server.name = resource.value(u"name").toString();
        server.owned = resource.value(u"owned").toBool();
        server.token = resource.value(u"accessToken").toString();
        if (server.token.isEmpty())
            server.token = accountToken;
        QStringList local;
        QStringList remote;
        QStringList relay;
        for (const QJsonValue &connectionValue : resource.value(u"connections").toArray())
        {
            const QJsonObject connection = connectionValue.toObject();
            const QString uri = connection.value(u"uri").toString();
            if (connection.value(u"relay").toBool())
            {
                relay.append(uri);
            }
            else if (connection.value(u"local").toBool())
            {
                local.append(uri);
                const QString address = connection.value(u"address").toString();
                if (!address.isEmpty())
                    local.append(u"http://%1:%2"_s.arg(address).arg(connection.value(u"port").toInt(32400)));
            }
            else
            {
                remote.append(uri);
            }
        }
        server.urls = local + remote + relay;
        servers.append(server);
    }
    std::stable_sort(servers.begin(), servers.end(), [](const Server &a, const Server &b) { return a.owned && !b.owned; });
    if (servers.isEmpty())
    {
        progress.close();
        QMessageBox::warning(parent, tr("Connect to Plex"), tr("No Plex server was found on this Plex account."));
        return;
    }

    Server chosen = servers.first();
    if (servers.size() > 1)
    {
        progress.hide();
        QStringList names;
        for (const Server &server : servers)
            names.append(server.owned ? server.name : tr("%1 (shared with you)").arg(server.name));
        bool ok = false;
        const QString picked = QInputDialog::getItem(parent, tr("Connect to Plex"), tr("Which server has your library?"), names, 0, false, &ok);
        if (!ok)
            return;
        chosen = servers.at(std::max<qsizetype>(0, names.indexOf(picked)));
        progress.show();
    }

    // 3. An address that answers from this Mac
    QString workingUrl;
    for (const QString &url : chosen.urls)
    {
        if (fetch(manager, plexRequest(url + u"/identity"_s, chosen.token, client), false, 5000).status == 200)
        {
            workingUrl = url;
            break;
        }
    }
    progress.close();
    if (workingUrl.isEmpty())
    {
        QMessageBox::warning(parent, tr("Connect to Plex"), tr("Couldn't reach %1 from this computer.").arg(chosen.name));
        return;
    }

    auto *storage = SettingsStorage::instance();
    storage->storeValue(KEY_SERVER_NAME, chosen.name);
    storage->storeValue(KEY_SERVER_URL, workingUrl);
    storage->storeValue(KEY_SERVER_TOKEN, chosen.token);
    m_items.clear();
    rebuildMaps();
    m_updated = {};
    refresh();
    QMessageBox::information(parent, tr("Connect to Plex"), tr("Connected to %1. Your library is being read in the background; "
        "search results you already own in the same or better quality will show in the In Plex column.").arg(chosen.name));
}

void PlexLibrary::disconnectAccount()
{
    auto *storage = SettingsStorage::instance();
    storage->removeValue(KEY_SERVER_NAME);
    storage->removeValue(KEY_SERVER_URL);
    storage->removeValue(KEY_SERVER_TOKEN);
    m_items.clear();
    rebuildMaps();
    m_updated = {};
    QFile::remove(cachePath().data());
    emit indexChanged();
}

void PlexLibrary::refreshIfStale()
{
    if (isConnected() && (!m_updated.isValid() || (m_updated.secsTo(QDateTime::currentDateTime()) > (STALE_HOURS * 3600))))
        refresh();
}

void PlexLibrary::refresh()
{
    if (!isConnected() || isLoading())
        return;

    const auto *storage = SettingsStorage::instance();
    const QString url = storage->loadValue<QString>(KEY_SERVER_URL);
    const QString token = storage->loadValue<QString>(KEY_SERVER_TOKEN);
    const QString client = clientId();
    QHash<QString, Item> previous;
    for (const Item &item : asConst(m_items))
    {
        if (item.hdrChecked)
            previous.insert(item.ratingKey, item);
    }

    QThread *thread = QThread::create([this, url, token, client, previous]()
    {
        QString error;
        const QList<Item> items = readLibrary(url, token, client, previous, error);
        if (QThread::currentThread()->isInterruptionRequested())
            return;
        QMetaObject::invokeMethod(this, [this, items, error]() { finishRefresh(items, error); }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    m_worker = thread;
    m_lastError.clear();
    thread->start(QThread::LowPriority);
    emit indexChanged();
}

void PlexLibrary::finishRefresh(const QList<Item> &items, const QString &error)
{
    if (!error.isEmpty() && items.isEmpty())
    {
        m_lastError = error;
        LogMsg(tr("Couldn't read the Plex library: %1").arg(error), Log::WARNING);
        emit indexChanged();
        return;
    }
    m_lastError.clear();
    applyItems(items, QDateTime::currentDateTime());
    saveCache();
}

void PlexLibrary::applyItems(const QList<Item> &items, const QDateTime &updated)
{
    m_items = items;
    m_updated = updated;
    rebuildMaps();
    emit indexChanged();
}

void PlexLibrary::rebuildMaps()
{
    buildIndex(m_items, m_index);
}

void PlexLibrary::buildIndex(const QList<Item> &items, Index &index)
{
    index = {};
    for (int i = 0; i < items.size(); ++i)
    {
        const Item &item = items.at(i);
        if (item.episode)
        {
            if (item.number >= 0)
                index.episodes[key3(item.title, item.season, item.number)].append(i);
            index.seasons[key2(item.title, item.season)].append(i);
        }
        else if (!item.title.isEmpty() && (item.year > 0))
        {
            index.movies[key2(item.title, item.year)].append(i);
            if (!item.originalTitle.isEmpty() && (item.originalTitle != item.title))
                index.movies[key2(item.originalTitle, item.year)].append(i);
        }
        for (const QString &file : item.files)
            index.files.insert(file, i);
    }
}

void PlexLibrary::scheduleDownloadsRebuild()
{
    m_downloadsTimer->start();
}

void PlexLibrary::rebuildDownloads()
{
    m_downloads.clear();
    if (const auto *session = BitTorrent::Session::instance())
    {
        for (const BitTorrent::Torrent *torrent : session->torrents())
        {
            const QString name = torrent->name();
            const ReleaseInfo release = parseRelease(name);
            Item item;
            item.episode = release.isEpisode();
            item.title = release.title;
            item.year = release.year;
            item.season = release.season;
            item.number = release.episode;
            item.displayTitle = name;
            item.quality = {release.resolution, release.hdr, release.sourceTier()};
            item.files = {normalizeTitle(name)};
            m_downloads.append(item);
        }
    }
    buildIndex(m_downloads, m_downloadsIndex);
    emit indexChanged();
}

bool PlexLibrary::bestMatch(const QList<Item> &items, const Index &index, const QString &releaseName, const ReleaseInfo &release
    , Quality &best, QString &description, bool &exact)
{
    if (items.isEmpty())
        return false;

    QList<int> candidates;
    bool seasonPack = false;
    exact = false;
    if (const auto it = index.files.constFind(normalizeTitle(releaseName)); it != index.files.cend())
    {
        candidates.append(it.value());
        exact = true;
    }
    else if (release.isEpisode() && !release.title.isEmpty())
    {
        if (release.episode >= 0)
        {
            candidates = index.episodes.value(key3(release.title, release.season, release.episode));
        }
        else
        {
            candidates = index.seasons.value(key2(release.title, release.season));
            seasonPack = true;
        }
    }
    else if (release.isMovie())
    {
        for (const int year : {release.year, (release.year - 1), (release.year + 1)})
            candidates += index.movies.value(key2(release.title, year));
    }
    if (candidates.isEmpty())
        return false;

    // a season pack counts by its worst episode
    best = items.at(candidates.first()).quality;
    for (const int i : asConst(candidates))
    {
        const Quality &quality = items.at(i).quality;
        if (seasonPack ? !quality.isAtLeast(best) : quality.isAtLeast(best))
            best = quality;
    }
    const Item &item = items.at(candidates.first());
    description = seasonPack
        ? tr("%1 episodes of season %2 of %3").arg(candidates.size()).arg(release.season).arg(item.displayTitle.section(u" S"_s, 0, 0))
        : item.displayTitle;
    return true;
}

PlexLibrary::Match PlexLibrary::lookup(const QString &releaseName, const ReleaseInfo &release) const
{
    Match match;
    const Quality wanted {release.resolution, release.hdr, release.sourceTier()};

    Quality plexBest;
    QString plexWhat;
    bool plexExact = false;
    const bool inPlex = bestMatch(m_items, m_index, releaseName, release, plexBest, plexWhat, plexExact);

    Quality downloadBest;
    QString downloadWhat;
    bool downloadExact = false;
    const bool downloading = bestMatch(m_downloads, m_downloadsIndex, releaseName, release, downloadBest, downloadWhat, downloadExact);

    const bool plexGood = inPlex && (plexExact || plexBest.isAtLeast(wanted));
    const bool downloadGood = downloading && (downloadExact || downloadBest.isAtLeast(wanted));

    if (plexGood)
    {
        match.state = Match::State::Yes;
        match.tooltip = plexExact
            ? tr("Plex already has this exact release: %1 (%2).").arg(plexWhat, plexBest.text())
            : tr("Plex has %1 in %2, the same or better than this listing.").arg(plexWhat, plexBest.text());
    }
    else if (downloadGood)
    {
        match.state = Match::State::Downloading;
        match.tooltip = tr("Already in qBittorrent, the same or better: %1 (%2).").arg(downloadWhat, downloadBest.text());
    }
    else if (inPlex && (!downloading || plexBest.isAtLeast(downloadBest)))
    {
        match.state = Match::State::Worse;
        match.tooltip = tr("Plex has %1 in %2, worse than this listing, so this would be an upgrade.").arg(plexWhat, plexBest.text());
    }
    else if (downloading)
    {
        match.state = Match::State::DownloadingWorse;
        match.tooltip = tr("qBittorrent has %1 (%2), worse than this listing, so this would be an upgrade.").arg(downloadWhat, downloadBest.text());
        if (inPlex)
            match.tooltip += u'\n' + tr("Plex has %1 in %2.").arg(plexWhat, plexBest.text());
    }
    else if (!m_items.isEmpty())
    {
        match.state = Match::State::No;
        match.tooltip = tr("Not in your Plex library or in qBittorrent.");
    }

    switch (match.state)
    {
    case Match::State::Yes:
        match.text = tr("Yes");
        match.filterValue = match.text;
        match.rank = 5;
        match.owned = true;
        break;
    case Match::State::Downloading:
        match.filterValue = tr("Downloading");
        match.downloading = true;
        match.rank = 4;
        break;
    case Match::State::Worse:
        match.text = tr("Worse");
        match.filterValue = match.text;
        match.rank = 3;
        break;
    case Match::State::DownloadingWorse:
        match.text = tr("Worse");
        match.filterValue = tr("Downloading worse");
        match.downloading = true;
        match.rank = 2;
        break;
    case Match::State::No:
        match.text = tr("No");
        match.filterValue = match.text;
        match.rank = 1;
        break;
    case Match::State::Unknown:
        break;
    }
    return match;
}

void PlexLibrary::loadCache()
{
    QFile file {cachePath().data()};
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    QList<Item> items;
    for (const QJsonValue &value : root.value(u"items").toArray())
    {
        const QJsonObject o = value.toObject();
        Item item;
        item.episode = o.value(u"e").toBool();
        item.title = o.value(u"t").toString();
        item.originalTitle = o.value(u"ot").toString();
        item.displayTitle = o.value(u"d").toString();
        item.year = o.value(u"y").toInt();
        item.season = o.value(u"s").toInt(-1);
        item.number = o.value(u"n").toInt(-1);
        item.ratingKey = o.value(u"k").toString();
        item.updatedAt = o.value(u"u").toVariant().toLongLong();
        item.hdrChecked = o.value(u"hc").toBool();
        item.quality.resolution = o.value(u"r").toInt();
        item.quality.hdr = o.value(u"h").toInt();
        item.quality.sourceTier = o.value(u"st").toInt(2);
        for (const QJsonValue &f : o.value(u"f").toArray())
            item.files.append(f.toString());
        items.append(item);
    }
    m_items = items;
    m_updated = QDateTime::fromSecsSinceEpoch(root.value(u"updated").toVariant().toLongLong());
    rebuildMaps();
}

void PlexLibrary::saveCache() const
{
    QJsonArray items;
    for (const Item &item : m_items)
    {
        QJsonObject o
        {
            {u"e"_s, item.episode}, {u"t"_s, item.title}, {u"d"_s, item.displayTitle}, {u"y"_s, item.year},
            {u"k"_s, item.ratingKey}, {u"u"_s, item.updatedAt}, {u"hc"_s, item.hdrChecked},
            {u"r"_s, item.quality.resolution}, {u"h"_s, item.quality.hdr}, {u"st"_s, item.quality.sourceTier},
            {u"f"_s, QJsonArray::fromStringList(item.files)}
        };
        if (!item.originalTitle.isEmpty())
            o.insert(u"ot"_s, item.originalTitle);
        if (item.episode)
        {
            o.insert(u"s"_s, item.season);
            o.insert(u"n"_s, item.number);
        }
        items.append(o);
    }
    const QJsonObject root {{u"updated"_s, m_updated.toSecsSinceEpoch()}, {u"items"_s, items}};
    QFile file {cachePath().data()};
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}
