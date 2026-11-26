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

#include "searchjobwidget.h"

#include <algorithm>
#include <cstdlib>

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QHeaderView>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QMessageBox>
#include <QPalette>
#include <QStandardItemModel>
#include <QUrl>

#include "base/logger.h"
#include "base/preferences.h"
#include "base/search/searchdownloadhandler.h"
#include "base/search/searchhandler.h"
#include "base/search/searchpluginmanager.h"
#include "base/utils/misc.h"
#include "gui/interfaces/iguiapplication.h"
#include "gui/lineedit.h"
#include "gui/uithememanager.h"
#include "searchfilterbuilder.h"
#include "searchsynonyms.h"
#include "searchsortmodel.h"
#include "ui_searchjobwidget.h"

namespace
{

    // Results with a shorter (normalized) name than this are never merged by name
    const int MIN_MERGE_NAME_LENGTH = 8;
    const int NAME_KEY_LENGTH = 20;

    // The Jackett plugin reports a missing API key or unreachable server as a fake result row
    bool isPluginErrorRow(const SearchResult &result)
    {
        return result.fileName.startsWith(u"Jackett: ") && (result.nbSeeders < 0)
            && result.descrLink.contains(u"Jackett", Qt::CaseInsensitive);
    }

    // "Blacked.23.05.13.Molly-Little [XC]" -> "blacked 23 05 13 molly little xc"
    QString normalizedName(const QString &name)
    {
        QString out;
        out.reserve(name.size());
        bool pendingSpace = false;
        for (const QChar c : name)
        {
            if (c.isLetterOrNumber())
            {
                if (pendingSpace && !out.isEmpty())
                    out.append(u' ');
                out.append(c.toLower());
                pendingSpace = false;
            }
            else
            {
                pendingSpace = true;
            }
        }
        return out;
    }

