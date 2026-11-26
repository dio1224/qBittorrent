/*
 * Bittorrent Client using Qt and libtorrent.
 * Dio's build: reads quality and identity out of release names
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include "releaseinfo.h"

#include <algorithm>
#include <utility>

#include <QList>
#include <QRegularExpression>

#include "base/global.h"

namespace
{
    // Patterns run on the lowercase name. "Word" edges are any non-letter/digit,
    // so "DV" matches "...DV.HDR10..." but not "DVD".
    QRegularExpression re(const QString &pattern)
    {
        return QRegularExpression(pattern, QRegularExpression::UseUnicodePropertiesOption);
    }

    bool has(const QString &text, const QRegularExpression &regex)
    {
        return regex.match(text).hasMatch();
    }

    const QRegularExpression &tokenSplitter()
    {
        static const QRegularExpression splitter {u"[^\\p{L}\\p{N}]+"_s};
        return splitter;
    }

    // Tokens that start the technical part of a name ("...2010.2160p.BluRay...")
    bool isQualityToken(const QString &token)
    {
        static const QRegularExpression quality {u"^(2160p|1080p|1080i|720p|576p|480p|4k|uhd|bluray|bdrip|brrip|remux|bdremux|web|webdl|webrip|hdtv|dvdrip|dvd|x264|x265|h264|h265|hevc|avc|hdr|hdr10|dv|dovi|proper|repack|extended|unrated|remastered|imax|internal|limited|complete)$"_s};
        return quality.match(token).hasMatch();
    }

    bool isYear(const QString &token)
    {
        if (token.size() != 4)
            return false;
        bool ok = false;
        const int value = token.toInt(&ok);
        return ok && (value >= 1900) && (value <= 2100);
    }

    QString joinTitle(const QStringList &tokens, const qsizetype count)
    {
        return normalizeTitle(tokens.mid(0, count).join(u' '));
    }
}

int ReleaseInfo::sourceTier() const
{
    if (source >= 6)
        return 3;
    if ((source == 5) || (source == 4) || (source < 0))
        return 2;
    if ((source == 3) || (source == 2))
        return 1;
    return 0;
}

QStringList ReleaseInfo::warnings(const qint64 size) const
{
    QStringList list;
    if (source == 0)
        list.append(u"Theater recording (CAM/TS)"_s);
    if ((source == 6) && reencoded)
        list.append(u"Says REMUX but also names an encoder (x264/x265), so it was re-encoded or mislabeled"_s);
    if (suspiciousFile)
        list.append(u"Name looks like a program or archive, not a video"_s);

    // Size checks only where the name clearly is a movie or a single episode
    const bool singleEpisode = isEpisode() && (episode >= 0);
    if ((size > 0) && (isMovie() || singleEpisode))
    {
        const double gib = static_cast<double>(size) / (1024.0 * 1024.0 * 1024.0);
        const QString sizeText = QString::number(gib, 'f', (gib < 10) ? 1 : 0) + u" GB"_s;
        if (source == 6)
        {
            double minimum = 0;
            if (resolution >= 2160)
                minimum = singleEpisode ? 3 : 15;
            else if (resolution == 1080)
                minimum = singleEpisode ? 1.5 : 8;
            if ((minimum > 0) && (gib < minimum))
                list.append(u"Too small for a %1 REMUX (%2)"_s.arg(resolutionText(resolution), sizeText));
        }
        else if (resolution >= 2160)
        {
            if (gib < (singleEpisode ? 0.3 : 1.5))
                list.append(u"Too small for 4K (%1)"_s.arg(sizeText));
        }
        else if (resolution == 1080)
        {
            if (gib < (singleEpisode ? 0.15 : 0.4))
                list.append(u"Too small for 1080p (%1)"_s.arg(sizeText));
        }
    }
    return list;
}

ReleaseInfo parseRelease(const QString &name)
{
    ReleaseInfo info;
    const QString n = name.toLower();

    // --- Identity: title, year, season/episode
    const QStringList tokens = n.split(tokenSplitter(), Qt::SkipEmptyParts);
    static const QRegularExpression episodeToken {u"^s(\\d{1,2})e(\\d{1,3})(e\\d{1,3})*$"_s};
    static const QRegularExpression seasonToken {u"^s(\\d{1,2})$"_s};
    static const QRegularExpression crossToken {u"^(\\d{1,2})x(\\d{2,3})$"_s};
    for (qsizetype i = 1; i < tokens.size(); ++i)
    {
        const QString &token = tokens[i];
        if (const auto m = episodeToken.match(token); m.hasMatch())
        {
            info.season = m.captured(1).toInt();
            info.episode = m.captured(2).toInt();
        }
        else if (const auto m2 = seasonToken.match(token); m2.hasMatch())
        {
            info.season = m2.captured(1).toInt();
        }
        else if (const auto m3 = crossToken.match(token); m3.hasMatch())
        {
            info.season = m3.captured(1).toInt();
            info.episode = m3.captured(2).toInt();
        }
        else if ((token == u"season") && ((i + 1) < tokens.size()) && (tokens[i + 1].toInt() > 0))
        {
            info.season = tokens[i + 1].toInt();
        }
        else
        {
            continue;
        }
        // "Show.Name.2019.S01E02": drop a year right before the episode tag from the title
        qsizetype titleEnd = i;
        if ((titleEnd > 1) && isYear(tokens[titleEnd - 1]))
        {
            info.year = tokens[titleEnd - 1].toInt();
            --titleEnd;
        }
        info.title = joinTitle(tokens, titleEnd);
        break;
    }

    // Adult scene names ("Studio 2024 Performer Title XXX 1080p") aren't movies
    if ((info.season < 0) && !tokens.contains(u"xxx"_s))
    {
        qsizetype qualityStart = tokens.size();
        for (qsizetype i = 1; i < tokens.size(); ++i)
        {
            if (isQualityToken(tokens[i]))
            {
                qualityStart = i;
                break;
            }
        }
        // the last year before the technical part: "Blade.Runner.2049.2017.2160p" -> 2017.
        // Movie names put the year right before it (a few tags like "German DL" may sit between).
        for (qsizetype i = qualityStart - 1; i >= 1; --i)
        {
            if (isYear(tokens[i]))
            {
                if ((qualityStart - i) <= 4)
                {
                    info.year = tokens[i].toInt();
                    info.title = joinTitle(tokens, i);
                }
                break;
            }
        }
    }

    // --- Resolution
    static const QRegularExpression res2160 = re(u"(?<![a-z0-9])(2160p|4k|uhd)(?![a-z0-9])|(?<![0-9])2160(?![0-9])"_s);
    static const QRegularExpression res1080 = re(u"(?<![0-9])1080[pi]?(?![0-9a-z])"_s);
    static const QRegularExpression res720 = re(u"(?<![0-9])720p(?![a-z0-9])"_s);
    static const QRegularExpression resSD = re(u"(?<![0-9])(576|480)p(?![a-z0-9])"_s);
    if (has(n, res2160))
        info.resolution = 2160;
    else if (has(n, res1080))
        info.resolution = 1080;
    else if (has(n, res720))
        info.resolution = 720;
    else if (has(n, resSD))
        info.resolution = 480;

    // --- HDR
    static const QRegularExpression dv = re(u"(?<![a-z0-9])(dv|dovi)(?![a-z])|dolby[ ._-]?vision"_s);
    static const QRegularExpression hdr10plus = re(u"hdr10(\\+|plus|p)(?![a-z0-9])"_s);
    static const QRegularExpression hdr = re(u"(?<![a-z0-9])hdr(10)?(?![a-z+])"_s);
    static const QRegularExpression hlg = re(u"(?<![a-z0-9])hlg(?![a-z0-9])"_s);
    QStringList hdrParts;
    if (has(n, dv))
    {
        info.hdr = 3;
        hdrParts.append(u"DV"_s);
    }
    if (has(n, hdr10plus))
    {
        info.hdr = std::max(info.hdr, 2);
        hdrParts.append(u"HDR10+"_s);
    }
    else if (const auto m = hdr.match(n); m.hasMatch())
    {
        info.hdr = std::max(info.hdr, 1);
        hdrParts.append(m.captured(1).isEmpty() ? u"HDR"_s : u"HDR10"_s);
    }
    else if (has(n, hlg))
    {
        info.hdr = std::max(info.hdr, 1);
        hdrParts.append(u"HLG"_s);
    }
    // only the best format: DV beats HDR10+, which beats HDR10/HDR; nothing means SDR
    info.hdrText = hdrParts.isEmpty() ? u"SDR"_s : hdrParts.first();

    // --- Codec
    static const QRegularExpression x26x = re(u"(?<![a-z0-9])x26[45](?![a-z0-9])"_s);
    info.reencoded = has(n, x26x);
    static const QList<std::pair<QRegularExpression, QString>> codecs
    {
        {re(u"(?<![a-z0-9])(x265|h[ ._]?265|hevc)(?![a-z0-9])"_s), u"HEVC"_s},
        {re(u"(?<![a-z0-9])av1(?![a-z0-9])"_s), u"AV1"_s},
        {re(u"(?<![a-z0-9])(x264|h[ ._]?264|avc)(?![a-z0-9])"_s), u"AVC"_s},
        {re(u"(?<![a-z0-9])vc[ ._-]?1(?![0-9])"_s), u"VC-1"_s},
        {re(u"(?<![a-z0-9])mpeg[ ._-]?2(?![0-9])"_s), u"MPEG-2"_s},
        {re(u"(?<![a-z0-9])(xvid|divx)(?![a-z0-9])"_s), u"XviD"_s}
    };
    for (const auto &[regex, label] : codecs)
    {
        if (has(n, regex))
        {
            info.codec = label;
            break;
        }
    }

    // --- Source
    static const QRegularExpression remux = re(u"(?<![a-z0-9])(bd)?remux(?![a-z0-9])"_s);
    static const QRegularExpression fullDisc = re(u"(?<![a-z0-9])(bdmv|bd(25|50|66|100))(?![a-z0-9])|complete[ ._-]?(uhd[ ._-]?)?blu[ ._-]?ray"_s);
    static const QRegularExpression camStrong = re(u"(?<![a-z0-9])(cam|camrip|hdcam|hdts|telesync|telecine|hdtc|dvdscr|screener)(?![a-z0-9])"_s);
    static const QRegularExpression camWeak = re(u"(?<![a-z0-9])(ts|tc|scr)(?![a-z0-9])"_s);
    static const QRegularExpression bluray = re(u"(?<![a-z0-9])(blu[ ._-]?ray|bdrip|brrip)(?![a-z0-9])"_s);
    static const QRegularExpression webdl = re(u"(?<![a-z0-9])web[ ._-]?dl(?![a-z0-9])"_s);
    static const QRegularExpression webrip = re(u"(?<![a-z0-9])web[ ._-]?rip(?![a-z0-9])"_s);
    static const QRegularExpression web = re(u"(?<![a-z0-9])web(?![a-z0-9])"_s);
    static const QRegularExpression hdtv = re(u"(?<![a-z0-9])(hdtv|pdtv|dsr|tvrip)(?![a-z0-9])"_s);
    static const QRegularExpression dvd = re(u"(?<![a-z0-9])(dvdrip|dvdr|dvd|dvd5|dvd9)(?![a-z0-9])"_s);
    if (has(n, remux))
    {
        info.source = 6;
        info.sourceText = u"REMUX"_s;
    }
    else if (has(n, fullDisc))
    {
        info.source = 6;
        info.sourceText = u"Full disc"_s;
    }
    else if (has(n, camStrong) || (info.isMovie() && has(n, camWeak)))
    {
        // "TS" / "TC" only count for movies: in other names they often mean something else
        info.source = 0;
        info.sourceText = u"CAM/TS"_s;
    }
    else if (has(n, bluray))
    {
        info.source = 5;
        info.sourceText = u"Blu-ray"_s;
    }
    else if (has(n, webdl))
    {
        info.source = 4;
        info.sourceText = u"WEB-DL"_s;
    }
    else if (has(n, webrip))
    {
        info.source = 3;
        info.sourceText = u"WEBRip"_s;
    }
    else if (has(n, web))
    {
        info.source = 4;
        info.sourceText = u"WEB"_s;
    }
    else if (has(n, hdtv))
    {
        info.source = 2;
        info.sourceText = u"HDTV"_s;
    }
    else if (has(n, dvd))
    {
        info.source = 1;
        info.sourceText = u"DVD"_s;
        if (info.resolution == 0)
            info.resolution = 480;
    }

    // --- Audio
    static const QRegularExpression atmos = re(u"(?<![a-z0-9])atmos(?![a-z0-9])"_s);
    static const QRegularExpression dtsx = re(u"dts[ ._:-]?x(?![a-z0-9])"_s);
    static const QRegularExpression truehd = re(u"true[ ._-]?hd"_s);
    static const QRegularExpression dtsma = re(u"dts[ ._-]?(hd[ ._-]?)?ma(?![a-z])"_s);
    static const QRegularExpression dtshd = re(u"dts[ ._-]?hd"_s);
    static const QRegularExpression flac = re(u"(?<![a-z0-9])flac(?![a-z])"_s);
    static const QRegularExpression pcm = re(u"(?<![a-z0-9])l?pcm(?![a-z])"_s);
    static const QRegularExpression ddp = re(u"(?<![a-z0-9])(ddp|eac3|e[ ._-]ac[ ._-]?3)(?![a-z])|(?<![a-z0-9])dd\\+|dolby[ ._-]?digital[ ._-]?plus"_s);
    static const QRegularExpression dts = re(u"(?<![a-z0-9])dts(?![a-z])"_s);
    static const QRegularExpression dd = re(u"(?<![a-z0-9])(dd|ac3|ac[ ._-]3)(?![a-z+])"_s);
    static const QRegularExpression lossy = re(u"(?<![a-z0-9])(aac|opus|mp3)(?![a-z])"_s);
    static const QRegularExpression channels = re(u"(?<![0-9])([257])[ ._]([01])(?![0-9])"_s);

    QString base;
    if (has(n, dtsx))
    {
        base = u"DTS:X"_s;
        info.audio = 5;
    }
    else if (has(n, truehd))
    {
        base = u"TrueHD"_s;
        info.audio = 4;
    }
    else if (has(n, dtsma))
    {
        base = u"DTS-HD MA"_s;
        info.audio = 4;
    }
    else if (has(n, flac))
    {
        base = u"FLAC"_s;
        info.audio = 4;
    }
    else if (has(n, pcm))
    {
        base = u"PCM"_s;
        info.audio = 4;
    }
    else if (has(n, ddp))
    {
        base = u"DD+"_s;
        info.audio = 3;
    }
    else if (has(n, dtshd))
    {
        base = u"DTS-HD"_s;
        info.audio = 3;
    }
    else if (has(n, dts))
    {
        base = u"DTS"_s;
        info.audio = 3;
    }
    else if (has(n, dd))
    {
        base = u"DD"_s;
        info.audio = 2;
    }
    else if (const auto m = lossy.match(n); m.hasMatch())
    {
        base = m.captured(1).toUpper();
        info.audio = 1;
    }

    if (has(n, atmos))
    {
        // the top feature says enough: "TrueHD Atmos 7.1" -> "Atmos"
        info.audio = 5;
        info.audioText = u"Atmos"_s;
    }
    else if (info.audio == 5)
    {
        info.audioText = base;  // DTS:X
    }
    else
    {
        if (const auto m = channels.match(n); m.hasMatch() && (!base.isEmpty() || info.isMovie() || info.isEpisode()))
            base += (base.isEmpty() ? QString() : u" "_s) + m.captured(1) + u'.' + m.captured(2);
        info.audioText = base;
    }

    // --- Not a video at all?
    static const QRegularExpression badFile = re(u"\\.(exe|scr|bat|msi|lnk|zip|rar|7z|apk|dmg|pkg)$|(?<![a-z0-9])(password|codec)(?![a-z0-9])"_s);
    info.suspiciousFile = has(n, badFile);

    return info;
}

QString normalizeTitle(const QString &title)
{
    // "Amélie" and "Amelie" compare equal
    QString text = title.normalized(QString::NormalizationForm_KD).toLower();
    text.replace(u'&', u" and "_s);
    text.remove(u'\'');
    text.remove(QChar(0x2019));
    QString out;
    out.reserve(text.size());
    bool pendingSpace = false;
    for (const QChar c : text)
    {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        if (c.isLetterOrNumber())
        {
            if (pendingSpace && !out.isEmpty())
                out.append(u' ');
            out.append(c);
            pendingSpace = false;
        }
        else
        {
            pendingSpace = true;
        }
    }
    return out;
}

QString resolutionText(const int resolution)
{
    switch (resolution)
    {
    case 2160:
        return u"2160p"_s;
    case 1080:
        return u"1080p"_s;
    case 720:
        return u"720p"_s;
    case 480:
        return u"SD"_s;
    default:
        return {};
    }
}

QString hdrTierText(const int hdr)
{
    switch (hdr)
    {
    case 3:
        return u"DV"_s;
    case 2:
        return u"HDR10+"_s;
    case 1:
        return u"HDR"_s;
    default:
        return {};
    }
}
