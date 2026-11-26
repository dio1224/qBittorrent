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

#include "searchfilterbuilder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDate>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMenu>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "base/global.h"
#include "base/settingsstorage.h"
#include "gui/uithememanager.h"

using Field = SearchSortModel::FilterRule::Field;
using Op = SearchSortModel::FilterRule::Op;

namespace
{
    const QString KEY_RULES = u"Search/FilterBuilder/Rules2"_s;
    const QString KEY_MATCH_ALL = u"Search/FilterBuilder/MatchAll"_s;
    const QString KEY_SEND_WORDS = u"Search/FilterBuilder/SendWordsToSites"_s;

    enum Editor
    {
        TextEditor,
        NumberEditor,
        DateEditor,
        DaysEditor
    };

    enum class Kind
    {
        Text,
        Number,
        Date
    };

    Kind kindOf(const Field field)
    {
        switch (field)
        {
        case Field::Size:
        case Field::Seeds:
        case Field::Leeches:
            return Kind::Number;
        case Field::PubDate:
            return Kind::Date;
        default:
            return Kind::Text;
        }
    }

    QList<std::pair<Op, QString>> opsFor(const Kind kind)
    {
        switch (kind)
        {
        case Kind::Number:
            return {
                {Op::Equal, SearchFilterBuilder::tr("equals")},
                {Op::NotEqual, SearchFilterBuilder::tr("does not equal")},
                {Op::Greater, SearchFilterBuilder::tr("is greater than")},
                {Op::GreaterOrEqual, SearchFilterBuilder::tr("is at least")},
                {Op::Less, SearchFilterBuilder::tr("is less than")},
                {Op::LessOrEqual, SearchFilterBuilder::tr("is at most")}
            };
        case Kind::Date:
            return {
                {Op::After, SearchFilterBuilder::tr("is after")},
                {Op::Before, SearchFilterBuilder::tr("is before")},
                {Op::WithinLastDays, SearchFilterBuilder::tr("is within the last")},
                {Op::OlderThanDays, SearchFilterBuilder::tr("is older than")}
            };
        default:
            return {
                {Op::Contains, SearchFilterBuilder::tr("contains")},
                {Op::DoesNotContain, SearchFilterBuilder::tr("does not contain")},
                {Op::ContainsWord, SearchFilterBuilder::tr("contains the word")},
                {Op::DoesNotContainWord, SearchFilterBuilder::tr("does not contain the word")},
                {Op::Is, SearchFilterBuilder::tr("equals")},
                {Op::IsNot, SearchFilterBuilder::tr("does not equal")},
                {Op::StartsWith, SearchFilterBuilder::tr("starts with")},
                {Op::DoesNotStartWith, SearchFilterBuilder::tr("does not start with")},
                {Op::EndsWith, SearchFilterBuilder::tr("ends with")},
                {Op::DoesNotEndWith, SearchFilterBuilder::tr("does not end with")},
                {Op::MatchesPattern, SearchFilterBuilder::tr("matches pattern (regex)")},
                {Op::DoesNotMatchPattern, SearchFilterBuilder::tr("does not match pattern (regex)")}
            };
        }
    }

    const QString KEY_PRESETS = u"Search/FilterBuilder/Presets"_s;

    // Saved filter sets: {"name": {"rules": ["field|op|unit|value", ...], "matchAll": true}}
    QJsonObject loadPresets()
    {
        const QString text = SettingsStorage::instance()->loadValue<QString>(KEY_PRESETS);
        if (text.isEmpty())
        {
            // starting examples (Name contains ...)
            return QJsonObject
            {
                {u"4K Dolby Vision"_s, QJsonObject {{u"rules"_s, QJsonArray {u"0|0|1|2160p"_s, u"0|0|1|DV"_s}}, {u"matchAll"_s, true}}},
                {u"4K Dolby Vision REMUX"_s, QJsonObject {{u"rules"_s, QJsonArray {u"0|0|1|2160p"_s, u"0|0|1|DV"_s, u"0|0|1|REMUX"_s}}, {u"matchAll"_s, true}}}
            };
        }
        return QJsonDocument::fromJson(text.toUtf8()).object();
    }

    void storePresets(const QJsonObject &presets)
    {
        SettingsStorage::instance()->storeValue(KEY_PRESETS, QString::fromUtf8(QJsonDocument(presets).toJson(QJsonDocument::Compact)));
    }

    // size units offered next to the number box
    const QList<std::pair<QString, double>> SIZE_UNITS
    {
        {u"MB"_s, 1024.0 * 1024},
        {u"GB"_s, 1024.0 * 1024 * 1024},
        {u"TB"_s, 1024.0 * 1024 * 1024 * 1024}
    };
}

