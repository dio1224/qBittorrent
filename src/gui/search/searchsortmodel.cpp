/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2025  Vladimir Golovnev <glassez@yandex.ru>
 * Copyright (C) 2013  sledgehammer999 <hammered999@gmail.com>
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

#include "searchsortmodel.h"

#include <cmath>

#include "base/global.h"
#include "searchsynonyms.h"

SearchSortModel::SearchSortModel(QObject *parent)
    : base(parent)
{
    setSortRole(UnderlyingDataRole);
    setFilterRole(UnderlyingDataRole);
}

void SearchSortModel::enableNameFilter(const bool enabled)
{
    if (m_isNameFilterEnabled == enabled)
        return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_isNameFilterEnabled = enabled;
    endFilterChange(Direction::Rows);
#else
    m_isNameFilterEnabled = enabled;
    invalidateRowsFilter();
#endif
}

void SearchSortModel::setNameFilter(const QString &searchTerm)
{
    if (m_searchTerm == searchTerm)
        return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();

    m_searchTerm = searchTerm;
    if ((searchTerm.length() > 2) && searchTerm.startsWith(u'"') && searchTerm.endsWith(u'"'))
        m_searchTermWords = QStringList(m_searchTerm.sliced(1, (m_searchTerm.length() - 2)));
    else
        m_searchTermWords = SearchSynonyms::splitTerms(searchTerm);
    compileSearchTermWords();

    endFilterChange(Direction::Rows);
#else
    m_searchTerm = searchTerm;
    if ((searchTerm.length() > 2) && searchTerm.startsWith(u'"') && searchTerm.endsWith(u'"'))
        m_searchTermWords = QStringList(m_searchTerm.sliced(1, (m_searchTerm.length() - 2)));
    else
        m_searchTermWords = SearchSynonyms::splitTerms(searchTerm);
    compileSearchTermWords();

    invalidateRowsFilter();
#endif
}

void SearchSortModel::setSizeFilter(qint64 minSize, qint64 maxSize)
{
    minSize = std::max<qint64>(0, minSize);
    maxSize = std::max<qint64>(-1, maxSize);

    if ((m_minSize == minSize) && (m_maxSize == maxSize))
        return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_minSize = minSize;
    m_maxSize = maxSize;
    endFilterChange(Direction::Rows);
#else
    m_minSize = minSize;
    m_maxSize = maxSize;
    invalidateRowsFilter();
#endif
}

void SearchSortModel::setSeedsFilter(int minSeeds, int maxSeeds)
{
    minSeeds = std::max(0, minSeeds);
    maxSeeds = std::max(-1, maxSeeds);

    if ((m_minSeeds == minSeeds) && (m_maxSeeds == maxSeeds))
        return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_minSeeds = minSeeds;
    m_maxSeeds = maxSeeds;
    endFilterChange(Direction::Rows);
#else
    m_minSeeds = minSeeds;
    m_maxSeeds = maxSeeds;
    invalidateRowsFilter();
#endif
}

void SearchSortModel::setLeechesFilter(int minLeeches, int maxLeeches)
{
    minLeeches = std::max(0, minLeeches);
    maxLeeches = std::max(-1, maxLeeches);

    if ((m_minLeeches == minLeeches) && (m_maxLeeches == maxLeeches))
        return;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    beginFilterChange();
    m_minLeeches = minLeeches;
    m_maxLeeches = maxLeeches;
    endFilterChange(Direction::Rows);
#else
    m_minLeeches = minLeeches;
    m_maxLeeches = maxLeeches;
    invalidateRowsFilter();
#endif
}

bool SearchSortModel::isNameFilterEnabled() const
{
    return m_isNameFilterEnabled;
}

QString SearchSortModel::searchTerm() const
{
    return m_searchTerm;
}

int SearchSortModel::minSeeds() const
{
    return m_minSeeds;
}

int SearchSortModel::maxSeeds() const
{
    return m_maxSeeds;
}

qint64 SearchSortModel::minSize() const
{
    return m_minSize;
}

qint64 SearchSortModel::maxSize() const
{
    return m_maxSize;
}

bool SearchSortModel::lessThan(const QModelIndex &left, const QModelIndex &right) const
{
    switch (sortColumn())
    {
    case NAME:
    case ENGINE_URL:
        {
            const QString strL = left.data().toString();
            const QString strR = right.data().toString();
            return m_naturalLessThan(strL, strR);
        }
        break;
    default:
        return base::lessThan(left, right);
    };
}

