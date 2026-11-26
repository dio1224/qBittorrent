/*
 * Bittorrent Client using Qt and libtorrent.
 * Dio's build: search synonyms and search options
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// Groups of release-name terms that mean the same thing (2160p / 4K / UHD,
// DV / Dolby Vision / DoVi...). They're used to run extra site searches and
// to make filters and name matching accept any term of a group.
namespace SearchSynonyms
{
    struct Group
    {
        QStringList variants;   // searched for, and matched in names
        QStringList triggers;   // matched in names and start the group, never searched for
    };

    QString defaultText();
    QString text();
    void setText(const QString &text);
    QList<Group> parse(const QString &text);
    const QList<Group> &groups();

    // Most site searches one search may run (the typed search, the search
    // with the filter's words, and synonym variants)
    int maxSearches();
    void setMaxSearches(int value);

    // Show a release listed by several sites as one row
    bool mergeDuplicates();
    void setMergeDuplicates(bool value);

    // Lowercase with separators (space . _ -) folded to one space
    QString normalize(const QString &term);
    // Index into groups() of the group holding this term, or -1
    int groupOf(const QString &term);
    // Splits a search into terms, keeping known multi-word terms ("dolby vision") together
    QStringList splitTerms(const QString &pattern);
    // Regex (unanchored) matching the term, or any term of its group, inside a release name.
    // Spaces, dots, dashes and underscores are interchangeable.
    QString namePattern(const QString &term);
}