SearchFilterBuilder::SearchFilterBuilder(QWidget *parent, const Mode mode)
    : QWidget(parent)
    , m_mode(mode)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(4);

    auto *header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);

    m_addButton = new QPushButton(UIThemeManager::instance()->getIcon(u"list-add"_s), tr("Add filter"), this);
    m_addButton->setToolTip(tr("Add a filter rule. Stack as many as you like."));
    connect(m_addButton, &QPushButton::clicked, this, [this]() { addRow(); });
    header->addWidget(m_addButton);

    m_matchBox = new QWidget(this);
    auto *matchLayout = new QHBoxLayout(m_matchBox);
    matchLayout->setContentsMargins(8, 0, 0, 0);
    matchLayout->addWidget(new QLabel(tr("Show results that match"), m_matchBox));
    m_matchMode = new QComboBox(m_matchBox);
    m_matchMode->addItem(tr("all rules (AND)"));
    m_matchMode->addItem(tr("any rule (OR)"));
    connect(m_matchMode, qOverload<int>(&QComboBox::currentIndexChanged), this, &SearchFilterBuilder::onChanged);
    matchLayout->addWidget(m_matchMode);
    header->addWidget(m_matchBox);

    m_clearButton = new QPushButton(UIThemeManager::instance()->getIcon(u"edit-clear"_s), tr("Clear filters"), this);
    connect(m_clearButton, &QPushButton::clicked, this, [this]()
    {
        clearRows();
        onChanged();
    });
    header->addWidget(m_clearButton);

    m_presetsButton = new QToolButton(this);
    m_presetsButton->setText(tr("Presets"));
    m_presetsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_presetsButton->setToolTip(tr("Apply a saved set of filters, or save the current ones"));
    connect(m_presetsButton, &QToolButton::clicked, this, &SearchFilterBuilder::showPresetsMenu);
    header->addWidget(m_presetsButton);
    header->addStretch();

    m_sendWords = new QCheckBox(tr("Include required words in the search"), this);
    m_sendWords->setToolTip(tr("Adds the words from \"Name contains\" rules to the searches sent to the sites, so they return "
        "those releases instead of only their top results for the title. With \"all rules\" the words are searched together; "
        "with \"any rule\" each word gets its own search. The plain search always runs too, and synonyms "
        "(2160p, 4K, UHD...) get their own searches. Other rules filter the results here."));
    connect(m_sendWords, &QCheckBox::toggled, this, &SearchFilterBuilder::onChanged);
    header->addWidget(m_sendWords);

    m_searchAgainButton = new QPushButton(UIThemeManager::instance()->getIcon(u"edit-find"_s)
        , ((mode == Mode::Search) ? tr("Apply && search again") : tr("Search again with these filters")), this);
    m_searchAgainButton->setToolTip((mode == Mode::Search)
        ? tr("Starts a new search with these filters, sending their required words to the sites")
        : tr("Runs this tab's search again from scratch, sending these filters' required words to the sites"));
    connect(m_searchAgainButton, &QPushButton::clicked, this, &SearchFilterBuilder::searchAgainRequested);
    header->addWidget(m_searchAgainButton);
    outer->addLayout(header);

    m_rowsLayout = new QVBoxLayout;
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(2);
    outer->addLayout(m_rowsLayout);

    if (m_mode == Mode::Search)
        load();
    updateHeaderVisibility();
}

void SearchFilterBuilder::addEmptyRow()
{
    addRow();
}

bool SearchFilterBuilder::matchAll() const
{
    return m_matchMode->currentIndex() == 0;
}

QStringList SearchFilterBuilder::searchWords() const
{
    QStringList words;
    if ((m_mode != Mode::Search) || !m_sendWords->isChecked())
        return words;
    for (const Row &row : m_rows)
    {
        const SearchSortModel::FilterRule rule = ruleOf(row);
        if ((rule.field == Field::Name) && ((rule.op == Op::Contains) || (rule.op == Op::ContainsWord)))
        {
            const QString text = rule.text.simplified();
            if (!text.isEmpty())
                words.append(text);
        }
    }
    return words;
}

bool SearchFilterBuilder::sendsWordsToSites() const
{
    return m_sendWords->isChecked();
}