    QString base32ToHex(const QString &base32)
    {
        const QString alphabet = u"ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"_s;
        QByteArray bytes;
        quint64 buffer = 0;
        int bits = 0;
        for (const QChar c : base32.toUpper())
        {
            const qsizetype value = alphabet.indexOf(c);
            if (value < 0)
                return {};
            buffer = (buffer << 5) | static_cast<quint64>(value);
            bits += 5;
            if (bits >= 8)
            {
                bits -= 8;
                bytes.append(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return (bytes.size() == 20) ? QString::fromLatin1(bytes.toHex()) : QString();
    }

    // Info hash from a magnet link, or a 40-character hash inside a download URL
    QString infoHashOf(const QString &url)
    {
        static const QRegularExpression btihRegex {u"btih:([0-9A-Za-z]{32,40})"_s, QRegularExpression::CaseInsensitiveOption};
        if (const QRegularExpressionMatch match = btihRegex.match(url); match.hasMatch())
        {
            const QString hash = match.captured(1);
            if (hash.size() == 40)
                return hash.toLower();
            if (hash.size() == 32)
                return base32ToHex(hash);
        }

        static const QRegularExpression hexRegex {u"(?<![0-9A-Fa-f])[0-9A-Fa-f]{40}(?![0-9A-Fa-f])"_s};
        if (const QRegularExpressionMatch match = hexRegex.match(url); match.hasMatch())
            return match.captured(0).toLower();
        return {};
    }

    bool sizesClose(const qlonglong a, const qlonglong b, const double tolerance)
    {
        if ((a <= 0) || (b <= 0))
            return false;
        return static_cast<double>(std::abs(a - b)) <= (tolerance * static_cast<double>(std::max(a, b)));
    }
    enum DataRole
    {
        LinkVisitedRole = Qt::UserRole + 100
    };

    QColor visitedRowColor()
    {
        return QApplication::palette().color(QPalette::Disabled, QPalette::WindowText);
    }

    QString statusText(const SearchJobWidget::Status st)
    {
        switch (st)
        {
        case SearchJobWidget::Status::Ready:
            break;
        case SearchJobWidget::Status::Ongoing:
            return SearchJobWidget::tr("Searching...");
        case SearchJobWidget::Status::Finished:
            return SearchJobWidget::tr("Search has finished");
        case SearchJobWidget::Status::Aborted:
            return SearchJobWidget::tr("Search aborted");
        case SearchJobWidget::Status::Error:
            return SearchJobWidget::tr("An error occurred during search...");
        case SearchJobWidget::Status::NoResults:
            return SearchJobWidget::tr("Search returned no results");
        }
        return {};
    }
}

using Utils::Misc::SizeUnit;

SearchJobWidget::SearchJobWidget(const QString &id, IGUIApplication *app, QWidget *parent)
    : GUIApplicationComponent(app, parent)
    , m_nameFilteringMode {u"Search/FilteringMode"_s}
    , m_id {id}
    , m_ui {new Ui::SearchJobWidget}
{
    m_ui->setupUi(this);

    loadSettings();

    header()->setFirstSectionMovable(true);
    header()->setStretchLastSection(false);
    header()->setTextElideMode(Qt::ElideRight);

    fillFilterComboBoxes();

    // Set Search results list model
    m_searchListModel = new QStandardItemModel(0, SearchSortModel::NB_SEARCH_COLUMNS, this);
    m_searchListModel->setHeaderData(SearchSortModel::NAME, Qt::Horizontal, tr("Name", "i.e: file name"));
    m_searchListModel->setHeaderData(SearchSortModel::SIZE, Qt::Horizontal, tr("Size", "i.e: file size"));
    m_searchListModel->setHeaderData(SearchSortModel::SEEDS, Qt::Horizontal, tr("Seeders", "i.e: Number of full sources"));
    m_searchListModel->setHeaderData(SearchSortModel::LEECHES, Qt::Horizontal, tr("Leechers", "i.e: Number of partial sources"));
    m_searchListModel->setHeaderData(SearchSortModel::ENGINE_NAME, Qt::Horizontal, tr("Engine"));
    m_searchListModel->setHeaderData(SearchSortModel::ENGINE_URL, Qt::Horizontal, tr("Engine URL"));
    m_searchListModel->setHeaderData(SearchSortModel::PUB_DATE, Qt::Horizontal, tr("Published On"));
    // Set columns text alignment
    m_searchListModel->setHeaderData(SearchSortModel::SIZE, Qt::Horizontal, QVariant(Qt::AlignRight | Qt::AlignVCenter), Qt::TextAlignmentRole);
    m_searchListModel->setHeaderData(SearchSortModel::SEEDS, Qt::Horizontal, QVariant(Qt::AlignRight | Qt::AlignVCenter), Qt::TextAlignmentRole);
    m_searchListModel->setHeaderData(SearchSortModel::LEECHES, Qt::Horizontal, QVariant(Qt::AlignRight | Qt::AlignVCenter), Qt::TextAlignmentRole);

    m_proxyModel = new SearchSortModel(this);
    m_proxyModel->setDynamicSortFilter(true);
    m_proxyModel->setSourceModel(m_searchListModel);
    m_proxyModel->enableNameFilter(m_nameFilteringMode.get(NameFilteringMode::OnlyNames) == NameFilteringMode::OnlyNames);
    m_proxyModel->setSeedsFilter(m_ui->minSeeds->value(), m_ui->maxSeeds->value());
    m_proxyModel->setSizeFilter(sizeInBytes(m_ui->minSize->value(), static_cast<SizeUnit>(m_ui->minSizeUnit->currentIndex()))
        , sizeInBytes(m_ui->maxSize->value(), static_cast<SizeUnit>(m_ui->maxSizeUnit->currentIndex())));

    updateResultsCount();

    m_ui->resultsBrowser->setModel(m_proxyModel);

    m_ui->resultsBrowser->hideColumn(SearchSortModel::DL_LINK); // Hide url column
    m_ui->resultsBrowser->hideColumn(SearchSortModel::DESC_LINK);

    m_ui->resultsBrowser->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_ui->resultsBrowser->setRootIsDecorated(false);
    m_ui->resultsBrowser->setAllColumnsShowFocus(true);
    m_ui->resultsBrowser->setSortingEnabled(true);
    m_ui->resultsBrowser->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Ensure that at least one column is visible at all times
    bool atLeastOne = false;
    for (int i = 0; i < SearchSortModel::DL_LINK; ++i)
    {
        if (!m_ui->resultsBrowser->isColumnHidden(i))
        {
            atLeastOne = true;
            break;
        }
    }
    if (!atLeastOne)
        m_ui->resultsBrowser->setColumnHidden(SearchSortModel::NAME, false);
    // To also mitigate the above issue, we have to resize each column when
    // its size is 0, because explicitly 'showing' the column isn't enough
    // in the above scenario.
    for (int i = 0; i < SearchSortModel::DL_LINK; ++i)
    {
        if ((m_ui->resultsBrowser->columnWidth(i) <= 0) && !m_ui->resultsBrowser->isColumnHidden(i))
            m_ui->resultsBrowser->resizeColumnToContents(i);
    }

    header()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(header(), &QWidget::customContextMenuRequested, this, &SearchJobWidget::displayColumnHeaderMenu);
    connect(header(), &QHeaderView::sectionResized, this, &SearchJobWidget::saveSettings);
    connect(header(), &QHeaderView::sectionMoved, this, &SearchJobWidget::saveSettings);
    connect(header(), &QHeaderView::sortIndicatorChanged, this, &SearchJobWidget::saveSettings);

    m_lineEditSearchResultsFilter = new LineEdit(this);
    m_lineEditSearchResultsFilter->setPlaceholderText(tr("Filter search results..."));
    m_lineEditSearchResultsFilter->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_lineEditSearchResultsFilter, &QWidget::customContextMenuRequested, this, &SearchJobWidget::showFilterContextMenu);
    connect(m_lineEditSearchResultsFilter, &LineEdit::textUpdated, this, &SearchJobWidget::filterSearchResults);
    m_ui->horizontalLayout->insertWidget(0, m_lineEditSearchResultsFilter);

    // Extra filter rules for just this tab's results, on top of the search's rules
    m_resultsFilterBuilder = new SearchFilterBuilder(this, SearchFilterBuilder::Mode::Results);
    m_ui->verticalLayout->insertWidget(1, m_resultsFilterBuilder);
    auto *addResultsFilter = new QPushButton(UIThemeManager::instance()->getIcon(u"list-add"_s), tr("Filter"), this);
    addResultsFilter->setToolTip(tr("Add filter rules for these results only"));
    connect(addResultsFilter, &QPushButton::clicked, m_resultsFilterBuilder, &SearchFilterBuilder::addEmptyRow);
    m_ui->horizontalLayout->insertWidget(1, addResultsFilter);
    connect(m_resultsFilterBuilder, &SearchFilterBuilder::rulesChanged, this, [this]()
    {
        m_proxyModel->setLocalFilterRules(m_resultsFilterBuilder->rules(), m_resultsFilterBuilder->matchAll());
        updateResultsCount();
    });

    connect(m_ui->filterMode, qOverload<int>(&QComboBox::currentIndexChanged), this, &SearchJobWidget::updateNameFilter);
    connect(m_ui->minSeeds, &QAbstractSpinBox::editingFinished, this, &SearchJobWidget::updateSeedsFilter);
    connect(m_ui->minSeeds, qOverload<int>(&QSpinBox::valueChanged), this, &SearchJobWidget::updateSeedsFilter);
    connect(m_ui->maxSeeds, &QAbstractSpinBox::editingFinished, this, &SearchJobWidget::updateSeedsFilter);
    connect(m_ui->maxSeeds, qOverload<int>(&QSpinBox::valueChanged), this, &SearchJobWidget::updateSeedsFilter);
    connect(m_ui->minSize, &QAbstractSpinBox::editingFinished, this, &SearchJobWidget::updateSizeFilter);
    connect(m_ui->minSize, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SearchJobWidget::updateSizeFilter);
    connect(m_ui->maxSize, &QAbstractSpinBox::editingFinished, this, &SearchJobWidget::updateSizeFilter);
    connect(m_ui->maxSize, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SearchJobWidget::updateSizeFilter);
    connect(m_ui->minSizeUnit, qOverload<int>(&QComboBox::currentIndexChanged), this, &SearchJobWidget::updateSizeFilter);
    connect(m_ui->maxSizeUnit, qOverload<int>(&QComboBox::currentIndexChanged), this, &SearchJobWidget::updateSizeFilter);

    connect(m_ui->resultsBrowser, &QAbstractItemView::doubleClicked, this, &SearchJobWidget::onItemDoubleClicked);

    connect(UIThemeManager::instance(), &UIThemeManager::themeChanged, this, &SearchJobWidget::onUIThemeChanged);
}

SearchJobWidget::SearchJobWidget(const QString &id, const QString &searchPattern
        , const QList<SearchResult> &searchResults, IGUIApplication *app, QWidget *parent)
    : SearchJobWidget(id, app, parent)
{
    m_searchPattern = searchPattern;
    m_proxyModel->setNameFilter(m_searchPattern);

    appendSearchResults(searchResults);
}

SearchJobWidget::~SearchJobWidget()
{
    saveSettings();
    delete m_ui;
}

QString SearchJobWidget::id() const
{
    return m_id;
}

QString SearchJobWidget::searchPattern() const
{
    return m_searchPattern;
}

QList<SearchResult> SearchJobWidget::searchResults() const
{
    return m_searchResults;
}

void SearchJobWidget::onItemDoubleClicked(const QModelIndex &index)
{
    downloadTorrent(index);
}

QHeaderView *SearchJobWidget::header() const
{
    return m_ui->resultsBrowser->header();
}

// Set the color of a row in data model
void SearchJobWidget::setRowColor(int row, const QColor &color)
{
    for (int i = 0; i < m_proxyModel->columnCount(); ++i)
        m_proxyModel->setData(m_proxyModel->index(row, i), color, Qt::ForegroundRole);
}

void SearchJobWidget::setRowVisited(const int row)
{
    m_proxyModel->setDynamicSortFilter(false);

    m_proxyModel->setData(m_proxyModel->index(row, 0), true, LinkVisitedRole);
    setRowColor(row, visitedRowColor());

    m_proxyModel->setDynamicSortFilter(true);
}

void SearchJobWidget::onUIThemeChanged()
{
    m_proxyModel->setDynamicSortFilter(false);

    for (int row = 0; row < m_proxyModel->rowCount(); ++row)
    {
        const QVariant userData = m_proxyModel->data(m_proxyModel->index(row, 0), LinkVisitedRole);
        const bool isVisited = userData.toBool();
        if (isVisited)
            setRowColor(row, visitedRowColor());
    }

    m_proxyModel->setDynamicSortFilter(true);
}

SearchJobWidget::Status SearchJobWidget::status() const
{
    return m_status;
}

int SearchJobWidget::visibleResultsCount() const
{
    return m_proxyModel->rowCount();
}

LineEdit *SearchJobWidget::lineEditSearchResultsFilter() const
{
    return m_lineEditSearchResultsFilter;
}

void SearchJobWidget::runSearches(const QString &pattern, const QStringList &queries
        , const QString &category, const QStringList &plugins)
{
    // stop whatever this tab was running
    m_pendingQueries.clear();
    for (SearchHandler *handler : asConst(m_searchHandlers))
    {
        handler->disconnect(this);
        handler->cancelSearch();
        handler->deleteLater();
    }
    m_searchHandlers.clear();

    m_searchResults.clear();
    clearResults();

    m_searchPattern = pattern;
    m_proxyModel->setNameFilter(m_searchPattern);

    m_queries = queries.isEmpty() ? QStringList {pattern} : queries;
    m_pendingQueries = m_queries;
    m_category = category;
    m_plugins = plugins;
    m_failedSearches = 0;
    m_searchCancelled = false;

    updateResultsCount();
    setStatus(Status::Ongoing);
    startPendingSearches();
}

void SearchJobWidget::startPendingSearches()
{
    // A few at a time, so the sites (and Cloudflare) aren't hit with every variant at once
    const int maxRunning = 3;
    while (!m_searchCancelled && !m_pendingQueries.isEmpty() && (m_searchHandlers.size() < maxRunning))
    {
        SearchHandler *handler = SearchPluginManager::instance()->startSearch(m_pendingQueries.takeFirst(), m_category, m_plugins);
        handler->setParent(this);
        m_searchHandlers.append(handler);
        connect(handler, &SearchHandler::newSearchResults, this, &SearchJobWidget::appendSearchResults);
        connect(handler, &SearchHandler::searchFinished, this, [this, handler]() { onSearchDone(handler, false); });
        connect(handler, &SearchHandler::searchFailed, this, [this, handler]() { onSearchDone(handler, true); });
    }
}

void SearchJobWidget::onSearchDone(SearchHandler *handler, const bool failed)
{
    // a failing process can report both "failed" and "finished"
    if (!m_searchHandlers.removeOne(handler))
        return;

    handler->disconnect(this);
    handler->deleteLater();
    if (failed)
        ++m_failedSearches;

    startPendingSearches();

    if (m_searchHandlers.isEmpty() && m_pendingQueries.isEmpty())
    {
        if (m_searchCancelled)
            setStatus(Status::Aborted);
        else if (m_failedSearches >= m_queries.size())
            setStatus(Status::Error);
        else if (m_noSearchResults)
            setStatus(Status::NoResults);
        else
            setStatus(Status::Finished);
    }
    updateResultsCount();
}

void SearchJobWidget::cancelSearch()
{
    if (m_searchHandlers.isEmpty() && m_pendingQueries.isEmpty())
        return;

    m_searchCancelled = true;
    m_pendingQueries.clear();
    const QList<SearchHandler *> running = m_searchHandlers;
    for (SearchHandler *handler : running)
        handler->cancelSearch();
    setStatus(Status::Aborted);
}

void SearchJobWidget::applySearchOptions()
{
    m_proxyModel->refreshSynonyms();
    clearResults();
    addResultsToModel(m_searchResults);
}

void SearchJobWidget::downloadTorrents(const AddTorrentOption option)
{
    const QModelIndexList rows = m_ui->resultsBrowser->selectionModel()->selectedRows();
    for (const QModelIndex &rowIndex : rows)
        downloadTorrent(rowIndex, option);
}

void SearchJobWidget::openTorrentPages()
{
    const QModelIndexList rows = m_ui->resultsBrowser->selectionModel()->selectedRows();
    qsizetype emptyLinkCount = 0;
    qsizetype badLinkCount = 0;
    QString warningEntryName;
    for (const QModelIndex &rowIndex : rows)
    {
        const QString entryName = m_proxyModel->index(rowIndex.row(), SearchSortModel::NAME).data().toString();
        const QString descrLink = m_proxyModel->index(rowIndex.row(), SearchSortModel::DESC_LINK).data().toString();

        const QUrl descrLinkURL {descrLink};
        if (descrLinkURL.isEmpty()) [[unlikely]]
        {
            if (warningEntryName.isEmpty())
                warningEntryName = entryName;
            ++emptyLinkCount;
        }
        else if (descrLinkURL.isLocalFile()) [[unlikely]]
        {
            if (badLinkCount == 0)
                warningEntryName = entryName;
            ++badLinkCount;

            LogMsg(tr("Blocked opening search result description page URL. URL pointing to local file might be malicious behaviour. Name: \"%1\". URL: \"%2\".")
                    .arg(entryName, descrLink), Log::WARNING);
        }
        else [[likely]]
        {
            QDesktopServices::openUrl(descrLinkURL);
        }
    }

    if (badLinkCount > 0)
    {
        QString message = tr("Blocked opening search result description page URL. The following result URL is pointing to local file and it may be malicious behaviour:\n%1").arg(warningEntryName);
        if (badLinkCount > 1)
            message.append(u"\n" + tr("There are %1 more results with the same issue.").arg(badLinkCount - 1));
        QMessageBox::warning(this, u"qBittorrent"_s, message, QMessageBox::Ok);
    }
    else if (emptyLinkCount > 0)
    {
        QString message = tr("Entry \"%1\" has no description page URL provided.").arg(warningEntryName);
        if (emptyLinkCount > 1)
            message.append(u"\n" + tr("There are %1 more entries with the same issue.").arg(emptyLinkCount - 1));
        QMessageBox::warning(this, u"qBittorrent"_s, message, QMessageBox::Ok);
    }
}

void SearchJobWidget::copyTorrentURLs() const
{
    copyField(SearchSortModel::DESC_LINK);
}

void SearchJobWidget::copyTorrentDownloadLinks() const
{
    copyField(SearchSortModel::DL_LINK);
}

void SearchJobWidget::copyTorrentNames() const
{
    copyField(SearchSortModel::NAME);
}

void SearchJobWidget::copyField(const int column) const
{
    const QModelIndexList rows {m_ui->resultsBrowser->selectionModel()->selectedRows()};
    QStringList list;

    for (const QModelIndex &rowIndex : rows)
    {
        const QString field = m_proxyModel->data(
            m_proxyModel->index(rowIndex.row(), column)).toString();
        if (!field.isEmpty())
            list << field;
    }

    if (!list.empty())
        QApplication::clipboard()->setText(list.join(u'\n'));
}

void SearchJobWidget::setStatus(const Status value)
{
    if (m_status == value)
        return;

    m_status = value;
    setStatusTip(statusText(value));
    emit statusChanged();
}

void SearchJobWidget::downloadTorrent(const QModelIndex &rowIndex, const AddTorrentOption option)
{
    const QString torrentUrl = m_proxyModel->data(
                m_proxyModel->index(rowIndex.row(), SearchSortModel::DL_LINK)).toString();
    const QString engineName = m_proxyModel->data(
                m_proxyModel->index(rowIndex.row(), SearchSortModel::ENGINE_NAME), SearchSortModel::UnderlyingDataRole).toString();

    if (torrentUrl.startsWith(u"magnet:", Qt::CaseInsensitive))
    {
        addTorrentToSession(torrentUrl, option);
    }
    else
    {
        SearchDownloadHandler *downloadHandler = SearchPluginManager::instance()->downloadTorrent(engineName, torrentUrl);
        connect(downloadHandler, &SearchDownloadHandler::downloadFinished, this
            , [this, downloadHandler, option](const QString &source, [[maybe_unused]] const QString &errorMessage)
        {
            addTorrentToSession(source, option);
            downloadHandler->deleteLater();
        });
    }

    setRowVisited(rowIndex.row());
}

void SearchJobWidget::addTorrentToSession(const QString &source, const AddTorrentOption option)
{
    app()->addTorrentManager()->addTorrent(source, {}, option);
}

void SearchJobWidget::updateResultsCount()
{
    const int totalResults = m_searchListModel->rowCount();
    const int filteredResults = m_proxyModel->rowCount();
    if (m_queries.size() > 1)
    {
        m_ui->resultsLbl->setText(tr("Results (showing <i>%1</i> out of <i>%2</i>, from %3 searches):")
                                  .arg(filteredResults).arg(totalResults).arg(m_queries.size()));
    }
    else
    {
        m_ui->resultsLbl->setText(tr("Results (showing <i>%1</i> out of <i>%2</i>):", "i.e: Search results")
                                  .arg(filteredResults).arg(totalResults));
    }
    QStringList tip;
    if (m_queries.size() > 1)
    {
        tip.append(tr("Searches sent to the sites:"));
        for (const QString &query : asConst(m_queries))
            tip.append(u"  \u2022 "_s + query);
    }
    if (m_mergedCount > 0)
        tip.append(tr("%1 duplicate listings merged into other rows").arg(m_mergedCount));
    m_ui->resultsLbl->setToolTip(tip.join(u'\n'));

    m_noSearchResults = (totalResults == 0);
    emit resultsCountUpdated();
}

void SearchJobWidget::updateNameFilter()
{
    const auto filteringMode = static_cast<NameFilteringMode>(m_ui->filterMode->itemData(m_ui->filterMode->currentIndex()).toInt());
    m_proxyModel->enableNameFilter(filteringMode == NameFilteringMode::OnlyNames);
    m_nameFilteringMode = filteringMode;

    updateResultsCount();
}

void SearchJobWidget::setFilterRules(const QList<SearchSortModel::FilterRule> &rules, const bool matchAll)
{
    m_proxyModel->setFilterRules(rules, matchAll);
    updateResultsCount();
}

void SearchJobWidget::updateSeedsFilter()
{
    // we update size and seeds filter parameters in the model even if they are disabled
    m_proxyModel->setSeedsFilter(m_ui->minSeeds->value(), m_ui->maxSeeds->value());

    updateResultsCount();
}

void SearchJobWidget::updateSizeFilter()
{
    // we update size and seeds filter parameters in the model even if they are disabled
    m_proxyModel->setSizeFilter(sizeInBytes(m_ui->minSize->value(), static_cast<SizeUnit>(m_ui->minSizeUnit->currentIndex()))
        , sizeInBytes(m_ui->maxSize->value(), static_cast<SizeUnit>(m_ui->maxSizeUnit->currentIndex())));

    updateResultsCount();
}

void SearchJobWidget::fillFilterComboBoxes()
{
    using Utils::Misc::unitString;

    QStringList unitStrings;
    unitStrings.append(unitString(SizeUnit::Byte));
    unitStrings.append(unitString(SizeUnit::KibiByte));
    unitStrings.append(unitString(SizeUnit::MebiByte));
    unitStrings.append(unitString(SizeUnit::GibiByte));
    unitStrings.append(unitString(SizeUnit::TebiByte));
    unitStrings.append(unitString(SizeUnit::PebiByte));
    unitStrings.append(unitString(SizeUnit::ExbiByte));

    m_ui->minSizeUnit->clear();
    m_ui->maxSizeUnit->clear();
    m_ui->minSizeUnit->addItems(unitStrings);
    m_ui->maxSizeUnit->addItems(unitStrings);

    m_ui->minSize->setValue(0);
    m_ui->minSizeUnit->setCurrentIndex(static_cast<int>(SizeUnit::MebiByte));

    m_ui->maxSize->setValue(-1);
    m_ui->maxSizeUnit->setCurrentIndex(static_cast<int>(SizeUnit::GibiByte));

    m_ui->filterMode->clear();

    m_ui->filterMode->addItem(tr("Torrent names only"), static_cast<int>(NameFilteringMode::OnlyNames));
    m_ui->filterMode->addItem(tr("Everywhere"), static_cast<int>(NameFilteringMode::Everywhere));
    const auto selectedFilteringMode = static_cast<int>(m_nameFilteringMode.get(NameFilteringMode::OnlyNames));
    const int index = m_ui->filterMode->findData(selectedFilteringMode);
    m_ui->filterMode->setCurrentIndex((index == -1) ? 0 : index);
}

void SearchJobWidget::filterSearchResults(const QString &name)
{
    const QString pattern = (Preferences::instance()->getRegexAsFilteringPatternForSearchJob()
        ? name : Utils::String::wildcardToRegexPattern(name));
    m_proxyModel->setFilterRegularExpression(QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption));
    updateResultsCount();
}

void SearchJobWidget::showFilterContextMenu()
{
    const Preferences *pref = Preferences::instance();

    QMenu *menu = m_lineEditSearchResultsFilter->createStandardContextMenu();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->addSeparator();

    QAction *useRegexAct = menu->addAction(tr("Use regular expressions"));
    useRegexAct->setCheckable(true);
    useRegexAct->setChecked(pref->getRegexAsFilteringPatternForSearchJob());
    connect(useRegexAct, &QAction::toggled, pref, &Preferences::setRegexAsFilteringPatternForSearchJob);
    connect(useRegexAct, &QAction::toggled, this, [this]() { filterSearchResults(m_lineEditSearchResultsFilter->text()); });

    menu->popup(QCursor::pos());
}

void SearchJobWidget::contextMenuEvent(QContextMenuEvent *event)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    menu->addAction(UIThemeManager::instance()->getIcon(u"download"_s)
        , tr("Open download window"), this, [this]() { downloadTorrents(AddTorrentOption::ShowDialog); });
    menu->addAction(UIThemeManager::instance()->getIcon(u"downloading"_s, u"download"_s)
        , tr("Download"), this, [this]() { downloadTorrents(AddTorrentOption::SkipDialog); });
    menu->addSeparator();
    menu->addAction(UIThemeManager::instance()->getIcon(u"application-url"_s), tr("Open description page")
        , this, &SearchJobWidget::openTorrentPages);

