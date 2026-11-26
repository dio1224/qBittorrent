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

#include <functional>
#include <utility>

#include <QList>
#include <QString>

#include <QObject>

class QMenu;
class QWidget;

// The "Extras" menu on the Search tab: Cloudflare helper setup, a fresh
// Cloudflare pass, the RuTracker login, and search options (synonyms,
// duplicate merging, how many searches run).
namespace DioExtrasMenu
{
    QMenu *create(QWidget *parent, const std::function<void ()> &onSearchOptionsChanged = {});

    // Asks once whether to set up the Cloudflare helper if it's missing.
    void offerHelperSetupOnce(QWidget *parent);

    void setUpHelper(QWidget *parent);
    void refreshPass(QWidget *parent);
    // Gets a fresh pass for each site (name, address) in turn; true when at least one worked
    bool refreshPassForSites(QWidget *parent, const QList<std::pair<QString, QString>> &sites);
    void editRuTrackerLogin(QWidget *parent);
    void editSearchOptions(QWidget *parent, const std::function<void ()> &onChanged);
}
