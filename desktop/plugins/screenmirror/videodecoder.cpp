/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "videodecoder.h"

#include <cstdlib>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

VideoDecoder::~VideoDecoder()
{
    close();
}

namespace
{
const AVCodec *findDecoder(Scrcpy::Codec codec)
{
    if (codec == Scrcpy::Codec::AV1) {
        // FFmpeg's own AV1 decoder only works with hardware acceleration
        const AVCodec *dav1d = avcodec_find_decoder_by_name("libdav1d");
        return dav1d ? dav1d : avcodec_find_decoder(AV_CODEC_ID_AV1);
    }
    return avcodec_find_decoder(codec == Scrcpy::Codec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
}
}

QString VideoDecoder::preferredCodec()
{
    if (findDecoder(Scrcpy::Codec::H264)) {
        return QStringLiteral("h264");
    }
    return findDecoder(Scrcpy::Codec::H265) ? QStringLiteral("h265") : QStringLiteral("av1");
}

bool VideoDecoder::open(Scrcpy::Codec codec)
{
    close();
    const AVCodec *decoder = findDecoder(codec);
    m_context = decoder ? avcodec_alloc_context3(decoder) : nullptr;
    if (!m_context) {
        return false;
    }
    m_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_context->flags2 |= AV_CODEC_FLAG2_FAST;
    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();
    if (avcodec_open2(m_context, decoder, nullptr) < 0 || !m_frame || !m_packet) {
        close();
        return false;
    }
    return true;
}

void VideoDecoder::close()
{
    sws_freeContext(m_sws);
    m_sws = nullptr;
    av_packet_free(&m_packet);
    av_frame_free(&m_frame);
    avcodec_free_context(&m_context);
    m_config.clear();
}

void VideoDecoder::flush()
{
    if (m_context) {
        avcodec_flush_buffers(m_context);
    }
}

bool VideoDecoder::decode(const Scrcpy::VideoEvent &frame, QImage &image)
{
    if (!m_context) {
        return false;
    }
    if (frame.config) {
        m_config = frame.data;
        return false;
    }
    const QByteArray data = std::exchange(m_config, {}) + frame.data;
    if (data.isEmpty() || av_new_packet(m_packet, int(data.size())) < 0) {
        return false;
    }
    std::memcpy(m_packet->data, data.constData(), data.size());
    m_packet->pts = frame.pts;
    const int sent = avcodec_send_packet(m_context, m_packet);
    av_packet_unref(m_packet);
    if (sent < 0) {
        return false;
    }
    bool decoded = false;
    while (avcodec_receive_frame(m_context, m_frame) == 0) {
        const int w = m_frame->width;
        const int h = m_frame->height;
        if (image.width() != w || image.height() != h) {
            // swscale may write past the end of rows that are not a multiple of 16 pixels: aligned stride and padding
            const qsizetype stride = (qsizetype(w) * 4 + 63) & ~63;
            auto *buffer = static_cast<uchar *>(std::calloc(1, stride * h + 128));
            if (!buffer) {
                return false;
            }
            image = QImage(buffer, w, h, stride, QImage::Format_RGB32, std::free, buffer);
        }
        m_sws = sws_getCachedContext(m_sws, w, h, AVPixelFormat(m_frame->format), w, h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (m_sws) {
            uint8_t *dst[4] = {image.bits(), nullptr, nullptr, nullptr};
            const int dstStride[4] = {int(image.bytesPerLine()), 0, 0, 0};
            sws_scale(m_sws, m_frame->data, m_frame->linesize, 0, h, dst, dstStride);
            decoded = true;
        }
        av_frame_unref(m_frame);
    }
    return decoded;
}
