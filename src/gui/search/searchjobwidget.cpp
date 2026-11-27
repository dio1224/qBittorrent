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
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QSpinBox>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QPushButton>
#include <QRegularExpression>
#include <QMessageBox>
#include <QPalette>
#include <QStandardItemModel>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QProcess>
#include <QJsonObject>
#include <QJsonDocument>
#include <QUrl>

#include "base/logger.h"
#include "base/path.h"
#include "base/search/dioextras.h"
#include "base/preferences.h"
#include "base/search/searchdownloadhandler.h"
#include "base/search/searchhandler.h"
#include "base/search/searchpluginmanager.h"
#include "base/settingsstorage.h"
#include "base/utils/foreignapps.h"
#include "base/utils/misc.h"
#include "gui/desktopintegration.h"
#include "gui/interfaces/iguiapplication.h"
#include "gui/lineedit.h"
#include "gui/uithememanager.h"
#include "dioextrasmenu.h"
#include "plexlibrary.h"
#include "searchfilterbuilder.h"
#include "searchsynonyms.h"
#include "searchsortmodel.h"
#include "ui_searchjobwidget.h"

namespace
{

    // The last multi-column sort used, so new tabs open sorted the same way
    const QString SORT_KEYS_KEY = u"Search/Dio/SortKeys"_s;

    bool isTextColumn(const int column)
    {
        return (column == SearchSortModel::NAME) || (column == SearchSortModel::ENGINE_NAME)
            || (column == SearchSortModel::ENGINE_URL) || (column == SearchSortModel::CODEC);
    }

    // What the two directions mean for a column, in words
    QString orderLabel(const int column, const Qt::SortOrder order)
    {
        const bool asc = (order == Qt::AscendingOrder);
        if (isTextColumn(column))
            return asc ? SearchJobWidget::tr("A to Z") : SearchJobWidget::tr("Z to A");
        if (column == SearchSortModel::PUB_DATE)
            return asc ? SearchJobWidget::tr("Oldest first") : SearchJobWidget::tr("Newest first");
        if ((column == SearchSortModel::SIZE) || (column == SearchSortModel::SEEDS) || (column == SearchSortModel::LEECHES))
            return asc ? SearchJobWidget::tr("Smallest to largest") : SearchJobWidget::tr("Largest to smallest");
        return asc ? SearchJobWidget::tr("Lowest first") : SearchJobWidget::tr("Highest first");
    }

    // Excel-style "Sort" dialog: Sort by, Then by, Then by...
    class SortDialog final : public QDialog
    {
    public:
        SortDialog(const QList<SearchSortModel::SortKey> &keys, const QList<std::pair<int, QString>> &columns, QWidget *parent)
            : QDialog(parent)
            , m_columns {columns}
        {
            setWindowTitle(SearchJobWidget::tr("Sort results"));
            auto *layout = new QVBoxLayout(this);
            auto *hint = new QLabel(SearchJobWidget::tr("Rows are sorted by the first column; the next ones only decide the order of rows that tie."), this);
            hint->setWordWrap(true);
            layout->addWidget(hint);
            m_rowsLayout = new QVBoxLayout;
            layout->addLayout(m_rowsLayout);

            auto *addButton = new QPushButton(SearchJobWidget::tr("Add level"), this);
            connect(addButton, &QPushButton::clicked, this, [this]()
            {
                // the first column not used yet
                for (const auto &[column, title] : m_columns)
                {
                    if (!usedColumns().contains(column))
                    {
                        addRow({column, isTextColumn(column) ? Qt::AscendingOrder : Qt::DescendingOrder,
                            SearchSortModel::supportsTolerance(column) ? SearchSortModel::DEFAULT_TOLERANCE : 0});
                        return;
                    }
                }
            });
            auto *addRowLayout = new QHBoxLayout;
            addRowLayout->addWidget(addButton);
            addRowLayout->addStretch();
            layout->addLayout(addRowLayout);

            auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
            connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
            layout->addWidget(buttons);

            for (const SearchSortModel::SortKey &key : keys)
                addRow(key);
            if (m_rows.isEmpty())
                addRow({SearchSortModel::SEEDS, Qt::DescendingOrder, SearchSortModel::DEFAULT_TOLERANCE});
            setMinimumWidth(520);
        }

        QList<SearchSortModel::SortKey> keys() const
        {
            QList<SearchSortModel::SortKey> result;
            QSet<int> seen;
            for (const Row &row : m_rows)
            {
                const int column = row.column->currentData().toInt();
                if (seen.contains(column))
                    continue;  // a column used twice only counts the first time
                seen.insert(column);
                const double tolerance = SearchSortModel::supportsTolerance(column) ? (row.close->value() / 100.0) : 0;
                result.append({column, static_cast<Qt::SortOrder>(row.order->currentData().toInt()), tolerance});
            }
            return result;
        }

    private:
        struct Row
        {
            QWidget *widget = nullptr;
            QLabel *label = nullptr;
            QComboBox *column = nullptr;
            QComboBox *order = nullptr;
            QWidget *closeBox = nullptr;
            QSpinBox *close = nullptr;
        };

        QSet<int> usedColumns() const
        {
            QSet<int> used;
            for (const Row &row : m_rows)
                used.insert(row.column->currentData().toInt());
            return used;
        }

        void fillOrders(const Row &row, const Qt::SortOrder order)
        {
            const int column = row.column->currentData().toInt();
            row.order->clear();
            row.order->addItem(orderLabel(column, Qt::AscendingOrder), static_cast<int>(Qt::AscendingOrder));
            row.order->addItem(orderLabel(column, Qt::DescendingOrder), static_cast<int>(Qt::DescendingOrder));
            row.order->setCurrentIndex((order == Qt::AscendingOrder) ? 0 : 1);
        }