    QMenu *copySubMenu = menu->addMenu(
        UIThemeManager::instance()->getIcon(u"edit-copy"_s), tr("Copy"));

    copySubMenu->addAction(UIThemeManager::instance()->getIcon(u"name"_s, u"edit-copy"_s), tr("Name")
        , this, &SearchJobWidget::copyTorrentNames);
    copySubMenu->addAction(UIThemeManager::instance()->getIcon(u"insert-link"_s, u"edit-copy"_s), tr("Download link")
        , this, &SearchJobWidget::copyTorrentDownloadLinks);
    copySubMenu->addAction(UIThemeManager::instance()->getIcon(u"application-url"_s, u"edit-copy"_s), tr("Description page URL")
        , this, &SearchJobWidget::copyTorrentURLs);

    menu->popup(event->globalPos());
}

void SearchJobWidget::loadSettings()
{
    header()->restoreState(Preferences::instance()->getSearchTabHeaderState());
}

void SearchJobWidget::saveSettings() const
{
    Preferences::instance()->setSearchTabHeaderState(header()->saveState());
}

int SearchJobWidget::visibleColumnsCount() const
{
    int count = 0;
    for (int i = 0, iMax = m_ui->resultsBrowser->header()->count(); i < iMax; ++i)
    {
        if (!m_ui->resultsBrowser->isColumnHidden(i))
            ++count;
    }

    return count;
}

