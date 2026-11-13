/**
 * SPDX-FileCopyrightText: 2026 KDE Connect contributors
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

// kdeconnect-webcam: every phone camera gets a virtual camera (v4l2loopback and/or PipeWire) that always shows a picture,
// so that apps list it; the phone's camera runs only while an app uses it.

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPainter>
#include <QProcess>
#include <QTextStream>
#include <QTimer>

#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <unistd.h>

#include "../../screenmirror/scrcpyprotocol.h"
#include "../../screenmirror/videodecoder.h"
#include "sink.h"

using namespace Qt::StringLiterals;

namespace
{
// v4l2loopback 0.13+ tells its writer how many apps stream from the device; merely opening it (listing cameras, which
// PipeWire and many apps do) does not count
constexpr quint32 kClientUsageEvent = V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1;

// v4l2loopback before 0.13 takes a program for its writer on VIDIOC_STREAMON only, not on write(). A program that only
// writes leaves the device announcing neither output nor capture (exclusive_caps) when it closes it: nothing can write to
// it, no app lists it, until the module is reloaded. Newer modules refuse this STREAMON (no buffers requested) and change
// nothing; their first write() makes the writer.
void claimWriter(int fd)
{
    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ioctl(fd, VIDIOC_STREAMON, &type);
}

// The module checks a format request against the capabilities it reported last, so ask for them first
bool setFormat(int fd, v4l2_format &format)
{
    v4l2_capability capability{};
    ioctl(fd, VIDIOC_QUERYCAP, &capability);
    return ioctl(fd, VIDIOC_S_FMT, &format) == 0;
}

// Frees a device left as above by an earlier writer, ours or any other program's: becoming its writer and leaving again
bool unstick(const QByteArray &path)
{
    const int fd = ::open(path.constData(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    v4l2_capability capability{};
    const bool stuck = ioctl(fd, VIDIOC_QUERYCAP, &capability) == 0 && (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
        && !(capability.device_caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT));
    if (stuck) {
        claimWriter(fd);
    }
    ::close(fd);
    return stuck;
}

class V4l2Sink : public Sink
{
public:
    explicit V4l2Sink(int fd)
        : m_fd(fd)
    {
        v4l2_event_subscription subscription{};
        subscription.type = kClientUsageEvent;
        subscription.flags = V4L2_EVENT_SUB_FL_SEND_INITIAL;
        m_usageEvents = ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &subscription) == 0;
        if (!m_usageEvents) {
            v4l2_priority priority = V4L2_PRIORITY_BACKGROUND;
            ioctl(fd, VIDIOC_S_PRIORITY, &priority);
        }
    }
    ~V4l2Sink() override
    {
        ::close(m_fd);
    }

    static std::unique_ptr<Sink> open(const QString &label, int width, int height, bool *exists, bool quiet)
    {
        *exists = false;
        const QDir sys(u"/sys/class/video4linux"_s);
        for (const QString &entry : sys.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QFile name(sys.filePath(entry + u"/name"_s));
            if (!name.open(QIODevice::ReadOnly) || QString::fromUtf8(name.readAll()).trimmed() != label) {
                continue;
            }
            *exists = true;
            const QString path = u"/dev/"_s + entry;
            const QByteArray file = QFile::encodeName(path);
            const int fd = ::open(file.constData(), O_WRONLY | O_CLOEXEC);
            v4l2_format format{};
            format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
            format.fmt.pix.width = width;
            format.fmt.pix.height = height;
            format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            format.fmt.pix.field = V4L2_FIELD_NONE;
            format.fmt.pix.bytesperline = width * 2;
            format.fmt.pix.sizeimage = width * height * 2;
            format.fmt.pix.colorspace = V4L2_COLORSPACE_SMPTE170M;
            const v4l2_format wanted = format;
            bool set = fd >= 0 && setFormat(fd, format);
            int error = errno;
            if (!set && fd >= 0 && error == EINVAL && unstick(file)) {
                format = wanted;
                set = setFormat(fd, format);
                error = errno;
                qInfo().noquote() << path << "was left unusable by an earlier program:" << (set ? "freed it" : "could not free it");
            }
            if (set && int(format.fmt.pix.width) == width && int(format.fmt.pix.height) == height && format.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
                claimWriter(fd); // before the constructor lowers the priority: the module would refuse it then
                return std::make_unique<V4l2Sink>(fd);
            }
            if (!quiet) {
                const QString reason = set ? u"it keeps %1x%2 from an earlier use"_s.arg(format.fmt.pix.width).arg(format.fmt.pix.height)
                                           : QString::fromLocal8Bit(strerror(error));
                qWarning().noquote() << "Cannot use" << path << "-" << reason << "- trying again while another program has it open";
            }
            if (fd >= 0) {
                ::close(fd);
            }
        }
        return nullptr;
    }

    void write(const QByteArray &frame) override
    {
        if (::write(m_fd, frame.constData(), frame.size()) < 0) {
        }
    }

    bool inUse() override
    {
        if (m_usageEvents) {
            pollfd pending{m_fd, POLLPRI, 0};
            while (::poll(&pending, 1, 0) > 0 && (pending.revents & POLLPRI)) {
                v4l2_event event{};
                if (ioctl(m_fd, VIDIOC_DQEVENT, &event) != 0) {
                    break;
                }
                if (event.type == kClientUsageEvent) {
                    std::memcpy(&m_streamingApps, event.u.data, sizeof(m_streamingApps));
                }
            }
            return m_streamingApps > 0;
        }
        // Older modules: whether another program has the device open. Every opener gets the default priority, the helper the
        // lowest, so the highest is above the helper's while anything else has it open (a Flatpak sees no other processes)
        v4l2_priority highest = V4L2_PRIORITY_UNSET;
        return ioctl(m_fd, VIDIOC_G_PRIORITY, &highest) == 0 && highest > V4L2_PRIORITY_BACKGROUND;
    }

private:
    int m_fd;
    bool m_usageEvents = false;
    quint32 m_streamingApps = 0;
};

// PipeWire looks at a v4l2loopback device only when it appears, before it is a camera (exclusive_caps): apps that take
// their cameras from PipeWire would not see it, so a PipeWire camera shows the same picture
class BothSinks : public Sink
{
public:
    BothSinks(std::unique_ptr<Sink> v4l2, std::unique_ptr<Sink> pipeWire)
        : m_v4l2(std::move(v4l2))
        , m_pipeWire(std::move(pipeWire))
    {
    }
    void write(const QByteArray &frame) override
    {
        m_v4l2->write(frame);
        m_pipeWire->write(frame);
    }
    bool inUse() override
    {
        return m_v4l2->inUse() | m_pipeWire->inUse();
    }

private:
    std::unique_ptr<Sink> m_v4l2;
    std::unique_ptr<Sink> m_pipeWire;
};

// 0xffRRGGBB pixels to packed YUYV, BT.601 limited range
QByteArray toYuyv(const QImage &image)
{
    QByteArray out(image.width() * image.height() * 2, Qt::Uninitialized);
    auto *o = reinterpret_cast<uchar *>(out.data());
    for (int y = 0; y < image.height(); ++y) {
        const auto *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x + 1 < image.width(); x += 2, o += 4) {
            const QRgb a = row[x];
            const QRgb b = row[x + 1];
            const int r = (qRed(a) + qRed(b)) / 2, g = (qGreen(a) + qGreen(b)) / 2, bl = (qBlue(a) + qBlue(b)) / 2;
            o[0] = uchar(((66 * qRed(a) + 129 * qGreen(a) + 25 * qBlue(a) + 128) >> 8) + 16);
            o[1] = uchar(qBound(0, ((-38 * r - 74 * g + 112 * bl + 128) >> 8) + 128, 255));
            o[2] = uchar(((66 * qRed(b) + 129 * qGreen(b) + 25 * qBlue(b) + 128) >> 8) + 16);
            o[3] = uchar(qBound(0, ((112 * r - 94 * g - 18 * bl + 128) >> 8) + 128, 255));
        }
    }
    return out;
}

class Camera : public QObject
{
    Q_OBJECT
public:
    Camera(const QString &deviceId, const QJsonObject &spec, std::unique_ptr<Sink> sink, QSize frameSize, QObject *parent)
        : QObject(parent)
        , m_plugin(u"org.kde.kdeconnect"_s, u"/modules/kdeconnect/devices/%1/webcam"_s.arg(deviceId), u"org.kde.kdeconnect.device.webcam"_s)
        , m_spec(spec)
        , m_sink(std::move(sink))
        , m_frameSize(frameSize)
    {
        QDBusConnection::sessionBus().connect(m_plugin.service(), m_plugin.path(), m_plugin.interface(), u"statusChanged"_s, this, SLOT(onStatus(QString, QString, QString)));
        connect(&m_socket, &QLocalSocket::readyRead, this, &Camera::readVideo);
        connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
            m_streamId.clear();
        });
        // Black until the video comes: an app that gets no picture may give up
        m_black.resize(qsizetype(frameSize.width()) * frameSize.height() * 2);
        for (qsizetype i = 0; i < m_black.size(); i += 2) {
            m_black[i] = 16;
            m_black[i + 1] = char(128);
        }
        connect(&m_placeholderTimer, &QTimer::timeout, this, [this] {
            if (!m_lastFrame.isValid() || m_lastFrame.elapsed() > 700) {
                m_sink->write(m_black);
            }
        });
        m_placeholderTimer.start(200);
        connect(&m_pollTimer, &QTimer::timeout, this, &Camera::poll);
        m_pollTimer.start(1000);
    }

private Q_SLOTS:
    void onStatus(const QString &streamId, const QString &state, const QString &message)
    {
        if (streamId != m_streamId) {
            return;
        }
        qInfo().noquote() << m_spec.value("device"_L1).toString() << "- the phone:" << state << message;
        m_streamId.clear();
        m_socket.abort();
        if (state != "stopped"_L1 && state != "busy"_L1) {
            m_failure.start();
        }
    }

private:
    bool failedRecently() const
    {
        return m_failure.isValid() && m_failure.elapsed() < 15000;
    }

    // Starts the stream when an app opens the camera twice in a row (listing the cameras is no use), stops it when unused
    void poll()
    {
        const bool inUse = m_sink->inUse();
        if (inUse) {
            m_lastUse.start();
        }
        if (m_streamId.isEmpty() && inUse && m_wasInUse && !failedRecently()) {
            const QDBusReply<QString> id = m_plugin.call(u"startStream"_s,
                                                         m_spec.value("id"_L1).toString(),
                                                         m_spec.value("width"_L1).toInt(),
                                                         m_spec.value("height"_L1).toInt(),
                                                         VideoDecoder::preferredCodec());
            if (id.isValid() && !id.value().isEmpty()) {
                m_streamId = id.value();
                qInfo().noquote() << m_spec.value("device"_L1).toString() << "is in use: the phone starts the camera";
                m_parser = Scrcpy::VideoParser();
                m_decoder.close();
                m_socket.connectToServer(u"kdeconnect-webcam-"_s + m_streamId);
            }
        } else if (!m_streamId.isEmpty() && m_lastUse.elapsed() > 5000) {
            qInfo().noquote() << m_spec.value("device"_L1).toString() << "is no longer in use: the phone stops the camera";
            m_plugin.call(QDBus::NoBlock, u"stopStream"_s, std::exchange(m_streamId, {}));
            m_socket.abort();
        }
        m_wasInUse = inUse;
    }

    void readVideo()
    {
        for (const Scrcpy::VideoEvent &event : m_parser.push(m_socket.readAll())) {
            if (event.type == Scrcpy::VideoEvent::CodecId) {
                m_decoder.open(event.codec);
            } else if (event.type == Scrcpy::VideoEvent::Size) {
                m_decoder.flush();
            } else if (event.type == Scrcpy::VideoEvent::Frame) {
                m_decoder.decode(event, m_picture);
            }
        }
        if (!m_parser.error().isEmpty()) {
            m_socket.abort();
        } else if (!m_picture.isNull()) {
            QImage picture = m_picture.transformed(QTransform().rotate(m_spec.value("rotation"_L1).toInt()));
            if (m_spec.value("mirror"_L1).toBool()) {
                picture = std::move(picture).mirrored(true, false);
            }
            show(picture);
            m_picture = QImage();
            m_lastFrame.start();
        }
    }

    void show(const QImage &picture)
    {
        QImage frame = picture;
        if (picture.size() != m_frameSize) {
            frame = QImage(m_frameSize, QImage::Format_RGB32);
            frame.fill(Qt::black);
            const QImage scaled = picture.scaled(m_frameSize, Qt::KeepAspectRatio, Qt::FastTransformation);
            QPainter(&frame).drawImage((m_frameSize.width() - scaled.width()) / 2, (m_frameSize.height() - scaled.height()) / 2, scaled);
        }
        m_sink->write(toYuyv(frame.convertToFormat(QImage::Format_RGB32)));
    }

    QDBusInterface m_plugin;
    QJsonObject m_spec;
    std::unique_ptr<Sink> m_sink;
    QSize m_frameSize;
    QTimer m_placeholderTimer;
    QTimer m_pollTimer;
    QElapsedTimer m_lastFrame;
    QElapsedTimer m_lastUse;
    QElapsedTimer m_failure;
    QByteArray m_black;
    QString m_streamId;
    bool m_wasInUse = false;
    QLocalSocket m_socket;
    Scrcpy::VideoParser m_parser;
    VideoDecoder m_decoder;
    QImage m_picture;
};

bool inFlatpak()
{
    return QFileInfo::exists(u"/.flatpak-info"_s);
}

const QString kSetupScript = uR"(
PATH=/usr/sbin:/sbin:/usr/bin:/bin:$PATH
printf '%s\n' 'options v4l2loopback devices=2 video_nr=40,41 card_label="Phone front camera,Phone back camera" exclusive_caps=1,1' > /etc/modprobe.d/kdeconnect-webcam.conf
echo v4l2loopback > /etc/modules-load.d/kdeconnect-webcam.conf
modprobe -r v4l2loopback 2>/dev/null
modprobe v4l2loopback && grep -qx 'Phone front camera' /sys/class/video4linux/*/name
)"_s;

