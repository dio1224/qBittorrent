/*
 * Bittorrent Client using Qt and libtorrent.
 * Dio's build: what's already in the Plex library
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>

#include "releaseinfo.h"

class QThread;
class QWidget;

// Keeps an index of the movies and episodes on Dio's Plex server, so search
// results he already owns in the same or better quality can be marked.
class PlexLibrary final : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(PlexLibrary)

public:
    struct Quality
    {
        int resolution = 0;
        int hdr = 0;            // 3 Dolby Vision, 2 HDR10+, 1 HDR, 0 none or not known
        int sourceTier = 2;     // 3 REMUX/disc, 2 encode/WEB-DL, 1 WEBRip/HDTV, 0 DVD/CAM

        QString text() const;   // "4K DV REMUX"
        bool isAtLeast(const Quality &other) const;
    };

    struct Item
    {
        bool episode = false;
        QString title;          // normalized; the show for episodes
        QString originalTitle;  // normalized
        QString displayTitle;
        int year = 0;
        int season = -1;
        int number = -1;        // episode number
        QString ratingKey;
        qint64 updatedAt = 0;
        bool hdrChecked = false;
        Quality quality;
        QStringList files;      // normalized file names, without extension
    };

    // Whether Dio already has a release: in Plex, or as a torrent in qBittorrent
    struct Match
    {
        enum class State
        {
            Unknown,            // Plex not connected and nothing downloading
            No,
            DownloadingWorse,
            Worse,              // in Plex, worse than the listing
            Downloading,        // in qBittorrent, the same or better
            Yes                 // in Plex, the same or better
        };

        State state = State::Unknown;
        QString text;           // what the Ownership column shows (Downloading shows an icon instead of a word)
        QString filterValue;    // what the column's value filter lists
        QString tooltip;
        int rank = 0;           // for sorting
        bool downloading = false;
        bool owned = false;     // in Plex, the same or better
    };

    static PlexLibrary *instance();

    bool isConnected() const;
    QString serverName() const;
    bool isLoading() const;
    QString statusText() const;

    void connectAccount(QWidget *parent);
    void disconnectAccount();
    void refresh();
    void refreshIfStale();

    Match lookup(const QString &releaseName, const ReleaseInfo &release) const;

signals:
    void indexChanged();

private:
    explicit PlexLibrary(QObject *parent);

    void loadCache();
    void saveCache() const;
    void applyItems(const QList<Item> &items, const QDateTime &updated);
    void rebuildMaps();
    void finishRefresh(const QList<Item> &items, const QString &error);
    void scheduleDownloadsRebuild();
    void rebuildDownloads();

    struct Index
    {
        QHash<QString, QList<int>> movies;      // "title|year" -> items
        QHash<QString, QList<int>> episodes;    // "show|season|episode" -> items
        QHash<QString, QList<int>> seasons;     // "show|season" -> items
        QHash<QString, int> files;              // normalized file or torrent name -> item
    };
    static void buildIndex(const QList<Item> &items, Index &index);
    // The best copy among items matching the release; false when none match
    static bool bestMatch(const QList<Item> &items, const Index &index, const QString &releaseName, const ReleaseInfo &release
        , Quality &best, QString &description, bool &exact);

    QList<Item> m_items;
    Index m_index;
    QList<Item> m_downloads;                  // torrents in qBittorrent
    Index m_downloadsIndex;
    class QTimer *m_downloadsTimer = nullptr;
    QDateTime m_updated;
    QString m_lastError;
    QPointer<QThread> m_worker;
};
