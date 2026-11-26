/*
 * Bittorrent Client using Qt and libtorrent.
 * Dio's build: reads quality and identity out of release names
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#pragma once

#include <QString>
#include <QStringList>

// What a release name says about the video: "Dune.Part.Two.2024.2160p.UHD.BluRay.REMUX.DV.HDR10.HEVC.TrueHD.7.1.Atmos-FGT"
struct ReleaseInfo
{
    // Quality (0 / empty when the name doesn't say)
    int resolution = 0;         // 2160, 1080, 720, 480
    int hdr = 0;                // 3 Dolby Vision, 2 HDR10+, 1 HDR10/HDR/HLG, 0 none
    QString hdrText;            // "DV HDR10"
    int source = -1;            // 6 REMUX, 5 Blu-ray encode, 4 WEB-DL, 3 WEBRip, 2 HDTV, 1 DVD, 0 CAM/TS; -1 unknown
    QString sourceText;
    QString codec;              // HEVC, AVC, AV1, VC-1, MPEG-2, XviD
    int audio = 0;              // 5 Atmos/DTS:X, 4 lossless (TrueHD, DTS-HD MA, FLAC, PCM), 3 DD+/DTS, 2 DD, 1 AAC/Opus/MP3
    QString audioText;          // "TrueHD Atmos 7.1"
    bool reencoded = false;     // names an encoder (x264/x265)
    bool suspiciousFile = false;

    // Identity
    QString title;              // normalized (see normalizeTitle)
    int year = 0;
    int season = -1;
    int episode = -1;           // -1 with season >= 0: a season pack

    bool isEpisode() const { return season >= 0; }
    bool isMovie() const { return (season < 0) && (year > 0) && !title.isEmpty(); }

    // Source quality in three steps, for comparing versions: 3 REMUX, 2 encode/WEB-DL, 1 WEBRip/HDTV, 0 DVD/CAM
    int sourceTier() const;
    // Reasons the listing looks fake or poor, for the Warning column
    QStringList warnings(qint64 size) const;
};

ReleaseInfo parseRelease(const QString &name);

// "Spider-Man: No Way Home" / "Spider.Man.No.Way.Home" -> "spider man no way home"
QString normalizeTitle(const QString &title);

// "2160p", "1080p"... for a resolution number
QString resolutionText(int resolution);
QString hdrTierText(int hdr);