int setup()
{
    const QStringList command{u"pkexec"_s, u"sh"_s, u"-c"_s, kSetupScript};
    return inFlatpak() ? QProcess::execute(u"flatpak-spawn"_s, QStringList{u"--host"_s} + command) : QProcess::execute(command.first(), command.mid(1));
}
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOption({u"setup"_s, u"Set up the v4l2loopback devices (as root)"_s});
    parser.addOption({u"setup-script"_s, u"Print the shell script of --setup, to run it as root by other means"_s});
    parser.addOption({u"device"_s, u"Device id"_s, u"id"_s});
    parser.addOption({u"backend"_s, u"auto, v4l2loopback or pipewire"_s, u"name"_s, u"auto"_s});
    parser.addOption({u"cameras"_s, u"JSON: [{id, device, label, width, height, rotation, mirror}]"_s, u"json"_s});
    parser.process(app);
    if (parser.isSet(u"setup"_s)) {
        return setup();
    }
    if (parser.isSet(u"setup-script"_s)) {
        QTextStream(stdout) << kSetupScript;
        return 0;
    }
    prctl(PR_SET_PDEATHSIG, SIGTERM); // never outlive the daemon: the virtual cameras would stay busy

    const QString backend = parser.value(u"backend"_s);
    const QString deviceId = parser.value(u"device"_s);
    enum Attached { Done, NoSink, Refused };
    int pending = 0;
    for (const QJsonValue &value : QJsonDocument::fromJson(parser.value(u"cameras"_s).toUtf8()).array()) {
        const QJsonObject spec = value.toObject();
        QSize size(spec.value("width"_L1).toInt() & ~1, spec.value("height"_L1).toInt() & ~1);
        if (spec.value("rotation"_L1).toInt() % 180) {
            size.transpose();
        }
        if (size.width() < 160 || size.height() < 120) {
            continue;
        }
        auto attach = [=, &app](bool quiet) {
            std::unique_ptr<Sink> sink;
            if (backend != "pipewire"_L1) {
                bool exists = false;
                sink = V4l2Sink::open(spec.value("device"_L1).toString(), size.width(), size.height(), &exists, quiet);
                if (!sink && exists) {
                    return Refused;
                }
            }
            if (sink || backend != "v4l2loopback"_L1) {
                auto pipeWire = PipeWireSink::open(u"kdeconnect-webcam-%1-%2"_s.arg(deviceId, spec.value("id"_L1).toString()), spec.value("label"_L1).toString(), size);
                if (sink && pipeWire) {
                    sink = std::make_unique<BothSinks>(std::move(sink), std::move(pipeWire));
                } else if (!sink) {
                    sink = std::move(pipeWire);
                }
            }
            if (!sink) {
                return NoSink;
            }
            new Camera(deviceId, spec, std::move(sink), size, &app);
            return Done;
        };
        const Attached attached = attach(false);
        if (attached == Refused) {
            auto *retry = new QTimer(&app);
            QObject::connect(retry, &QTimer::timeout, retry, [retry, attach] {
                if (attach(true) != Refused) {
                    retry->deleteLater();
                }
            });
            retry->start(3000);
        }
        pending += attached != NoSink;
    }
    return pending ? app.exec() : 0;
}

#include "main.moc"
