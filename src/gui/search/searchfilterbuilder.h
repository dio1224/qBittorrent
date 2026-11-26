/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026  Diossley Rodriguez
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

#include <QList>
#include <QWidget>

#include "searchsortmodel.h"

class QComboBox;
class QDateEdit;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QVBoxLayout;

// Lets the user stack filter rules (any column, standard conditions such as
// contains / is / starts with / greater than / before) and choose whether all
// of them or any of them must match. Rules are remembered between searches.
class SearchFilterBuilder final : public QWidget
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SearchFilterBuilder)

public:
    explicit SearchFilterBuilder(QWidget *parent = nullptr);

    QList<SearchSortModel::FilterRule> rules() const;
    bool matchAll() const;

signals:
    void rulesChanged();

private:
    struct Row
    {
        QWidget *widget = nullptr;
        QComboBox *field = nullptr;
        QComboBox *op = nullptr;
        QStackedWidget *editors = nullptr;
        QLineEdit *text = nullptr;
        QDoubleSpinBox *number = nullptr;
        QComboBox *sizeUnit = nullptr;
        QDateEdit *date = nullptr;
        QSpinBox *days = nullptr;
    };

    void addRow(const QStringList &saved = {});
    void removeRow(QWidget *rowWidget);
    void clearRows();
    void fillOps(Row &row) const;
    void showEditor(Row &row) const;
    SearchSortModel::FilterRule ruleOf(const Row &row) const;
    QString serialize(const Row &row) const;
    void onChanged();
    void load();
    void save() const;
    void updateHeaderVisibility();
    Row *findRow(QWidget *rowWidget);

    QVBoxLayout *m_rowsLayout = nullptr;
    QComboBox *m_matchMode = nullptr;
    QWidget *m_matchBox = nullptr;
    QPushButton *m_clearButton = nullptr;
    QList<Row> m_rows;
    bool m_loading = false;
};
