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

#include "dioextras.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>

#include "base/global.h"
#include "base/logger.h"
#include "base/search/searchpluginmanager.h"
#include "base/utils/fs.h"

namespace
{
    const QString BUNDLED_ENGINES = u":/dioextras/engines"_s;
    const QString BUNDLED_HELPER = u":/dioextras/cfhelper/cfpass.py"_s;
    const QString BUNDLED_REQUIREMENTS = u":/dioextras/cfhelper/requirements.txt"_s;
    const QString RUTRACKER_FILE = u"rutracker.py"_s;
    const QString PLACEHOLDER_USER = u"YOUR_USERNAME_HERE"_s;
    const QString PLACEHOLDER_PASS = u"YOUR_PASSWORD_HERE"_s;

    QByteArray readAll(const QString &path)
    {
        QFile file {path};
        if (!file.open(QIODevice::ReadOnly))
            return {};
        return file.readAll();
    }

    bool writeAll(const QString &path, const QByteArray &data)
    {
        QFile file {path};
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        return (file.write(data) == data.size());
    }

    // Python string literal for a value (repr-style, single quotes)
    QString pyQuote(QString value)
    {
        value.replace(u'\\', u"\\\\"_s);
        value.replace(u'\'', u"\\'"_s);
        value.replace(u'\n', u"\\n"_s);
        return u'\'' + value + u'\'';
    }

    // Reads "username = ..." / "password = ..." lines from a RuTracker plugin file
    QString configLine(const QString &source, const QString &key)
    {
        const QRegularExpression re {u"^(\\s*)"_s + key + u"\\s*=\\s*(.+)$"_s, QRegularExpression::MultilineOption};
        const QRegularExpressionMatch m = re.match(source);
        return m.hasMatch() ? m.captured(2).trimmed() : QString();
    }

    QString replaceConfigLine(QString source, const QString &key, const QString &pyValue)
    {
        const QRegularExpression re {u"^(\\s*)"_s + key + u"\\s*=\\s*.+$"_s, QRegularExpression::MultilineOption};
        const QRegularExpressionMatch m = re.match(source);
        if (m.hasMatch())
            source.replace(m.capturedStart(), m.capturedLength(), m.captured(1) + key + u" = "_s + pyValue);
        return source;
    }
}

Path DioExtras::helperDir()
{
    return Path(QDir::homePath()) / Path(u".qbt-cf"_s);
}

Path DioExtras::helperPython()
{
    return helperDir() / Path(u"bin/python"_s);
}

bool DioExtras::isHelperInstalled()
{
    return QFile::exists(helperPython().data());
}

QString DioExtras::helperRequirements()
{
    return QString::fromUtf8(readAll(BUNDLED_REQUIREMENTS));
}

void DioExtras::syncBundledFiles(const Path &pluginsLocation)
{
    QDirIterator iter {BUNDLED_ENGINES, {u"*.py"_s}, QDir::Files};
    while (iter.hasNext())
    {
        const QString bundledPath = iter.next();
        const QString fileName = iter.fileName();
        const Path installedPath = pluginsLocation / Path(fileName);

        const auto bundledVersion = SearchPluginManager::getPluginVersion(Path(bundledPath));
        const bool exists = QFile::exists(installedPath.data());
        if (exists && (bundledVersion <= SearchPluginManager::getPluginVersion(installedPath)))
            continue;

        QByteArray data = readAll(bundledPath);
        if (exists && (fileName == RUTRACKER_FILE))
        {
            // keep the account already typed into the installed copy
            const QString old = QString::fromUtf8(readAll(installedPath.data()));
            QString updated = QString::fromUtf8(data);
            for (const QString &key : {u"username"_s, u"password"_s})
            {
                const QString value = configLine(old, key);
                if (!value.isEmpty() && !value.contains(u"YOUR_"_s))
                    updated = replaceConfigLine(updated, key, value);
            }
            data = updated.toUtf8();
        }

        if (writeAll(installedPath.data(), data))
        {
            QFile::setPermissions(installedPath.data(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            LogMsg(QObject::tr("Installed bundled search plugin: %1").arg(fileName));
        }
    }

    // keep the Cloudflare helper script current (only if the helper folder exists)
    const Path helperScript = helperDir() / Path(u"cfpass.py"_s);
    if (QFile::exists(helperDir().data()))
    {
        const QByteArray bundled = readAll(BUNDLED_HELPER);
        if (!bundled.isEmpty() && (readAll(helperScript.data()) != bundled))
            writeAll(helperScript.data(), bundled);
    }
}

bool DioExtras::hasRuTrackerLogin(const Path &pluginsLocation)
{
    const QString source = QString::fromUtf8(readAll((pluginsLocation / Path(RUTRACKER_FILE)).data()));
    const QString user = configLine(source, u"username"_s);
    return !user.isEmpty() && !user.contains(PLACEHOLDER_USER);
}

bool DioExtras::setRuTrackerLogin(const Path &pluginsLocation, const QString &username, const QString &password)
{
    const Path path = pluginsLocation / Path(RUTRACKER_FILE);
    QString source = QString::fromUtf8(readAll(path.data()));
    if (source.isEmpty())
        return false;
    source = replaceConfigLine(source, u"username"_s, pyQuote(username));
    source = replaceConfigLine(source, u"password"_s, pyQuote(password));
    if (!writeAll(path.data(), source.toUtf8()))
        return false;
    // only you can read it
    QFile::setPermissions(path.data(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}