QStringList SearchFilterBuilder::requiredWords() const
{
    QStringList words;
    for (const Row &row : m_rows)
    {
        const SearchSortModel::FilterRule rule = ruleOf(row);
        if ((rule.field == Field::Name) && ((rule.op == Op::Contains) || (rule.op == Op::ContainsWord)))
        {
            const QString text = rule.text.simplified();
            if (!text.isEmpty())
                words.append(text);
        }
    }
    return words;
}

QStringList SearchFilterBuilder::serializedRows() const
{
    QStringList rows;
    for (const Row &row : m_rows)
        rows.append(serialize(row));
    return rows;
}

void SearchFilterBuilder::setRows(const QStringList &rows, const bool matchAll)
{
    m_loading = true;
    clearRows();
    for (const QString &entry : rows)
    {
        // field|op|unit|value (value may contain '|')
        const QStringList parts = entry.split(u'|');
        if (parts.size() < 4)
            continue;
        addRow({parts[0], parts[1], parts.mid(3).join(u'|'), parts[2]});
    }
    m_matchMode->setCurrentIndex(matchAll ? 0 : 1);
    m_loading = false;
    updateHeaderVisibility();
    onChanged();
}

void SearchFilterBuilder::showPresetsMenu()
{
    const QJsonObject presets = loadPresets();

    QMenu menu(this);
    for (auto it = presets.constBegin(); it != presets.constEnd(); ++it)
    {
        const QString name = it.key();
        const QJsonObject preset = it.value().toObject();
        menu.addAction(name, this, [this, preset]()
        {
            QStringList rows;
            for (const QJsonValue &row : preset.value(u"rules").toArray())
                rows.append(row.toString());
            setRows(rows, preset.value(u"matchAll").toBool(true));
        });
    }
    if (!presets.isEmpty())
        menu.addSeparator();

    QAction *saveAction = menu.addAction(tr("Save current filters as a preset..."), this, &SearchFilterBuilder::savePreset);
    saveAction->setEnabled(!m_rows.isEmpty());

    if (!presets.isEmpty())
    {
        QMenu *deleteMenu = menu.addMenu(tr("Delete preset"));
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it)
        {
            const QString name = it.key();
            deleteMenu->addAction(name, this, [name]()
            {
                QJsonObject all = loadPresets();
                all.remove(name);
                storePresets(all);
            });
        }
    }

    menu.exec(m_presetsButton->mapToGlobal(QPoint(0, m_presetsButton->height())));
}

void SearchFilterBuilder::savePreset()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save preset"), tr("Name for these filters:")
        , QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty())
        return;

    QJsonArray rules;
    for (const QString &row : serializedRows())
        rules.append(row);
    QJsonObject all = loadPresets();
    all.insert(name, QJsonObject {{u"rules"_s, rules}, {u"matchAll"_s, matchAll()}});
    storePresets(all);
}

QList<SearchSortModel::FilterRule> SearchFilterBuilder::rules() const
{
    QList<SearchSortModel::FilterRule> result;
    for (const Row &row : m_rows)
        result.append(ruleOf(row));
    return result;
}

SearchSortModel::FilterRule SearchFilterBuilder::ruleOf(const Row &row) const
{
    SearchSortModel::FilterRule rule;
    rule.field = static_cast<Field>(row.field->currentData().toInt());
    rule.op = static_cast<Op>(row.op->currentData().toInt());
    switch (kindOf(rule.field))
    {
    case Kind::Text:
        rule.text = row.text->text();
        break;
    case Kind::Number:
        {
            double value = row.number->value();
            if (rule.field == Field::Size)
                value *= SIZE_UNITS.value(row.sizeUnit->currentIndex()).second;
            rule.text = QString::number(value, 'f', 0);
        }
        break;
    case Kind::Date:
        rule.text = ((rule.op == Op::WithinLastDays) || (rule.op == Op::OlderThanDays))
            ? QString::number(row.days->value())
            : row.date->date().toString(Qt::ISODate);
        break;
    }
    return rule;
}

void SearchFilterBuilder::fillOps(Row &row) const
{
    const QSignalBlocker blocker {row.op};
    row.op->clear();
    for (const auto &[op, label] : opsFor(kindOf(static_cast<Field>(row.field->currentData().toInt()))))
        row.op->addItem(label, static_cast<int>(op));
}