        void addRow(const SearchSortModel::SortKey &key)
        {
            Row row;
            row.widget = new QWidget(this);
            auto *rowLayout = new QHBoxLayout(row.widget);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            row.label = new QLabel(row.widget);
            row.label->setMinimumWidth(60);
            row.column = new QComboBox(row.widget);
            for (const auto &[column, title] : m_columns)
                row.column->addItem(title, column);
            row.column->setCurrentIndex(std::max(0, row.column->findData(key.column)));
            row.order = new QComboBox(row.widget);
            fillOrders(row, key.order);
            row.closeBox = new QWidget(row.widget);
            auto *closeLayout = new QHBoxLayout(row.closeBox);
            closeLayout->setContentsMargins(0, 0, 0, 0);
            closeLayout->addWidget(new QLabel(SearchJobWidget::tr("same if within"), row.closeBox));
            row.close = new QSpinBox(row.closeBox);
            row.close->setRange(0, 50);
            row.close->setSuffix(u"%"_s);
            row.close->setValue(qRound(key.tolerance * 100));
            row.close->setToolTip(SearchJobWidget::tr("Values this close to each other count as equal here, so the next level decides their order.\n0% means only exact ties."));
            closeLayout->addWidget(row.close);
            auto *up = new QToolButton(row.widget);
            up->setText(u"\u25B2"_s);
            up->setToolTip(SearchJobWidget::tr("Move up"));
            auto *down = new QToolButton(row.widget);
            down->setText(u"\u25BC"_s);
            down->setToolTip(SearchJobWidget::tr("Move down"));
            auto *remove = new QToolButton(row.widget);
            remove->setText(u"\u2715"_s);
            remove->setToolTip(SearchJobWidget::tr("Delete level"));
            rowLayout->addWidget(row.label);
            rowLayout->addWidget(row.column, 2);
            rowLayout->addWidget(row.order, 2);
            rowLayout->addWidget(row.closeBox);
            rowLayout->addWidget(up);
            rowLayout->addWidget(down);
            rowLayout->addWidget(remove);
            m_rowsLayout->addWidget(row.widget);
            m_rows.append(row);

            QWidget *widget = row.widget;
            connect(row.column, &QComboBox::currentIndexChanged, this, [this, widget]()
            {
                const Row &r = m_rows[indexOf(widget)];
                // keep the direction, relabelled for the new column
                fillOrders(r, static_cast<Qt::SortOrder>(r.order->currentData().toInt()));
                if (r.close->value() == 0)
                    r.close->setValue(qRound(SearchSortModel::DEFAULT_TOLERANCE * 100));
                refreshLabels();
            });
            connect(up, &QToolButton::clicked, this, [this, widget]() { swapRows(indexOf(widget), indexOf(widget) - 1); });
            connect(down, &QToolButton::clicked, this, [this, widget]() { swapRows(indexOf(widget), indexOf(widget) + 1); });
            connect(remove, &QToolButton::clicked, this, [this, widget]()
            {
                if (m_rows.size() <= 1)
                    return;
                const int i = indexOf(widget);
                m_rows.takeAt(i).widget->deleteLater();
                refreshLabels();
            });
            refreshLabels();
        }

        int indexOf(const QWidget *widget) const
        {
            for (int i = 0; i < m_rows.size(); ++i)
            {
                if (m_rows[i].widget == widget)
                    return i;
            }
            return -1;
        }

        void swapRows(const int a, const int b)
        {
            if ((a < 0) || (b < 0) || (a >= m_rows.size()) || (b >= m_rows.size()))
                return;
            const int columnA = m_rows[a].column->currentData().toInt();
            const auto orderA = static_cast<Qt::SortOrder>(m_rows[a].order->currentData().toInt());
            const int columnB = m_rows[b].column->currentData().toInt();
            const auto orderB = static_cast<Qt::SortOrder>(m_rows[b].order->currentData().toInt());
            const int closeA = m_rows[a].close->value();
            m_rows[a].close->setValue(m_rows[b].close->value());
            m_rows[b].close->setValue(closeA);
            m_rows[a].column->setCurrentIndex(m_rows[a].column->findData(columnB));
            fillOrders(m_rows[a], orderB);
            m_rows[b].column->setCurrentIndex(m_rows[b].column->findData(columnA));
            fillOrders(m_rows[b], orderA);
        }

        void refreshLabels()
        {
            for (int i = 0; i < m_rows.size(); ++i)
            {
                m_rows[i].label->setText((i == 0) ? SearchJobWidget::tr("Sort by") : SearchJobWidget::tr("Then by"));
                // "close enough" only matters when a later level breaks the ties
                const bool numeric = SearchSortModel::supportsTolerance(m_rows[i].column->currentData().toInt());
                m_rows[i].closeBox->setVisible(numeric);
                m_rows[i].closeBox->setEnabled(i < (m_rows.size() - 1));
            }
        }

