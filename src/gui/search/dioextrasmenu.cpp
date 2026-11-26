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

#include "dioextrasmenu.h"

#include <QCoreApplication>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QTemporaryFile>

#include "base/global.h"
#include "base/search/dioextras.h"
#include "base/search/searchpluginmanager.h"
#include "base/settingsstorage.h"
#include "base/utils/foreignapps.h"

namespace
{
    const QString KEY_PROMPTED = u"Search/DioExtras/HelperSetupOffered"_s;

    // Sites whose Cloudflare pass can be renewed from the menu
    const QList<std::pair<QString, QString>> PASS_SITES
    {
        {u"1337x"_s, u"https://1337x.to/"_s},
        {u"EXT Torrents"_s, u"https://ext.to/"_s},
        {u"RuTracker"_s, u"https://rutracker.org/forum/index.php"_s}
    };

    // Runs one step and returns its output; empty error string on success.
    QString runStep(QProgressDialog *progress, const QString &label, const QString &program, const QStringList &args, int timeoutMs)
    {
        progress->setLabelText(label);
        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.start(program, args);
        if (!proc.waitForStarted(15000))
            return QObject::tr("Couldn't start %1.").arg(program);

        int waited = 0;
        while (!proc.waitForFinished(200))
        {
            QCoreApplication::processEvents();
            waited += 200;
            if (progress->wasCanceled() || (waited > timeoutMs))
            {
                proc.kill();
                proc.waitForFinished(3000);
                return progress->wasCanceled() ? QObject::tr("Cancelled.") : QObject::tr("Timed out.");
            }
        }
        if ((proc.exitStatus() != QProcess::NormalExit) || (proc.exitCode() != 0))
            return QString::fromUtf8(proc.readAll().right(1500));
        return {};
    }
}

QMenu *DioExtrasMenu::create(QWidget *parent)
{
    auto *menu = new QMenu(parent);
    menu->addAction(QObject::tr("Set up Cloudflare helper..."), parent, [parent]() { setUpHelper(parent); });
    menu->addAction(QObject::tr("Get a fresh Cloudflare pass..."), parent, [parent]() { refreshPass(parent); });
    menu->addSeparator();
    menu->addAction(QObject::tr("RuTracker login..."), parent, [parent]() { editRuTrackerLogin(parent); });
    return menu;
}

void DioExtrasMenu::offerHelperSetupOnce(QWidget *parent)
{
    if (DioExtras::isHelperInstalled())
        return;
    auto *storage = SettingsStorage::instance();
    if (storage->loadValue<bool>(KEY_PROMPTED, false))
        return;
    storage->storeValue(KEY_PROMPTED, true);

    const auto answer = QMessageBox::question(parent, QObject::tr("Cloudflare helper")
        , QObject::tr("1337x, EXT Torrents and RuTracker sit behind Cloudflare and need a small helper to search.\n\n"
            "Set it up now? It takes a minute or two. You can also do it later from Extras.")
        , (QMessageBox::Yes | QMessageBox::No), QMessageBox::Yes);
    if (answer == QMessageBox::Yes)
        setUpHelper(parent);
}

