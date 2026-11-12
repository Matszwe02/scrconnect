/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "sink.h"

#ifndef HAVE_PIPEWIRE
std::unique_ptr<Sink> PipeWireSink::open(const QString &, const QString &, QSize)
{
    return nullptr;
}
#else

#include <pipewire/pipewire.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace
{
class PipeWire : public Sink
{
public:
    ~PipeWire() override
    {
        if (!m_loop) {
            return;
        }
        pw_thread_loop_stop(m_loop);
        if (m_stream) {
            pw_stream_destroy(m_stream);
        }
        pw_thread_loop_destroy(m_loop);
    }

    bool open(const QString &name, const QString &description, QSize size)
    {
        pw_init(nullptr, nullptr);
        m_size = size;
        m_loop = pw_thread_loop_new("kdeconnect-webcam", nullptr);
        if (!m_loop) {
            return false;
        }
        m_events.version = PW_VERSION_STREAM_EVENTS;
        m_events.state_changed = [](void *self, pw_stream_state, pw_stream_state state, const char *) {
            static_cast<PipeWire *>(self)->m_streaming = state == PW_STREAM_STATE_STREAMING;
        };
        m_events.process = [](void *self) {
            static_cast<PipeWire *>(self)->process();
        };
        pw_properties *properties = pw_properties_new(PW_KEY_MEDIA_CLASS, "Video/Source", PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_MEDIA_TYPE, "Video",
                                                      PW_KEY_NODE_NAME, name.toUtf8().constData(), PW_KEY_NODE_DESCRIPTION, description.toUtf8().constData(),
                                                      nullptr);
        const int stride = size.width() * 2;
        uint8_t buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_video_info_raw raw{};
        raw.format = SPA_VIDEO_FORMAT_YUY2;
        raw.size = SPA_RECTANGLE(uint32_t(size.width()), uint32_t(size.height()));
        raw.framerate = SPA_FRACTION(30, 1);
        const spa_pod *params[] = {
            spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &raw),
            static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
                                                                    SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(6, 2, 16),
                                                                    SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
                                                                    SPA_PARAM_BUFFERS_size, SPA_POD_Int(stride * size.height()),
                                                                    SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
                                                                    SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)))),
        };
        pw_thread_loop_lock(m_loop);
        m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "kdeconnect-webcam", properties, &m_events, this);
        const bool connected = m_stream
            && pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 2) >= 0;
        pw_thread_loop_unlock(m_loop);
        return connected && pw_thread_loop_start(m_loop) == 0;
    }

    void write(const QByteArray &frame) override
    {
        std::lock_guard lock(m_mutex);
        m_frame = frame;
    }

    bool inUse() override
    {
        return m_streaming;
    }

private:
    void process()
    {
        pw_buffer *buffer = pw_stream_dequeue_buffer(m_stream);
        if (!buffer) {
            return;
        }
        spa_data &data = buffer->buffer->datas[0];
        const uint32_t size = uint32_t(m_size.width() * m_size.height() * 2);
        if (data.data && data.maxsize >= size) {
            std::lock_guard lock(m_mutex);
            if (m_frame.size() == qsizetype(size)) {
                std::memcpy(data.data, m_frame.constData(), size);
            }
            data.chunk->offset = 0;
            data.chunk->size = size;
            data.chunk->stride = m_size.width() * 2;
        }
        pw_stream_queue_buffer(m_stream, buffer);
    }

    QSize m_size;
    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
    pw_stream_events m_events{};
    std::mutex m_mutex;
    QByteArray m_frame;
    std::atomic<bool> m_streaming = false;
};
}

std::unique_ptr<Sink> PipeWireSink::open(const QString &name, const QString &description, QSize size)
{
    auto sink = std::make_unique<PipeWire>();
    return sink->open(name, description, size) ? std::move(sink) : nullptr;
}
#endif
