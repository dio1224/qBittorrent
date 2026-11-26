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

    struct Match
    {
        bool owned = false;     // owned in the same or better quality
        QString text;           // "Have 4K DV"
        QString tooltip;
        int rank = 0;           // for sorting
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

    QList<Item> m_items;
    QHash<QString, QList<int>> m_movies;      // "title|year" -> items
    QHash<QString, QList<int>> m_episodes;    // "show|season|episode" -> items
    QHash<QString, QList<int>> m_seasons;     // "show|season" -> items
    QHash<QString, int> m_files;              // normalized file name -> item
    QDateTime m_updated;
    QString m_lastError;
    QPointer<QThread> m_worker;
};