void SearchSortModel::compileSearchTermWords()
{
    m_searchTermRegexes.clear();
    for (const QString &word : asConst(m_searchTermWords))
    {
        m_searchTermRegexes.append(QRegularExpression(SearchSynonyms::namePattern(word)
            , QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption));
    }
}

void SearchSortModel::refreshSynonyms()
{
    compileSearchTermWords();
    m_filterRules = compileRules(m_rawFilterRules);
    m_localRules = compileRules(m_rawLocalRules);
    invalidateRowsFilter();
}

void SearchSortModel::setFilterRules(const QList<FilterRule> &rules, const bool matchAll)
{
    m_rawFilterRules = rules;
    m_filterRules = compileRules(rules);
    m_filterRulesMatchAll = matchAll;
    invalidateRowsFilter();
}

void SearchSortModel::setLocalFilterRules(const QList<FilterRule> &rules, const bool matchAll)
{
    m_rawLocalRules = rules;
    m_localRules = compileRules(rules);
    m_localRulesMatchAll = matchAll;
    invalidateRowsFilter();
}

QList<SearchSortModel::CompiledRule> SearchSortModel::compileRules(const QList<FilterRule> &rules) const
{
    using Op = FilterRule::Op;
    using Field = FilterRule::Field;

    QList<CompiledRule> compiled;
    for (const FilterRule &rule : rules)
    {
        CompiledRule c;
        c.rule = rule;
        const QString text = rule.text.trimmed();
        if (text.isEmpty())
            continue;

        switch (rule.field)
        {
        case Field::Name:
        case Field::Engine:
            {
                // Names: spaces/dots/dashes are interchangeable and synonyms count
                // ("2160p" also matches "4K" and "UHD"). Engines: plain text.
                const QString esc = (rule.field == Field::Name)
                    ? SearchSynonyms::namePattern(text) : QRegularExpression::escape(text);
                const QString notWordChar = u"[\\p{L}\\p{N}]"_s;
                QString pattern;
                switch (rule.op)
                {
                case Op::ContainsWord:
                case Op::DoesNotContainWord:
                    pattern = u"(?<!"_s + notWordChar + u")"_s + esc + u"(?!"_s + notWordChar + u")"_s;
                    break;
                case Op::Is:
                case Op::IsNot:
                    pattern = u"^"_s + esc + u"$"_s;
                    break;
                case Op::StartsWith:
                case Op::DoesNotStartWith:
                    pattern = u"^"_s + esc;
                    break;
                case Op::EndsWith:
                case Op::DoesNotEndWith:
                    pattern = esc + u"$"_s;
                    break;
                case Op::MatchesPattern:
                case Op::DoesNotMatchPattern:
                    pattern = text;
                    break;
                default:
                    pattern = esc;
                    break;
                }
                c.regex = QRegularExpression(pattern, QRegularExpression::CaseInsensitiveOption);
                if (!c.regex.isValid())
                    continue;   // half-typed pattern: ignore until it's valid
            }
            break;
        case Field::Size:
        case Field::Seeds:
        case Field::Leeches:
            {
                bool ok = false;
                c.number = text.toDouble(&ok);
                if (!ok)
                    continue;
            }
            break;
        case Field::PubDate:
            if ((rule.op == Op::WithinLastDays) || (rule.op == Op::OlderThanDays))
            {
                bool ok = false;
                c.days = text.toLongLong(&ok);
                if (!ok)
                    continue;
            }
            else
            {
                c.dateTime = QDateTime(QDate::fromString(text, Qt::ISODate), QTime(0, 0));
                if (!c.dateTime.isValid())
                    continue;
            }
            break;
        }
        compiled.append(c);
    }
    return compiled;
}

bool SearchSortModel::rulesPass(const QList<CompiledRule> &rules, const bool matchAll, const int sourceRow, const QModelIndex &sourceParent) const
{
    if (rules.isEmpty())
        return true;
    for (const CompiledRule &rule : rules)
    {
        const bool ok = ruleMatches(sourceRow, sourceParent, rule);
        if (matchAll && !ok)
            return false;
        if (!matchAll && ok)
            return true;
    }
    return matchAll;
}