void SearchJobWidget::displayColumnHeaderMenu()
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setTitle(tr("Column visibility"));
    menu->setToolTipsVisible(true);

    for (int i = 0; i < SearchSortModel::DL_LINK; ++i)
    {
        const auto columnName = m_searchListModel->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString();
        QAction *action = menu->addAction(columnName, this, [this, i](const bool checked)
        {
            if (!checked && (visibleColumnsCount() <= 1))
                return;

            m_ui->resultsBrowser->setColumnHidden(i, !checked);

            if (checked && (m_ui->resultsBrowser->columnWidth(i) <= 5))
                m_ui->resultsBrowser->resizeColumnToContents(i);

            saveSettings();
        });
        action->setCheckable(true);
        action->setChecked(!m_ui->resultsBrowser->isColumnHidden(i));
    }

    menu->addSeparator();
    QAction *resizeAction = menu->addAction(tr("Resize columns"), this, [this]()
    {
        for (int i = 0, count = m_ui->resultsBrowser->header()->count(); i < count; ++i)
        {
            if (!m_ui->resultsBrowser->isColumnHidden(i))
                m_ui->resultsBrowser->resizeColumnToContents(i);
        }
        saveSettings();
    });
    resizeAction->setToolTip(tr("Resize all non-hidden columns to the size of their contents"));

    menu->popup(QCursor::pos());
}

