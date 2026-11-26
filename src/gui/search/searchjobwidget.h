/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2018-2025  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2006  Christophe Dumez <chris@qbittorrent.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * In addition, as a special exception, the copyright holders give permission to
 * link this program with the OpenSSL project's "OpenSSL" library (or with
 * modified versions of it that use the same license as the "OpenSSL" library),
 * and distribute the linked executables. You must obey the GNU General Public
 * License in all respects for all of the code used other than "OpenSSL".  If you
 * modify file(s), you may extend this exception to your version of the file(s),
 * but you are not obligated to do so. If you do not wish to do so, delete this
 * exception statement from your version.
 */

#pragma once

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include "base/search/searchhandler.h"
#include "base/settingvalue.h"
#include "gui/guiaddtorrentmanager.h"
#include "gui/guiapplicationcomponent.h"
#include "releaseinfo.h"
#include "searchsortmodel.h"

#define ENGINE_URL_COLUMN 4
#define URL_COLUMN 5

class QFrame;
class QHeaderView;
class QLabel;
class QPushButton;
class QModelIndex;
class QStandardItemModel;

class LineEdit;
class SearchHandler;
class SearchSortModel;
struct SearchResult;

template <typename T> class SettingValue;

namespace Ui
{
    class SearchJobWidget;
}

class SearchJobWidget final : public GUIApplicationComponent<QWidget>
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SearchJobWidget)

public:
    enum class NameFilteringMode
    {
        Everywhere,
        OnlyNames
    };
    Q_ENUM(NameFilteringMode)

    enum class Status
    {
        Ready,
        Ongoing,
        Finished,
        Error,
        Aborted,
        NoResults
    };

    SearchJobWidget(const QString &id, const QString &searchPattern, const QList<SearchResult> &searchResults, IGUIApplication *app, QWidget *parent = nullptr);
    SearchJobWidget(const QString &id, IGUIApplication *app, QWidget *parent = nullptr);
    ~SearchJobWidget() override;

    QString id() const;
    QString searchPattern() const;
    QList<SearchResult> searchResults() const;
    Status status() const;
    int visibleResultsCount() const;
    LineEdit *lineEditSearchResultsFilter() const;

    // Runs several site searches into this tab: the typed search, the search with
    // the filter's words, and synonym variants. A few run at a time.
    void runSearches(const QString &pattern, const QStringList &queries, const QString &category, const QStringList &plugins);
    // The synonym list or duplicate merging changed
    void applySearchOptions();

    // This tab's own filters (copied from the filter bar when the search started)
    void setSearchFilters(const QStringList &rows, bool matchAll);
    QStringList searchFilterRows() const;
    bool searchFiltersMatchAll() const;
    QStringList filterWords() const;
    void setFilterRules(const QList<SearchSortModel::FilterRule> &rules, bool matchAll);
    void cancelSearch();

signals:
    void resultsCountUpdated();
    void statusChanged();
    void refreshRequested();
    void filtersChanged();

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void displayColumnHeaderMenu(const QPoint &pos);