bool SearchSortModel::ruleMatches(const int sourceRow, const QModelIndex &sourceParent, const CompiledRule &c) const
{
    using Op = FilterRule::Op;
    using Field = FilterRule::Field;

    const QAbstractItemModel *const model = sourceModel();
    const auto value = [&](const int column) { return model->data(model->index(sourceRow, column, sourceParent), UnderlyingDataRole); };

    switch (c.rule.field)
    {
    case Field::Name:
    case Field::Engine:
        {
            QString subject;
            if (c.rule.field == Field::Name)
            {
                subject = value(NAME).toString();
            }
            else
            {
                // merged rows carry every site that listed them
                const QVariant sources = model->data(model->index(sourceRow, ENGINE_NAME, sourceParent), SourcesRole);
                subject = sources.isValid() ? sources.toStringList().join(u'\n') : value(ENGINE_NAME).toString();
            }
            bool found = false;
            if (c.rule.field == Field::Engine)
            {
                for (const QString &line : subject.split(u'\n'))
                    found = found || c.regex.match(line).hasMatch();
            }
            else
            {
                found = c.regex.match(subject).hasMatch();
            }
            switch (c.rule.op)
            {
            case Op::DoesNotContain:
            case Op::DoesNotContainWord:
            case Op::IsNot:
            case Op::DoesNotStartWith:
            case Op::DoesNotEndWith:
            case Op::DoesNotMatchPattern:
                return !found;
            default:
                return found;
            }
        }
    case Field::Size:
    case Field::Seeds:
    case Field::Leeches:
        {
            const int column = (c.rule.field == Field::Size) ? SIZE : ((c.rule.field == Field::Seeds) ? SEEDS : LEECHES);
            const double v = value(column).toDouble();
            // sizes compare within half a percent so "equal to 56 GB" matches 56.03 GB
            const double tolerance = (c.rule.field == Field::Size) ? (c.number * 0.005) : 0.0;
            switch (c.rule.op)
            {
            case Op::Equal: return std::abs(v - c.number) <= tolerance;
            case Op::NotEqual: return std::abs(v - c.number) > tolerance;
            case Op::Greater: return v > c.number;
            case Op::GreaterOrEqual: return v >= c.number;
            case Op::Less: return v < c.number;
            case Op::LessOrEqual: return v <= c.number;
            default: return true;
            }
        }
    case Field::PubDate:
        {
            const QDateTime date = value(PUB_DATE).toDateTime();
            if (!date.isValid())
                return false;
            switch (c.rule.op)
            {
            case Op::After: return date >= c.dateTime.addDays(1);
            case Op::Before: return date < c.dateTime;
            case Op::WithinLastDays: return date >= QDateTime::currentDateTime().addDays(-c.days);
            case Op::OlderThanDays: return date < QDateTime::currentDateTime().addDays(-c.days);
            default: return true;
            }
        }
    }
    return true;
}

bool SearchSortModel::filterAcceptsRow(const int sourceRow, const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *const sourceModel = this->sourceModel();

    if (!rulesPass(m_filterRules, m_filterRulesMatchAll, sourceRow, sourceParent)
        || !rulesPass(m_localRules, m_localRulesMatchAll, sourceRow, sourceParent))
        return false;

    if (m_isNameFilterEnabled && !m_searchTerm.isEmpty())
    {
        const QString name = sourceModel->data(sourceModel->index(sourceRow, NAME, sourceParent), UnderlyingDataRole).toString();
        for (const QRegularExpression &word : asConst(m_searchTermRegexes))
        {
            if (!word.match(name).hasMatch())
                return false;
        }
    }

    if ((m_minSize > 0) || (m_maxSize >= 0))
    {
        const qlonglong size = sourceModel->data(sourceModel->index(sourceRow, SIZE, sourceParent), UnderlyingDataRole).toLongLong();
        if (((m_minSize > 0) && (size < m_minSize))
            || ((m_maxSize > 0) && (size > m_maxSize)))
            return false;
    }

    if ((m_minSeeds > 0) || (m_maxSeeds >= 0))
    {
        const int seeds = sourceModel->data(sourceModel->index(sourceRow, SEEDS, sourceParent), UnderlyingDataRole).toInt();
        if (((m_minSeeds > 0) && (seeds < m_minSeeds))
            || ((m_maxSeeds > 0) && (seeds > m_maxSeeds)))
            return false;
    }

    if ((m_minLeeches > 0) || (m_maxLeeches >= 0))
    {
        const int leeches = sourceModel->data(sourceModel->index(sourceRow, LEECHES, sourceParent), UnderlyingDataRole).toInt();
        if (((m_minLeeches > 0) && (leeches < m_minLeeches))
            || ((m_maxLeeches > 0) && (leeches > m_maxLeeches)))
            return false;
    }

    return base::filterAcceptsRow(sourceRow, sourceParent);
}