void SearchJobWidget::appendSearchResults(const QList<SearchResult> &results)
{
    QList<SearchResult> kept;
    kept.reserve(results.size());
    for (const SearchResult &result : results)
    {
        if (!isPluginErrorRow(result))
            kept.append(result);
    }
    m_searchResults.append(kept);
    addResultsToModel(kept);
}

void SearchJobWidget::addResultsToModel(const QList<SearchResult> &results)
{
    const bool merge = SearchSynonyms::mergeDuplicates();
    for (const SearchResult &result : results)
    {
        const QString normName = normalizedName(result.fileName);
        const QString infoHash = infoHashOf(result.fileUrl);
        if (merge)
        {
            if (const int row = findDuplicateRow(normName, infoHash, result.fileSize); row >= 0)
            {
                mergeIntoRow(row, result, normName, infoHash);
                continue;
            }
        }
        addResultRow(result, normName, infoHash);
    }
    updateResultsCount();
}

void SearchJobWidget::addResultRow(const SearchResult &result, const QString &normName, const QString &infoHash)
{
    const int row = m_searchListModel->rowCount();
    m_searchListModel->insertRow(row);

    const auto setModelData = [this, row] (const int column, const QString &displayData
            , const QVariant &underlyingData, const Qt::Alignment textAlignmentData = {})
    {
        const QMap<int, QVariant> data =
        {
            {Qt::DisplayRole, displayData},
            {SearchSortModel::UnderlyingDataRole, underlyingData},
            {Qt::TextAlignmentRole, QVariant {textAlignmentData}}
        };
        m_searchListModel->setItemData(m_searchListModel->index(row, column), data);
    };

    setModelData(SearchSortModel::NAME, result.fileName, result.fileName);
    setModelData(SearchSortModel::DL_LINK, result.fileUrl, result.fileUrl);
    setModelData(SearchSortModel::ENGINE_NAME, result.engineName, result.engineName);
    setModelData(SearchSortModel::ENGINE_URL, result.siteUrl, result.siteUrl);
    setModelData(SearchSortModel::DESC_LINK, result.descrLink, result.descrLink);
    setModelData(SearchSortModel::SIZE, Utils::Misc::friendlyUnit(result.fileSize), result.fileSize, (Qt::AlignRight | Qt::AlignVCenter));
    setModelData(SearchSortModel::SEEDS, QString::number(result.nbSeeders), result.nbSeeders, (Qt::AlignRight | Qt::AlignVCenter));
    setModelData(SearchSortModel::LEECHES, QString::number(result.nbLeechers), result.nbLeechers, (Qt::AlignRight | Qt::AlignVCenter));
    setModelData(SearchSortModel::PUB_DATE, QLocale().toString(result.pubDate.toLocalTime(), QLocale::ShortFormat), result.pubDate);

    RowInfo info;
    info.normName = normName;
    info.infoHash = infoHash;
    info.size = result.fileSize;
    info.seeds = result.nbSeeders;
    info.engine = result.engineName;
    info.sources = {result.engineName};
    info.sourceSeeds = {result.nbSeeders};
    info.listings = {result.engineName + u'\n' + result.fileUrl};
    m_rowInfo.append(info);

    if (!infoHash.isEmpty())
        m_rowByHash.insert(infoHash, row);
    if (normName.size() >= MIN_MERGE_NAME_LENGTH)
        m_rowsByNameKey[normName.left(NAME_KEY_LENGTH)].append(row);
}