void SearchFilterBuilder::showEditor(Row &row) const
{
    const auto field = static_cast<Field>(row.field->currentData().toInt());
    const auto op = static_cast<Op>(row.op->currentData().toInt());
    switch (kindOf(field))
    {
    case Kind::Text:
        row.editors->setCurrentIndex(TextEditor);
        row.text->setPlaceholderText((op == Op::MatchesPattern) || (op == Op::DoesNotMatchPattern)
            ? tr("e.g. 2160p.*(REMUX|HYBRID)") : tr("e.g. REMUX"));
        break;
    case Kind::Number:
        row.editors->setCurrentIndex(NumberEditor);
        row.sizeUnit->setVisible(field == Field::Size);
        row.number->setDecimals((field == Field::Size) ? 2 : 0);
        break;
    case Kind::Date:
        row.editors->setCurrentIndex(((op == Op::WithinLastDays) || (op == Op::OlderThanDays)) ? DaysEditor : DateEditor);
        break;
    }
}

void SearchFilterBuilder::addRow(const QStringList &saved)
{
    Row row;
    row.widget = new QWidget(this);
    row.widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *layout = new QHBoxLayout(row.widget);
    layout->setContentsMargins(0, 0, 0, 0);

    row.field = new QComboBox(row.widget);
    row.field->addItem(tr("Name"), static_cast<int>(Field::Name));
    row.field->addItem(tr("Size"), static_cast<int>(Field::Size));
    row.field->addItem(tr("Seeders"), static_cast<int>(Field::Seeds));
    row.field->addItem(tr("Leechers"), static_cast<int>(Field::Leeches));
    row.field->addItem(tr("Engine"), static_cast<int>(Field::Engine));
    row.field->addItem(tr("Published"), static_cast<int>(Field::PubDate));
    layout->addWidget(row.field);

    row.op = new QComboBox(row.widget);
    row.op->setToolTip(tr("\"The word\" matches whole words only, so DV doesn't match DVD."));
    layout->addWidget(row.op);

    row.editors = new QStackedWidget(row.widget);
    row.text = new QLineEdit;
    row.text->setClearButtonEnabled(true);
    row.editors->addWidget(row.text);

    auto *numberBox = new QWidget;
    auto *numberLayout = new QHBoxLayout(numberBox);
    numberLayout->setContentsMargins(0, 0, 0, 0);
    row.number = new QDoubleSpinBox;
    row.number->setRange(0, 1e9);
    numberLayout->addWidget(row.number, 1);
    row.sizeUnit = new QComboBox;
    for (const auto &unit : SIZE_UNITS)
        row.sizeUnit->addItem(unit.first);
    row.sizeUnit->setCurrentIndex(1);
    numberLayout->addWidget(row.sizeUnit);
    row.editors->addWidget(numberBox);

    row.date = new QDateEdit(QDate::currentDate().addYears(-1));
    row.date->setCalendarPopup(true);
    row.date->setDisplayFormat(u"yyyy-MM-dd"_s);
    row.editors->addWidget(row.date);

    row.days = new QSpinBox;
    row.days->setRange(0, 100000);
    row.days->setValue(30);
    row.days->setSuffix(tr(" days"));
    row.editors->addWidget(row.days);
    // keep every row one line tall, whatever editor is showing
    row.editors->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row.editors->setFixedHeight(row.field->sizeHint().height());
    layout->addWidget(row.editors, 1);

    auto *remove = new QToolButton(row.widget);
    remove->setIcon(UIThemeManager::instance()->getIcon(u"list-remove"_s));
    remove->setToolTip(tr("Remove this rule"));
    layout->addWidget(remove);

    // restore a saved rule: field|op|value|unit
    if (saved.size() >= 3)
    {
        row.field->setCurrentIndex(qMax(0, row.field->findData(saved[0].toInt())));
        fillOps(row);
        row.op->setCurrentIndex(qMax(0, row.op->findData(saved[1].toInt())));
        const auto field = static_cast<Field>(row.field->currentData().toInt());
        switch (kindOf(field))
        {
        case Kind::Text:
            row.text->setText(saved[2]);
            break;
        case Kind::Number:
            row.number->setValue(saved[2].toDouble());
            if (saved.size() >= 4)
                row.sizeUnit->setCurrentIndex(qBound(0, saved[3].toInt(), static_cast<int>(SIZE_UNITS.size()) - 1));
            break;
        case Kind::Date:
            if (saved[2].contains(u'-'))
                row.date->setDate(QDate::fromString(saved[2], Qt::ISODate));
            else
                row.days->setValue(saved[2].toInt());
            break;
        }
    }
    else
    {
        fillOps(row);
    }
    showEditor(row);

    QWidget *rowWidget = row.widget;
    connect(row.field, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, rowWidget]()
    {
        if (Row *r = findRow(rowWidget))
        {
            fillOps(*r);
            showEditor(*r);
        }
        onChanged();
    });
    connect(row.op, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, rowWidget]()
    {
        if (Row *r = findRow(rowWidget))
            showEditor(*r);
        onChanged();
    });
    connect(row.text, &QLineEdit::textChanged, this, &SearchFilterBuilder::onChanged);
    connect(row.number, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &SearchFilterBuilder::onChanged);
    connect(row.sizeUnit, qOverload<int>(&QComboBox::currentIndexChanged), this, &SearchFilterBuilder::onChanged);
    connect(row.date, &QDateEdit::dateChanged, this, &SearchFilterBuilder::onChanged);
    connect(row.days, qOverload<int>(&QSpinBox::valueChanged), this, &SearchFilterBuilder::onChanged);
    connect(remove, &QToolButton::clicked, this, [this, rowWidget]() { removeRow(rowWidget); });

    m_rows.append(row);
    m_rowsLayout->addWidget(row.widget);
    updateHeaderVisibility();

    if (!m_loading)
    {
        row.text->setFocus();
        onChanged();
    }
}