        QList<std::pair<int, QString>> m_columns;
        QVBoxLayout *m_rowsLayout = nullptr;
        QList<Row> m_rows;
    };

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
using namespace std::chrono_literals;

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
    m_searchListModel->setHeaderData(SearchSortModel::RESOLUTION, Qt::Horizontal, tr("Resolution"));
    m_searchListModel->setHeaderData(SearchSortModel::HDR_FORMAT, Qt::Horizontal, tr("HDR"));
    m_searchListModel->setHeaderData(SearchSortModel::SOURCE, Qt::Horizontal, tr("Source"));
    m_searchListModel->setHeaderData(SearchSortModel::AUDIO, Qt::Horizontal, tr("Audio"));
    m_searchListModel->setHeaderData(SearchSortModel::CODEC, Qt::Horizontal, tr("Codec"));
    m_searchListModel->setHeaderData(SearchSortModel::IN_PLEX, Qt::Horizontal, tr("Ownership"));
    m_searchListModel->setHeaderData(SearchSortModel::WARNING, Qt::Horizontal, tr("Warning"));
    for (int column = 0; column < SearchSortModel::NB_SEARCH_COLUMNS; ++column)
        m_columnTitles.insert(column, m_searchListModel->headerData(column, Qt::Horizontal, Qt::DisplayRole).toString());
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
    placeQualityColumns();

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
    connect(header(), &QHeaderView::sortIndicatorChanged, this, &SearchJobWidget::onSortIndicatorChanged);
    if (const QList<SearchSortModel::SortKey> savedKeys = SearchSortModel::sortKeysFromString(
            SettingsStorage::instance()->loadValue<QString>(SORT_KEYS_KEY)); savedKeys.size() > 1)
    {
        applySortKeys(savedKeys);
    }
    else if (header()->sortIndicatorSection() >= 0)
    {
        m_proxyModel->setSortKeys({{header()->sortIndicatorSection(), header()->sortIndicatorOrder()}});
    }

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
        emit filtersChanged();
    });
    connect(m_resultsFilterBuilder, &SearchFilterBuilder::searchAgainRequested, this, &SearchJobWidget::refreshRequested);

    // Sites that failed in this tab's searches, with fixes
    m_siteStatusBar = new QFrame(this);
    m_siteStatusBar->setFrameShape(QFrame::StyledPanel);
    auto *statusLayout = new QHBoxLayout(m_siteStatusBar);
    statusLayout->setContentsMargins(8, 4, 8, 4);
    auto *statusIcon = new QLabel(m_siteStatusBar);
    statusIcon->setPixmap(UIThemeManager::instance()->getIcon(u"dialog-warning"_s).pixmap(16, 16));
    statusLayout->addWidget(statusIcon);
    m_siteStatusLabel = new QLabel(m_siteStatusBar);
    m_siteStatusLabel->setWordWrap(true);
    statusLayout->addWidget(m_siteStatusLabel, 1);
    m_retrySitesButton = new QPushButton(tr("Retry these sites"), m_siteStatusBar);
    connect(m_retrySitesButton, &QPushButton::clicked, this, &SearchJobWidget::retryFailedSites);
    statusLayout->addWidget(m_retrySitesButton);
    m_cloudflareButton = new QPushButton(m_siteStatusBar);
    connect(m_cloudflareButton, &QPushButton::clicked, this, [this]()
    {
        if (!DioExtras::isHelperInstalled())
        {
            DioExtrasMenu::setUpHelper(this);
            return;
        }
        // a fresh pass for exactly the sites that were blocked, then search them again
        QList<std::pair<QString, QString>> blocked;
        for (const SearchEngineStatus &status : asConst(m_siteStatus))
        {
            if (status.state != u"blocked")
                continue;
            if (const SearchPluginInfo *info = SearchPluginManager::instance()->pluginInfo(status.plugin); info && !info->url.isEmpty())
                blocked.append({status.name, info->url});
        }
        if (!blocked.isEmpty() && DioExtrasMenu::refreshPassForSites(this, blocked))
            retryFailedSites();
    });
    statusLayout->addWidget(m_cloudflareButton);
    m_turnOffButton = new QPushButton(m_siteStatusBar);
    connect(m_turnOffButton, &QPushButton::clicked, this, [this]()
    {
        for (const QString &plugin : asConst(m_turnOffCandidates))
            SearchPluginManager::instance()->enablePlugin(plugin, false);
        m_turnOffButton->hide();
        m_siteStatusLabel->setText(tr("Turned off %1. You can turn sites back on under Search plugins.")
            .arg(m_turnOffButton->property("names").toString()));
    });
    statusLayout->addWidget(m_turnOffButton);
    m_siteStatusBar->hide();
    m_ui->verticalLayout->insertWidget(2, m_siteStatusBar);

    connect(PlexLibrary::instance(), &PlexLibrary::indexChanged, this, &SearchJobWidget::updateAllPlexColumns);



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
    // QWidget deletes its child objects only after this destructor has run, and a process that is
    // still running emits finished() while it is being deleted. Detach the running processes first,
    // so they can't call back into this tab after its members are gone (closing a tab during
    // "Look up details" crashed the app).
    stopDetailsLookup();
    for (SearchHandler *handler : asConst(m_searchHandlers))
    {
        handler->disconnect(this);
        handler->cancelSearch();
    }
    m_searchHandlers.clear();
    m_pendingQueries.clear();

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
    m_siteStatus.clear();
    m_retrying = false;
    m_siteStatusCounted = false;
    m_siteStatusBar->hide();

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
        connect(handler, &SearchHandler::engineStatusesReported, this, &SearchJobWidget::onEngineStatuses);
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
        if (!m_searchCancelled)
            updateSiteStatus(true);
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
        m_ui->resultsLbl->setText(tr("Results (showing <i>%1</i> out of <i>%2</i>, from %3 searches%4):")
                                  .arg(filteredResults).arg(totalResults).arg(m_queries.size())
                                  .arg(((filteredResults == 0) && (totalResults > 0)) ? tr("; none match the filters") : QString()));
    }
    else
    {
        m_ui->resultsLbl->setText(tr("Results (showing <i>%1</i> out of <i>%2</i>):", "i.e: Search results")
                                  .arg(filteredResults).arg(totalResults));
    }
    if (!m_detailsStatus.isEmpty())
        m_ui->resultsLbl->setText(m_ui->resultsLbl->text().chopped(1) + u" \u00B7 "_s + m_detailsStatus + u':');
    QStringList tip;
    if (!m_detailsFailures.isEmpty())
        tip.append(tr("Pages that couldn't be read:") + u'\n' + m_detailsFailures.join(u'\n'));
    if (m_queries.size() > 1)
    {
        tip.append(tr("Searches sent to the sites:"));
        for (const QString &query : asConst(m_queries))
            tip.append(u"  \u2022 "_s + query);
    }
    if (m_mergedCount > 0)
        tip.append(tr("%1 repeat listings combined (the same release found by more than one search or site)").arg(m_mergedCount));
    if ((filteredResults == 0) && (totalResults > 0))
        tip.append(tr("None of the results match this tab's filters. Synonyms count: \"2160p\" also matches 4K and UHD."));
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
    QAction *detailsAction = menu->addAction(UIThemeManager::instance()->getIcon(u"edit-find"_s), tr("Look up details")
        , this, &SearchJobWidget::lookUpDetails);
    detailsAction->setToolTip(tr("Reads the selected results' own pages for details the names leave out, such as Dolby Vision or Atmos"));
    detailsAction->setEnabled(!m_detailsProcess && m_ui->resultsBrowser->selectionModel()->hasSelection());
    menu->setToolTipsVisible(true);

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