int SearchJobWidget::findDuplicateRow(const QString &normName, const QString &infoHash, const qlonglong size) const
{
    if (!infoHash.isEmpty())
    {
        if (const auto it = m_rowByHash.constFind(infoHash); it != m_rowByHash.cend())
            return it.value();
    }

    if (normName.size() < MIN_MERGE_NAME_LENGTH)
        return -1;

    for (const int row : m_rowsByNameKey.value(normName.left(NAME_KEY_LENGTH)))
    {
        const RowInfo &info = m_rowInfo.at(row);
        // different torrent files are kept apart, even under the same name (fakes copy names)
        if (!infoHash.isEmpty() && !info.infoHash.isEmpty() && (info.infoHash != infoHash))
            continue;

        if (info.normName == normName)
        {
            if ((size <= 0) || (info.size <= 0) || sizesClose(info.size, size, 0.08))
                return row;
            continue;
        }

        // one site cut the name short: "...MP4-P2P [X" vs "...MP4-P2P [XC]"
        const bool newIsShorter = (normName.size() < info.normName.size());
        const QString &shorter = newIsShorter ? normName : info.normName;
        const QString &longer = newIsShorter ? info.normName : normName;
        if ((shorter.size() >= 25) && ((shorter.size() * 10) >= (longer.size() * 6))
            && longer.startsWith(shorter) && sizesClose(info.size, size, 0.03))
        {
            return row;
        }
    }
    return -1;
}