void DioExtrasMenu::setUpHelper(QWidget *parent)
{
    const QString systemPython = Utils::ForeignApps::pythonInfo().executablePath.data();
    if (systemPython.isEmpty())
    {
        QMessageBox::warning(parent, QObject::tr("Cloudflare helper")
            , QObject::tr("Python wasn't found. On a Mac, open Terminal and run:\n\nxcode-select --install\n\nThen try again."));
        return;
    }

    QTemporaryFile requirements;
    if (!requirements.open())
        return;
    requirements.write(DioExtras::helperRequirements().toUtf8());
    requirements.flush();

    QProgressDialog progress {QObject::tr("Setting up..."), QObject::tr("Cancel"), 0, 0, parent};
    progress.setWindowTitle(QObject::tr("Cloudflare helper"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();

    const QString dir = DioExtras::helperDir().data();
    QString error;
    if (!DioExtras::isHelperInstalled())
        error = runStep(&progress, QObject::tr("Creating the helper's Python environment..."), systemPython, {u"-m"_s, u"venv"_s, dir}, 180000);
    if (error.isEmpty())
    {
        error = runStep(&progress, QObject::tr("Installing the helper (downloads about 30 MB)...")
            , DioExtras::helperPython().data(), {u"-m"_s, u"pip"_s, u"install"_s, u"--upgrade"_s, u"-r"_s, requirements.fileName()}, 900000);
    }
    progress.close();

    if (!error.isEmpty())
    {
        QMessageBox::warning(parent, QObject::tr("Cloudflare helper"), QObject::tr("Setup didn't finish:\n\n%1").arg(error));
        return;
    }

    // puts cfpass.py in place now that the folder exists
    DioExtras::syncBundledFiles(SearchPluginManager::pluginsLocation());

    const auto answer = QMessageBox::question(parent, QObject::tr("Cloudflare helper")
        , QObject::tr("The helper is set up.\n\nGet a Cloudflare pass for 1337x now? Chrome opens on the site; "
            "if you see \"Verify you are human\", click it. The pass lasts a long time.")
        , (QMessageBox::Yes | QMessageBox::No), QMessageBox::Yes);
    if (answer == QMessageBox::Yes)
        refreshPass(parent);
}

void DioExtrasMenu::refreshPass(QWidget *parent)
{
    if (!DioExtras::isHelperInstalled())
    {
        setUpHelper(parent);
        return;
    }

    QStringList names;
    for (const auto &site : PASS_SITES)
        names.append(site.first);
    bool ok = false;
    const QString choice = QInputDialog::getItem(parent, QObject::tr("Cloudflare pass")
        , QObject::tr("Which site?\nChrome opens on it. If you see \"Verify you are human\", click it."), names, 0, false, &ok);
    if (!ok)
        return;

    QString url;
    for (const auto &site : PASS_SITES)
    {
        if (site.first == choice)
            url = site.second;
    }

    QProgressDialog progress {QString(), QObject::tr("Cancel"), 0, 0, parent};
    progress.setWindowTitle(QObject::tr("Cloudflare pass"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    const QString error = runStep(&progress, QObject::tr("Waiting for %1 in Chrome...").arg(choice)
        , DioExtras::helperPython().data(), {(DioExtras::helperDir() / Path(u"cfpass.py"_s)).data(), u"refresh"_s, url}, 200000);
    progress.close();

    if (error.isEmpty())
        QMessageBox::information(parent, QObject::tr("Cloudflare pass"), QObject::tr("Pass saved for %1.").arg(choice));
    else
        QMessageBox::warning(parent, QObject::tr("Cloudflare pass"), QObject::tr("Couldn't get a pass:\n\n%1").arg(error));
}

void DioExtrasMenu::editRuTrackerLogin(QWidget *parent)
{
    const Path plugins = SearchPluginManager::pluginsLocation();
    if (!QFile::exists((plugins / Path(u"rutracker.py"_s)).data()))
    {
        QMessageBox::warning(parent, QObject::tr("RuTracker"), QObject::tr("The RuTracker plugin isn't installed."));
        return;
    }

    bool ok = false;
    const QString user = QInputDialog::getText(parent, QObject::tr("RuTracker login")
        , QObject::tr("RuTracker username:"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || user.isEmpty())
        return;
    const QString pass = QInputDialog::getText(parent, QObject::tr("RuTracker login")
        , QObject::tr("RuTracker password:"), QLineEdit::Password, {}, &ok);
    if (!ok || pass.isEmpty())
        return;

    if (DioExtras::setRuTrackerLogin(plugins, user, pass))
        QMessageBox::information(parent, QObject::tr("RuTracker login"), QObject::tr("Saved. It's stored only in the RuTracker plugin on this Mac."));
    else
        QMessageBox::warning(parent, QObject::tr("RuTracker login"), QObject::tr("Couldn't save the login."));
}
