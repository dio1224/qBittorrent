/*
 * Bittorrent Client using Qt and libtorrent.
 * Dio's build: search synonyms and search options
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "searchsynonyms.h"

#include <algorithm>

#include <QRegularExpression>

#include "base/global.h"
#include "base/settingsstorage.h"

namespace
{
    const QString KEY_TEXT = u"Search/Dio/Synonyms"_s;
    const QString KEY_MAX_SEARCHES = u"Search/Dio/MaxSearches"_s;
    const QString KEY_MERGE = u"Search/Dio/MergeDuplicates"_s;

    const int DEFAULT_MAX_SEARCHES = 6;

    QList<SearchSynonyms::Group> g_groups;
    bool g_loaded = false;

    // Separators release names use between words
    const QRegularExpression &separators()
    {
        static const QRegularExpression re {u"[\\s._\\-]+"_s};
        return re;
    }

    // "Dolby.Vision" / "dolby vision" / "DolbyVision" all compare equal
    QString tight(const QString &term)
    {
        QString out = term.toLower();
        out.remove(separators());
        return out;
    }
}

QString SearchSynonyms::defaultText()
{
    return u"# One group per line: terms that mean the same thing, separated by commas.\n"
        "# Searches also run with the other terms in a group, and filters match any of them.\n"
        "# Terms after \"|\" are matched in names but never searched for on their own.\n"
        "2160p, 4K, UHD | 2160, Ultra HD\n"
        "1080p | 1080, Full HD, FHD\n"
        "720p | 720\n"
        "DV, Dolby Vision, DoVi\n"
        "HDR10, HDR | HDR10+, HDR10 Plus\n"
        "HEVC, x265 | H.265\n"
        "x264, AVC | H.264\n"
        "REMUX | BD Remux\n"
        "BluRay | Blu-ray\n"
        "Atmos | Dolby Atmos\n"
        "DDP, EAC3 | DD+, E-AC-3, Dolby Digital Plus\n"_s;
}

QString SearchSynonyms::text()
{
    return SettingsStorage::instance()->loadValue<QString>(KEY_TEXT, defaultText());
}

void SearchSynonyms::setText(const QString &text)
{
    SettingsStorage::instance()->storeValue(KEY_TEXT, text);
    g_loaded = false;
}

QList<SearchSynonyms::Group> SearchSynonyms::parse(const QString &text)
{
    QList<Group> groups;
    const QStringList lines = text.split(u'\n');
    for (QString line : lines)
    {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;

        const qsizetype bar = line.indexOf(u'|');
        const QString searched = (bar >= 0) ? line.left(bar) : line;
        const QString matchedOnly = (bar >= 0) ? line.mid(bar + 1) : QString();

        Group group;
        for (const QString &term : searched.split(u',', Qt::SkipEmptyParts))
        {
            if (const QString t = term.trimmed(); !t.isEmpty())
                group.variants.append(t);
        }
        for (const QString &term : matchedOnly.split(u',', Qt::SkipEmptyParts))
        {
            if (const QString t = term.trimmed(); !t.isEmpty())
                group.triggers.append(t);
        }
        if ((group.variants.size() + group.triggers.size()) >= 2)
            groups.append(group);
    }
    return groups;
}

const QList<SearchSynonyms::Group> &SearchSynonyms::groups()
{
    if (!g_loaded)
    {
        g_groups = parse(text());
        g_loaded = true;
    }
    return g_groups;
}

int SearchSynonyms::maxSearches()
{
    return std::clamp(SettingsStorage::instance()->loadValue<int>(KEY_MAX_SEARCHES, DEFAULT_MAX_SEARCHES), 1, 10);
}

void SearchSynonyms::setMaxSearches(const int value)
{
    SettingsStorage::instance()->storeValue(KEY_MAX_SEARCHES, std::clamp(value, 1, 10));
}

bool SearchSynonyms::mergeDuplicates()
{
    return SettingsStorage::instance()->loadValue<bool>(KEY_MERGE, true);
}

void SearchSynonyms::setMergeDuplicates(const bool value)
{
    SettingsStorage::instance()->storeValue(KEY_MERGE, value);
}

QString SearchSynonyms::normalize(const QString &term)
{
    QString out = term.toLower();
    out.replace(separators(), u" "_s);
    return out.trimmed();
}

int SearchSynonyms::groupOf(const QString &term)
{
    const QString key = tight(term);
    if (key.isEmpty())
        return -1;

    const QList<Group> &all = groups();
    for (int i = 0; i < all.size(); ++i)
    {
        for (const QString &t : all[i].variants)
        {
            if (tight(t) == key)
                return i;
        }
        for (const QString &t : all[i].triggers)
        {
            if (tight(t) == key)
                return i;
        }
    }
    return -1;
}

QStringList SearchSynonyms::splitTerms(const QString &pattern)
{
    const QStringList tokens = pattern.split(u' ', Qt::SkipEmptyParts);
    QStringList terms;
    for (int i = 0; i < tokens.size();)
    {
        // Longest run of up to 3 words that is a known term ("dolby digital plus")
        int taken = 1;
        for (int len = std::min<int>(3, (tokens.size() - i)); len >= 2; --len)
        {
            if (groupOf(tokens.mid(i, len).join(u' ')) >= 0)
            {
                taken = len;
                break;
            }
        }
        terms.append(tokens.mid(i, taken).join(u' '));
        i += taken;
    }
    return terms;
}

QString SearchSynonyms::namePattern(const QString &term)
{
    const int group = groupOf(term);
    QStringList terms;
    if (group >= 0)
        terms = groups()[group].variants + groups()[group].triggers;
    else
        terms = {term};

    QStringList alternatives;
    for (const QString &t : terms)
    {
        const QStringList parts = t.split(separators(), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;

        QStringList escaped;
        for (const QString &part : parts)
            escaped.append(QRegularExpression::escape(part));
        QString body = escaped.join(u"[\\s._\\-]*"_s);

        // Short synonyms ("DV", "4K", "UHD") must stand on their own, so "DV" doesn't
        // match "DVD". A digit may follow ("HDR10", "DDP5.1"), a letter may not.
        if ((group >= 0) && (parts.join(QString()).size() <= 3))
            body = u"(?<![\\p{L}\\p{N}])"_s + body + u"(?![\\p{L}])"_s;

        alternatives.append(body);
    }
    if (alternatives.isEmpty())
        return QRegularExpression::escape(term);
    return u"(?:"_s + alternatives.join(u'|') + u')';
}