SearchFilterBuilder::Row *SearchFilterBuilder::findRow(QWidget *rowWidget)
{
    for (Row &row : m_rows)
    {
        if (row.widget == rowWidget)
            return &row;
    }
    return nullptr;
}

void SearchFilterBuilder::removeRow(QWidget *rowWidget)
{
    for (qsizetype i = 0; i < m_rows.size(); ++i)
    {
        if (m_rows[i].widget == rowWidget)
        {
            m_rows.removeAt(i);
            break;
        }
    }
    rowWidget->deleteLater();
    updateHeaderVisibility();
    onChanged();
}

void SearchFilterBuilder::clearRows()
{
    for (const Row &row : m_rows)
        row.widget->deleteLater();
    m_rows.clear();
    updateHeaderVisibility();
}

void SearchFilterBuilder::updateHeaderVisibility()
{
    m_matchBox->setVisible(m_rows.size() > 1);
    m_clearButton->setVisible(!m_rows.isEmpty());
    const bool searchMode = (m_mode == Mode::Search);
    m_addButton->setVisible(searchMode || !m_rows.isEmpty());
    m_sendWords->setVisible(searchMode && !m_rows.isEmpty());
    m_searchAgainButton->setVisible(!m_rows.isEmpty());
    m_presetsButton->setVisible(searchMode || !m_rows.isEmpty());

}

void SearchFilterBuilder::onChanged()
{
    if (m_loading)
        return;
    updateHeaderVisibility();
    if (m_mode == Mode::Search)
        save();
    emit rulesChanged();
}

QString SearchFilterBuilder::serialize(const Row &row) const
{
    const auto field = static_cast<Field>(row.field->currentData().toInt());
    const auto op = static_cast<Op>(row.op->currentData().toInt());
    QString value;
    switch (kindOf(field))
    {
    case Kind::Text:
        value = row.text->text();
        break;
    case Kind::Number:
        value = QString::number(row.number->value());
        break;
    case Kind::Date:
        value = ((op == Op::WithinLastDays) || (op == Op::OlderThanDays))
            ? QString::number(row.days->value()) : row.date->date().toString(Qt::ISODate);
        break;
    }
    // value goes last but one; text values may contain '|', so it's joined back on load
    return QString::number(static_cast<int>(field)) + u'|' + QString::number(static_cast<int>(op))
        + u'|' + QString::number(row.sizeUnit->currentIndex()) + u'|' + value;
}

void SearchFilterBuilder::load()
{
    m_loading = true;
    const auto *storage = SettingsStorage::instance();
    const QStringList saved = storage->loadValue<QStringList>(KEY_RULES);
    for (const QString &entry : saved)
    {
        // stored as field|op|unit|value (value may contain '|')
        const QStringList parts = entry.split(u'|');
        if (parts.size() < 4)
            continue;
        const QString value = parts.mid(3).join(u'|');
        addRow({parts[0], parts[1], value, parts[2]});
    }
    m_matchMode->setCurrentIndex(storage->loadValue<bool>(KEY_MATCH_ALL, true) ? 0 : 1);
    m_sendWords->setChecked(storage->loadValue<bool>(KEY_SEND_WORDS, true));
    m_loading = false;
    updateHeaderVisibility();
}

void SearchFilterBuilder::save() const
{
    QStringList saved;
    for (const Row &row : m_rows)
        saved.append(serialize(row));
    auto *storage = SettingsStorage::instance();
    storage->storeValue(KEY_RULES, saved);
    storage->storeValue(KEY_MATCH_ALL, matchAll());
    storage->storeValue(KEY_SEND_WORDS, m_sendWords->isChecked());
}
