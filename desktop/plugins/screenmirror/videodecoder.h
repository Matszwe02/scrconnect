/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#pragma once

#include <QImage>

#include "scrcpyprotocol.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

// scrcpy's codec configuration frames do not decode alone: they are prepended to the next frame
class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    Q_DISABLE_COPY(VideoDecoder)

    static QString preferredCodec();

    bool open(Scrcpy::Codec codec);
    void close();
    void flush();
    bool decode(const Scrcpy::VideoEvent &frame, QImage &image);

private:
    AVCodecContext *m_context = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_sws = nullptr;
    QByteArray m_config;
};