void SearchJobWidget::mergeIntoRow(const int row, const SearchResult &result, const QString &normName, const QString &infoHash)
{
    RowInfo &info = m_rowInfo[row];
    ++m_mergedCount;

    // the same listing, found again by another of this tab's searches
    const QString listingKey = result.engineName + u'\n' + result.fileUrl;
    if (info.listings.contains(listingKey))
        return;
    info.listings.insert(listingKey);
    info.sources.append(result.engineName);
    info.sourceSeeds.append(result.nbSeeders);

    if (!infoHash.isEmpty() && info.infoHash.isEmpty())
    {
        info.infoHash = infoHash;
        m_rowByHash.insert(infoHash, row);
    }

    const auto setData = [this, row](const int column, const QString &display, const QVariant &underlying)
    {
        const QModelIndex index = m_searchListModel->index(row, column);
        m_searchListModel->setData(index, display, Qt::DisplayRole);
        m_searchListModel->setData(index, underlying, SearchSortModel::UnderlyingDataRole);
    };

    // keep the complete name when a site cut it short
    if ((normName.size() > info.normName.size()) && normName.startsWith(info.normName))
    {
        setData(SearchSortModel::NAME, result.fileName, result.fileName);
        info.normName = normName;
    }

    // the best-seeded listing is the one used for downloading
    if (result.nbSeeders > info.seeds)
    {
        info.seeds = result.nbSeeders;
        info.engine = result.engineName;
        setData(SearchSortModel::DL_LINK, result.fileUrl, result.fileUrl);
        setData(SearchSortModel::ENGINE_URL, result.siteUrl, result.siteUrl);
        setData(SearchSortModel::DESC_LINK, result.descrLink, result.descrLink);
        setData(SearchSortModel::SEEDS, QString::number(result.nbSeeders), result.nbSeeders);
        setData(SearchSortModel::LEECHES, QString::number(result.nbLeechers), result.nbLeechers);
        if (result.fileSize > 0)
        {
            info.size = result.fileSize;
            setData(SearchSortModel::SIZE, Utils::Misc::friendlyUnit(result.fileSize), result.fileSize);
        }
        if (result.pubDate.isValid())
            setData(SearchSortModel::PUB_DATE, QLocale().toString(result.pubDate.toLocalTime(), QLocale::ShortFormat), result.pubDate);
    }

    updateSourcesDisplay(row);
}