private:
    struct RowInfo
    {
        QString normName;           // lowercase, punctuation folded to spaces
        QString infoHash;
        qlonglong size = 0;
        qlonglong seeds = -1;       // of the listing used for downloading
        QString engine;             // the listing used for downloading
        QStringList sources;        // site of every distinct listing
        QList<qlonglong> sourceSeeds;
        QSet<QString> listings;     // site + download link, to spot the same listing found by several searches
        ReleaseInfo release;
        QStringList extraWarnings;
        QSet<int> fromPage;         // columns filled in from the torrent's page
    };

    void loadSettings();
    void saveSettings() const;
    void updateNameFilter();
    void updateSeedsFilter();
    void updateSizeFilter();
    void filterSearchResults(const QString &name);
    void showFilterContextMenu();
    void contextMenuEvent(QContextMenuEvent *event) override;
    void onItemDoubleClicked(const QModelIndex &index);
    void startPendingSearches();
    void onSearchDone(SearchHandler *handler, bool failed);
    void appendSearchResults(const QList<SearchResult> &results);
    void addResultsToModel(const QList<SearchResult> &results);
    void addResultRow(const SearchResult &result, const QString &normName, const QString &infoHash);
    int findDuplicateRow(const QString &normName, const QString &infoHash, qlonglong size) const;
    void mergeIntoRow(int row, const SearchResult &result, const QString &normName, const QString &infoHash);
    void updateSourcesDisplay(int row);
    void clearResults();
    void setCell(int row, int column, const QString &display, const QVariant &underlying, const QString &tooltip = {});
    void updateQualityColumns(int row);
    void updateWarningColumn(int row);
    void updatePlexColumn(int row);
    void updateAllPlexColumns();
    void checkSameNameSizes(int row);
    void placeQualityColumns();
    void onEngineStatuses(const QList<SearchEngineStatus> &statuses);
    void updateSiteStatus(bool searchFinished);
    void retryFailedSites();
    void updateColumnFilterTitles();
    void lookUpDetails();
    void readDetailsOutput();
    void applyDetails(const class QJsonObject &details);
    void stopDetailsLookup();
    void updateResultsCount();
    void setStatus(Status value);
    void downloadTorrent(const QModelIndex &rowIndex, AddTorrentOption option = AddTorrentOption::Default);
    void addTorrentToSession(const QString &source, AddTorrentOption option = AddTorrentOption::Default);
    void fillFilterComboBoxes();
    QHeaderView *header() const;
    int visibleColumnsCount() const;
    void setRowColor(int row, const QColor &color);
    void setRowVisited(int row);
    void onUIThemeChanged();

    void downloadTorrents(AddTorrentOption option = AddTorrentOption::Default);
    void openTorrentPages();
    void copyTorrentURLs() const;
    void copyTorrentDownloadLinks() const;
    void copyTorrentNames() const;
    void copyField(int column) const;

    SettingValue<NameFilteringMode> m_nameFilteringMode;

    QString m_id;
    QString m_searchPattern;
    QList<SearchResult> m_searchResults;
    Ui::SearchJobWidget *m_ui = nullptr;
    QList<SearchHandler *> m_searchHandlers;    // running
    QStringList m_pendingQueries;
    QStringList m_queries;
    QString m_category;
    QStringList m_plugins;
    int m_failedSearches = 0;
    bool m_searchCancelled = false;
    QList<RowInfo> m_rowInfo;                   // one per source-model row
    QHash<QString, int> m_rowByHash;
    QHash<QString, QList<int>> m_rowsByNameKey;  // first 20 chars of the normalized name
    int m_mergedCount = 0;

    QHash<QString, SearchEngineStatus> m_siteStatus;   // plugin -> best status across this tab's searches
    bool m_retrying = false;
    bool m_siteStatusCounted = false;
    QFrame *m_siteStatusBar = nullptr;
    QLabel *m_siteStatusLabel = nullptr;
    QPushButton *m_retrySitesButton = nullptr;
    QPushButton *m_cloudflareButton = nullptr;
    QPushButton *m_turnOffButton = nullptr;
    QStringList m_turnOffCandidates;
    QHash<int, QString> m_columnTitles;

    // "Look up details": reads the selected results' own pages
    class QProcess *m_detailsProcess = nullptr;
    QHash<QString, QList<int>> m_detailsRows;   // page address -> source rows
    QByteArray m_detailsBuffer;
    int m_detailsTotal = 0;
    int m_detailsDone = 0;
    int m_detailsImproved = 0;
    QStringList m_detailsFailures;
    QString m_detailsStatus;                    // shown on the results line
    QStandardItemModel *m_searchListModel = nullptr;
    SearchSortModel *m_proxyModel = nullptr;
    LineEdit *m_lineEditSearchResultsFilter = nullptr;
    class SearchFilterBuilder *m_resultsFilterBuilder = nullptr;
    Status m_status = Status::Ready;
    bool m_noSearchResults = true;
};

Q_DECLARE_METATYPE(SearchJobWidget::NameFilteringMode)