void SearchJobWidget::displayColumnHeaderMenu(const QPoint &pos)
{
    auto *menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->setToolTipsVisible(true);

    QAction *sortAction = menu->addAction(tr("Sort by several columns..."), this, &SearchJobWidget::showSortDialog);
    sortAction->setToolTip(tr("Like a spreadsheet: sort by one column, then by another for rows that tie.\nTip: Shift-click a column header to add it as the next sort level."));
    if (m_proxyModel->sortKeys().size() > 1)
    {
        menu->addAction(tr("Sort by one column only"), this, [this]()
        {
            applySortKeys({m_proxyModel->sortKeys().first()});
        });
    }
    menu->addSeparator();

    // Spreadsheet-style filter for the column that was clicked: show only some of its values
    const int column = header()->logicalIndexAt(pos);
    const QList<int> valueColumns
    {
        SearchSortModel::RESOLUTION, SearchSortModel::HDR_FORMAT, SearchSortModel::SOURCE, SearchSortModel::AUDIO,
        SearchSortModel::CODEC, SearchSortModel::IN_PLEX, SearchSortModel::WARNING, SearchSortModel::ENGINE_NAME
    };
    if (valueColumns.contains(column))
    {
        // every value in the column, with how many rows have it
        // counted over the rows the other filters let through, like a spreadsheet does
        QHash<QString, int> counts;
        for (int row = 0; row < m_searchListModel->rowCount(); ++row)
        {
            if (!m_proxyModel->acceptsRowIgnoringColumn(row, column))
                continue;
            if (column == SearchSortModel::ENGINE_NAME)
            {
                const QStringList sites = m_searchListModel->index(row, column).data(SearchSortModel::SourcesRole).toStringList();
                for (const QString &site : (sites.isEmpty() ? QStringList {SearchSortModel::filterValue(m_searchListModel, row, column)} : sites))
                    ++counts[site];
            }
            else
            {
                ++counts[SearchSortModel::filterValue(m_searchListModel, row, column)];
            }
        }
        for (const QString &value : m_proxyModel->columnValueFilter(column))
            counts.insert(value, counts.value(value));
        QStringList values = counts.keys();
        std::sort(values.begin(), values.end(), [&counts](const QString &a, const QString &b)
        {
            if (a.isEmpty() != b.isEmpty())
                return b.isEmpty();  // blanks last
            return (counts.value(a) != counts.value(b)) ? (counts.value(a) > counts.value(b)) : (a < b);
        });

        const QSet<QString> current = m_proxyModel->columnValueFilter(column);
        menu->addSection(tr("Show only (%1)").arg(m_columnTitles.value(column)));
        QAction *allAction = menu->addAction(tr("All"), this, [this, column]()
        {
            m_proxyModel->setColumnValueFilter(column, {});
            updateColumnFilterTitles();
            updateResultsCount();
        });
        allAction->setCheckable(true);
        allAction->setChecked(current.isEmpty());
        for (const QString &value : asConst(values))
        {
            const QString label = (value.isEmpty() ? tr("(blank)") : value) + u"  (%1)"_s.arg(counts.value(value));
            QAction *action = menu->addAction(label, this, [this, column, value]()
            {
                QSet<QString> selected = m_proxyModel->columnValueFilter(column);
                if (selected.contains(value))
                    selected.remove(value);
                else
                    selected.insert(value);
                m_proxyModel->setColumnValueFilter(column, selected);
                updateColumnFilterTitles();
                updateResultsCount();
            });
            action->setCheckable(true);
            action->setChecked(current.contains(value));
        }
        menu->addSeparator();
    }
    if (m_proxyModel->hasColumnValueFilters())
    {
        menu->addAction(tr("Clear all column filters"), this, [this]()
        {
            m_proxyModel->clearColumnValueFilters();
            updateColumnFilterTitles();
            updateResultsCount();
        });
        menu->addSeparator();
    }

    QMenu *columnsMenu = valueColumns.contains(column) ? menu->addMenu(tr("Columns")) : menu;
    columnsMenu->setTitle(tr("Column visibility"));
    for (int i = 0; i < SearchSortModel::NB_SEARCH_COLUMNS; ++i)
    {
        if ((i == SearchSortModel::DL_LINK) || (i == SearchSortModel::DESC_LINK))
            continue;
        QAction *action = columnsMenu->addAction(m_columnTitles.value(i), this, [this, i](const bool checked)
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

    columnsMenu->addSeparator();
    QAction *resizeAction = columnsMenu->addAction(tr("Resize columns"), this, [this]()
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

void SearchJobWidget::showSortDialog()
{
    QList<std::pair<int, QString>> columns;
    for (int i = 0; i < SearchSortModel::NB_SEARCH_COLUMNS; ++i)
    {
        if ((i == SearchSortModel::DL_LINK) || (i == SearchSortModel::DESC_LINK))
            continue;
        columns.append({i, m_columnTitles.value(i)});
    }
    QList<SearchSortModel::SortKey> current = m_proxyModel->sortKeys();
    if (current.isEmpty() && (header()->sortIndicatorSection() >= 0))
    {
        const int column = header()->sortIndicatorSection();
        current.append({column, header()->sortIndicatorOrder(),
            SearchSortModel::supportsTolerance(column) ? SearchSortModel::DEFAULT_TOLERANCE : 0});
    }

    SortDialog dialog(current, columns, this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    applySortKeys(dialog.keys());
}

void SearchJobWidget::applySortKeys(const QList<SearchSortModel::SortKey> &keys)
{
    if (keys.isEmpty())
        return;
    m_applyingSort = true;
    m_proxyModel->setSortKeys(keys);
    header()->setSortIndicator(keys.first().column, keys.first().order);
    m_applyingSort = false;
    SettingsStorage::instance()->storeValue(SORT_KEYS_KEY, SearchSortModel::sortKeysToString(keys));
    updateColumnFilterTitles();
}

void SearchJobWidget::onSortIndicatorChanged(const int column, const Qt::SortOrder order)
{
    if (m_applyingSort || (column < 0))
        return;

    QList<SearchSortModel::SortKey> keys = m_proxyModel->sortKeys();
    if (QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier) && !keys.isEmpty())
    {
        // Shift-click: add the column as the next level, or flip it if it's already a level
        auto it = std::find_if(keys.begin(), keys.end(), [column](const SearchSortModel::SortKey &key) { return key.column == column; });
        if (it == keys.begin())
            it->order = order;
        else if (it != keys.end())
            it->order = (it->order == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder;
        else
            keys.append({column, isTextColumn(column) ? Qt::AscendingOrder : Qt::DescendingOrder,
                SearchSortModel::supportsTolerance(column) ? SearchSortModel::DEFAULT_TOLERANCE : 0});
    }
    else if (!keys.isEmpty() && (keys.first().column == column))
    {
        // clicking the main sort column flips it and keeps the tie-break levels
        keys.first().order = order;
    }
    else
    {
        // a plain click on another column sorts by that one column, like a spreadsheet
        keys = {{column, order, SearchSortModel::supportsTolerance(column) ? SearchSortModel::DEFAULT_TOLERANCE : 0}};
    }
    // a key added as the next level makes the one before it a tie-break level, where "close enough" applies
    applySortKeys(keys);
}

void SearchJobWidget::updateColumnFilterTitles()
{
    // a filtered column shows what it's limited to: "Source: REMUX", "Audio: 2 kinds"
    for (auto it = m_columnTitles.cbegin(); it != m_columnTitles.cend(); ++it)
    {
        const QSet<QString> selected = m_proxyModel->columnValueFilter(it.key());
        QString title = it.value();
        if (selected.size() == 1)
        {
            const QString value = *selected.cbegin();
            title += u": "_s + (value.isEmpty() ? tr("(blank)") : value);
        }
        else if (selected.size() > 1)
        {
            title += u": "_s + tr("%1 kinds").arg(selected.size());
        }
        // with a multi-column sort, every sort column shows its level and direction: "Seeders (2 \u2193)"
        if (const QList<SearchSortModel::SortKey> keys = m_proxyModel->sortKeys(); keys.size() > 1)
        {
            for (int level = 0; level < keys.size(); ++level)
            {
                if (keys[level].column == it.key())
                {
                    title += u" (%1 %2%3)"_s.arg(level + 1)
                        .arg((keys[level].order == Qt::AscendingOrder) ? u"\u2191"_s : u"\u2193"_s,
                            ((keys[level].tolerance > 0) && (level < (keys.size() - 1)))
                                ? u" \u00B1%1%"_s.arg(qRound(keys[level].tolerance * 100)) : QString());
                }
            }
        }
        if (m_searchListModel->headerData(it.key(), Qt::Horizontal, Qt::DisplayRole).toString() != title)
            m_searchListModel->setHeaderData(it.key(), Qt::Horizontal, title);
    }
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
    info.release = parseRelease(result.fileName);
    m_rowInfo.append(info);

    if (!infoHash.isEmpty())
        m_rowByHash.insert(infoHash, row);
    if (normName.size() >= MIN_MERGE_NAME_LENGTH)
        m_rowsByNameKey[normName.left(NAME_KEY_LENGTH)].append(row);

    updateQualityColumns(row);
    checkSameNameSizes(row);
    updateWarningColumn(row);
    updatePlexColumn(row);
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
    bool nameChanged = false;
    if ((normName.size() > info.normName.size()) && normName.startsWith(info.normName))
    {
        setData(SearchSortModel::NAME, result.fileName, result.fileName);
        info.normName = normName;
        info.release = parseRelease(result.fileName);
        nameChanged = true;
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
    if (nameChanged)
    {
        updateQualityColumns(row);
        updatePlexColumn(row);
    }
    updateWarningColumn(row);
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

void SearchJobWidget::setCell(const int row, const int column, const QString &display, const QVariant &underlying, const QString &tooltip)
{
    const QModelIndex index = m_searchListModel->index(row, column);
    m_searchListModel->setData(index, display, Qt::DisplayRole);
    m_searchListModel->setData(index, underlying, SearchSortModel::UnderlyingDataRole);
    m_searchListModel->setData(index, (tooltip.isEmpty() ? QVariant() : QVariant(tooltip)), Qt::ToolTipRole);
}

void SearchJobWidget::updateQualityColumns(const int row)
{
    const RowInfo &info = m_rowInfo.at(row);
    const ReleaseInfo &release = info.release;
    const auto tip = [&info](const int column) { return info.fromPage.contains(column) ? tr("Read from the torrent's page") : QString(); };
    setCell(row, SearchSortModel::RESOLUTION, resolutionText(release.resolution), release.resolution, tip(SearchSortModel::RESOLUTION));
    setCell(row, SearchSortModel::HDR_FORMAT, release.hdrText, release.hdr, tip(SearchSortModel::HDR_FORMAT));
    setCell(row, SearchSortModel::SOURCE, release.sourceText, release.source, tip(SearchSortModel::SOURCE));
    setCell(row, SearchSortModel::AUDIO, release.audioText, release.audio, tip(SearchSortModel::AUDIO));
    setCell(row, SearchSortModel::CODEC, release.codec, release.codec, tip(SearchSortModel::CODEC));
}

void SearchJobWidget::lookUpDetails()
{
    if (m_detailsProcess)
        return;

    m_detailsRows.clear();
    QStringList urls;
    for (const QModelIndex &proxyIndex : m_ui->resultsBrowser->selectionModel()->selectedRows())
    {
        const int sourceRow = m_proxyModel->mapToSource(proxyIndex).row();
        const QString url = m_searchListModel->index(sourceRow, SearchSortModel::DESC_LINK).data(SearchSortModel::UnderlyingDataRole).toString();
        if (!url.startsWith(u"http", Qt::CaseInsensitive))
            continue;
        if (!m_detailsRows.contains(url))
        {
            if (urls.size() >= 50)
                break;
            urls.append(url);
        }
        m_detailsRows[url].append(sourceRow);
    }
    if (urls.isEmpty())
    {
        m_detailsStatus = tr("no pages to read");
        updateResultsCount();
        return;
    }

    const QString python = DioExtras::isHelperInstalled()
        ? DioExtras::helperPython().data() : Utils::ForeignApps::pythonInfo().executablePath.data();
    const Path script = SearchPluginManager::engineLocation() / Path(u"dio_details.py"_s);

    m_detailsProcess = new QProcess(this);
    m_detailsProcess->setProgram(python);
    m_detailsProcess->setArguments(QStringList {Utils::ForeignApps::PYTHON_UTF8_MODE_FLAG, script.data()} + urls);
    m_detailsProcess->setWorkingDirectory(SearchPluginManager::engineLocation().data());
    connect(m_detailsProcess, &QProcess::readyReadStandardOutput, this, &SearchJobWidget::readDetailsOutput);
    connect(m_detailsProcess, &QProcess::finished, this, [this]()
    {
        readDetailsOutput();
        m_detailsStatus = tr("pages: %1 of %2 had more").arg(m_detailsImproved).arg(m_detailsDone);
        if (!m_detailsFailures.isEmpty())
            m_detailsStatus += tr(", %1 failed").arg(m_detailsFailures.size());
        updateResultsCount();
        m_detailsProcess->deleteLater();
        m_detailsProcess = nullptr;
    });
    connect(m_detailsProcess, &QProcess::errorOccurred, this, [this](const QProcess::ProcessError error)
    {
        if (error == QProcess::FailedToStart)
        {
            m_detailsStatus = tr("couldn't start Python to read the pages");
            updateResultsCount();
            m_detailsProcess->deleteLater();
            m_detailsProcess = nullptr;
        }
    });
    QTimer::singleShot(5min, m_detailsProcess, &QProcess::kill);

    m_detailsTotal = urls.size();
    m_detailsDone = 0;
    m_detailsImproved = 0;
    m_detailsFailures.clear();
    m_detailsBuffer.clear();
    m_detailsStatus = tr("reading pages 0/%1...").arg(m_detailsTotal);
    updateResultsCount();
    m_detailsProcess->start(QIODevice::ReadOnly);
}

void SearchJobWidget::readDetailsOutput()
{
    if (!m_detailsProcess)
        return;
    m_detailsBuffer += m_detailsProcess->readAllStandardOutput();
    qsizetype newline = m_detailsBuffer.indexOf('\n');
    while (newline >= 0)
    {
        const QByteArray line = m_detailsBuffer.left(newline).trimmed();
        m_detailsBuffer.remove(0, newline + 1);
        if (!line.isEmpty())
            applyDetails(QJsonDocument::fromJson(line).object());
        newline = m_detailsBuffer.indexOf('\n');
    }
}

void SearchJobWidget::applyDetails(const QJsonObject &details)
{
    const QString url = details.value(u"url").toString();
    ++m_detailsDone;
    if (!details.value(u"ok").toBool())
    {
        m_detailsFailures.append(u"%1: %2"_s.arg(QUrl(url).host(), details.value(u"error").toString()));
    }
    else
    {
        bool improved = false;
        for (const int row : m_detailsRows.value(url))
        {
            if (row >= m_rowInfo.size())
                continue;
            RowInfo &info = m_rowInfo[row];
            ReleaseInfo &release = info.release;
            // only ever add to what the name says
            if (details.value(u"hdr").toInt() > release.hdr)
            {
                release.hdr = details.value(u"hdr").toInt();
                release.hdrText = details.value(u"hdrText").toString();
                info.fromPage.insert(SearchSortModel::HDR_FORMAT);
                improved = true;
            }
            if (details.value(u"audio").toInt() > release.audio)
            {
                release.audio = details.value(u"audio").toInt();
                release.audioText = details.value(u"audioText").toString();
                info.fromPage.insert(SearchSortModel::AUDIO);
                improved = true;
            }
            if ((release.resolution == 0) && (details.value(u"resolution").toInt() > 0))
            {
                release.resolution = details.value(u"resolution").toInt();
                info.fromPage.insert(SearchSortModel::RESOLUTION);
                improved = true;
            }
            if ((release.source < 0) && details.contains(u"source"))
            {
                release.source = details.value(u"source").toInt();
                release.sourceText = details.value(u"sourceText").toString();
                info.fromPage.insert(SearchSortModel::SOURCE);
                improved = true;
            }
            if (release.codec.isEmpty() && details.contains(u"codec"))
            {
                release.codec = details.value(u"codec").toString();
                info.fromPage.insert(SearchSortModel::CODEC);
                improved = true;
            }
            updateQualityColumns(row);
            updateWarningColumn(row);
            updatePlexColumn(row);
        }
        if (improved)
            ++m_detailsImproved;
    }
    if (m_detailsProcess)
    {
        m_detailsStatus = tr("reading pages %1/%2...").arg(m_detailsDone).arg(m_detailsTotal);
        updateResultsCount();
    }
}

void SearchJobWidget::stopDetailsLookup()
{
    if (!m_detailsProcess)
        return;
    m_detailsProcess->disconnect(this);
    m_detailsProcess->kill();
    m_detailsProcess->deleteLater();
    m_detailsProcess = nullptr;
    m_detailsRows.clear();
    m_detailsStatus.clear();
}

void SearchJobWidget::checkSameNameSizes(const int row)
{
    // Fakes copy the names of popular releases: flag listings that share a name but not a size
    RowInfo &info = m_rowInfo[row];
    if ((info.size <= 0) || (info.normName.size() < MIN_MERGE_NAME_LENGTH))
        return;
    for (const int other : m_rowsByNameKey.value(info.normName.left(NAME_KEY_LENGTH)))
    {
        if (other == row)
            continue;
        RowInfo &otherInfo = m_rowInfo[other];
        if ((otherInfo.normName != info.normName) || (otherInfo.size <= 0) || sizesClose(otherInfo.size, info.size, 0.08))
            continue;
        const QString warning = tr("Another listing with this exact name is a very different size (%1 vs %2)")
            .arg(Utils::Misc::friendlyUnit(info.size), Utils::Misc::friendlyUnit(otherInfo.size));
        const QString otherWarning = tr("Another listing with this exact name is a very different size (%1 vs %2)")
            .arg(Utils::Misc::friendlyUnit(otherInfo.size), Utils::Misc::friendlyUnit(info.size));
        if (!info.extraWarnings.contains(warning))
            info.extraWarnings.append(warning);
        if (!otherInfo.extraWarnings.contains(otherWarning))
        {
            otherInfo.extraWarnings.append(otherWarning);
            updateWarningColumn(other);
        }
    }
}

void SearchJobWidget::updateWarningColumn(const int row)
{
    const RowInfo &info = m_rowInfo.at(row);
    const QStringList warnings = info.release.warnings(info.size) + info.extraWarnings;
    QString display;
    if (!warnings.isEmpty())
        display = (warnings.size() > 1) ? tr("%1 (+%2 more)").arg(warnings.first()).arg(warnings.size() - 1) : warnings.first();
    setCell(row, SearchSortModel::WARNING, display, static_cast<int>(warnings.size()), warnings.join(u'\n'));
    m_searchListModel->setData(m_searchListModel->index(row, SearchSortModel::NAME)
        , (warnings.isEmpty() ? QVariant() : QVariant(UIThemeManager::instance()->getIcon(u"dialog-warning"_s))), Qt::DecorationRole);
}

void SearchJobWidget::updatePlexColumn(const int row)
{
    const RowInfo &info = m_rowInfo.at(row);
    const QString name = m_searchListModel->index(row, SearchSortModel::NAME).data(SearchSortModel::UnderlyingDataRole).toString();
    const PlexLibrary::Match match = PlexLibrary::instance()->lookup(name, info.release);
    setCell(row, SearchSortModel::IN_PLEX, match.text, match.rank, match.tooltip);
    const QModelIndex index = m_searchListModel->index(row, SearchSortModel::IN_PLEX);
    // already in qBittorrent: a down arrow instead of the word "Downloading"
    m_searchListModel->setData(index, (match.downloading ? QVariant(UIThemeManager::instance()->getIcon(u"downloading"_s)) : QVariant()), Qt::DecorationRole);
    m_searchListModel->setData(index, (match.filterValue.isEmpty() ? QVariant() : QVariant(match.filterValue)), SearchSortModel::FilterValueRole);
}

void SearchJobWidget::updateAllPlexColumns()
{
    for (int row = 0; row < m_rowInfo.size(); ++row)
        updatePlexColumn(row);
}

void SearchJobWidget::placeQualityColumns()
{
    // The first time these columns exist: put them after Leechers, with sensible widths
    const QString placedKey = u"Search/Dio/QualityColumnsPlaced"_s;
    auto *storage = SettingsStorage::instance();
    if (storage->loadValue<bool>(placedKey))
        return;

    QHeaderView *headerView = header();
    int target = headerView->visualIndex(SearchSortModel::LEECHES) + 1;
    const QList<std::pair<int, int>> columns
    {
        {SearchSortModel::RESOLUTION, 80}, {SearchSortModel::HDR_FORMAT, 90}, {SearchSortModel::SOURCE, 80},
        {SearchSortModel::AUDIO, 110}, {SearchSortModel::IN_PLEX, 70}, {SearchSortModel::WARNING, 220}
    };
    for (const auto &[column, width] : columns)
    {
        m_ui->resultsBrowser->setColumnHidden(column, false);
        headerView->moveSection(headerView->visualIndex(column), target++);
        m_ui->resultsBrowser->setColumnWidth(column, width);
    }
    m_ui->resultsBrowser->setColumnHidden(SearchSortModel::CODEC, true);
    m_ui->resultsBrowser->setColumnWidth(SearchSortModel::CODEC, 70);

    storage->storeValue(placedKey, true);
    saveSettings();
}

void SearchJobWidget::onEngineStatuses(const QList<SearchEngineStatus> &statuses)
{
    const auto rank = [](const QString &state) { return (state == u"ok") ? 3 : ((state == u"empty") ? 2 : 1); };
    for (const SearchEngineStatus &status : statuses)
    {
        const auto it = m_siteStatus.find(status.plugin);
        if (it == m_siteStatus.end())
            m_siteStatus.insert(status.plugin, status);
        else if (rank(status.state) > rank(it->state))
            *it = status;
    }
}

void SearchJobWidget::updateSiteStatus(const bool searchFinished)
{
    QStringList failedNames;
    QStringList details;
    QStringList newlyFailed;
    bool anyBlocked = false;

    // how many searches in a row each site has failed, to suggest turning off dead ones
    auto *storage = SettingsStorage::instance();
    const QString streakKey = u"Search/Dio/SiteFailStreak"_s;
    QVariantMap streaks = storage->loadValue<QVariantMap>(streakKey);
    m_turnOffCandidates.clear();
    QStringList turnOffNames;

    for (const SearchEngineStatus &status : asConst(m_siteStatus))
    {
        const bool failed = (status.state != u"ok") && (status.state != u"empty");
        if (!m_siteStatusCounted)
            streaks[status.plugin] = failed ? (streaks.value(status.plugin).toInt() + 1) : 0;
        if (!failed)
            continue;

        QString reason;
        if (status.state == u"blocked")
        {
            reason = tr("blocked by Cloudflare");
            anyBlocked = true;
        }
        else if (status.state == u"down")
        {
            reason = tr("site down or not answering");
        }
        else
        {
            reason = status.message.isEmpty() ? tr("error") : tr("error: %1").arg(status.message.left(90));
        }
        failedNames.append(status.name);
        details.append(u"%1 (%2)"_s.arg(status.name, reason));

        const int streak = streaks.value(status.plugin).toInt();
        if (streak <= 1)
            newlyFailed.append(status.name);
        if (streak >= 5)
        {
            m_turnOffCandidates.append(status.plugin);
            turnOffNames.append(status.name);
        }
    }
    if (!m_siteStatusCounted)
    {
        storage->storeValue(streakKey, streaks);
        m_siteStatusCounted = true;
    }

    if (failedNames.isEmpty())
    {
        m_siteStatusBar->hide();
        return;
    }

    QString text = tr("%1 of the sites didn't return results: %2.").arg(failedNames.size()).arg(details.join(u", "_s));
    if (!turnOffNames.isEmpty())
        text += u' ' + tr("%1 has failed 5 or more searches in a row.").arg(turnOffNames.join(u", "_s));
    m_siteStatusLabel->setText(text);
    m_retrySitesButton->setVisible(!m_retrying);
    m_cloudflareButton->setVisible(anyBlocked);
    m_cloudflareButton->setText(DioExtras::isHelperInstalled() ? tr("Get past Cloudflare and retry") : tr("Set up Cloudflare helper..."));
    m_turnOffButton->setVisible(!turnOffNames.isEmpty());
    m_turnOffButton->setText(tr("Turn off %1").arg(turnOffNames.join(u", "_s)));
    m_turnOffButton->setProperty("names", turnOffNames.join(u", "_s));
    m_siteStatusBar->show();

    // A notification only when a site that was working stops; ongoing failures stay in the tab
    if (searchFinished && !m_retrying && !newlyFailed.isEmpty())
    {
        app()->desktopIntegration()->showNotification(tr("Search: %1").arg(m_searchPattern)
            , tr("%1 didn't return results. The search tab has the details and a retry button.").arg(newlyFailed.join(u", "_s)));
    }
}

void SearchJobWidget::retryFailedSites()
{
    QStringList failed;
    for (const SearchEngineStatus &status : asConst(m_siteStatus))
    {
        if ((status.state != u"ok") && (status.state != u"empty"))
            failed.append(status.plugin);
    }
    if (failed.isEmpty() || m_queries.isEmpty() || !m_searchHandlers.isEmpty())
        return;

    // the same searches, only on the sites that failed; results join the ones already here
    m_plugins = failed;
    m_pendingQueries = m_queries;
    m_failedSearches = 0;
    m_searchCancelled = false;
    m_retrying = true;
    m_siteStatusBar->hide();
    setStatus(Status::Ongoing);
    startPendingSearches();
}

void SearchJobWidget::setSearchFilters(const QStringList &rows, const bool matchAll)
{
    m_resultsFilterBuilder->setRows(rows, matchAll);
}

QStringList SearchJobWidget::searchFilterRows() const
{
    return m_resultsFilterBuilder->serializedRows();
}

bool SearchJobWidget::searchFiltersMatchAll() const
{
    return m_resultsFilterBuilder->matchAll();
}

QStringList SearchJobWidget::filterWords() const
{
    return m_resultsFilterBuilder->requiredWords();
}

void SearchJobWidget::clearResults()
{
    stopDetailsLookup();
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