void SearchJobWidget::updateSourcesDisplay(const int row)
{
    const RowInfo &info = m_rowInfo.at(row);
    const QModelIndex index = m_searchListModel->index(row, SearchSortModel::ENGINE_NAME);

    QStringList sites;
    for (const QString &site : info.sources)
    {
        if (!sites.contains(site))
            sites.append(site);
    }
    const qsizetype otherSites = sites.size() - 1;
    const qsizetype listings = info.sources.size();

    QString display = info.engine;
    if (otherSites > 0)
        display = tr("%1 +%2").arg(info.engine).arg(otherSites);
    else if (listings > 1)
        display = tr("%1 (%2 uploads)").arg(info.engine).arg(listings);

    m_searchListModel->setData(index, display, Qt::DisplayRole);
    m_searchListModel->setData(index, info.engine, SearchSortModel::UnderlyingDataRole);
    m_searchListModel->setData(index, sites, SearchSortModel::SourcesRole);

    QString tooltip;
    if (listings > 1)
    {
        QStringList lines {tr("Listed %1 times. Downloads use the one with the most seeders.").arg(listings)};
        for (qsizetype i = 0; i < listings; ++i)
            lines.append(tr("%1: %2 seeders").arg(info.sources[i]).arg(info.sourceSeeds[i]));
        tooltip = lines.join(u'\n');
    }
    m_searchListModel->setData(index, tooltip, Qt::ToolTipRole);
}

void SearchJobWidget::clearResults()
{
    m_searchListModel->removeRows(0, m_searchListModel->rowCount());
    m_rowInfo.clear();
    m_rowByHash.clear();
    m_rowsByNameKey.clear();
    m_mergedCount = 0;
}

void SearchJobWidget::keyPressEvent(QKeyEvent *event)
{
    switch (event->key())
    {
    case Qt::Key_Enter:
    case Qt::Key_Return:
        downloadTorrents();
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}
