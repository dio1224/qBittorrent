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

#include <QString>

#include "base/path.h"

// Dio's additions: bundled search plugins and the Cloudflare pass helper that
// several of them use. Everything here ships inside the app, so a fresh install
// gets the same plugins with no Terminal steps.
namespace DioExtras
{
    // Folder of the Cloudflare helper (~/.qbt-cf) and its Python
    Path helperDir();
    Path helperPython();
    bool isHelperInstalled();

    // Installs or updates the bundled plugins (only when the bundled copy is
    // newer than what's installed) and keeps the helper script up to date.
    void syncBundledFiles(const Path &pluginsLocation);

    // Bundled helper requirements (pip lines)
    QString helperRequirements();

    // RuTracker account stored inside the installed RuTracker plugin
    bool hasRuTrackerLogin(const Path &pluginsLocation);
    bool setRuTrackerLogin(const Path &pluginsLocation, const QString &username, const QString &password);
}
