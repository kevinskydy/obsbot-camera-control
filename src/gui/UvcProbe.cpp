#include "UvcProbe.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uvcvideo.h>
#include <linux/usb/video.h>
#include <cstring>

// Resolve the real USB device path from /dev/videoN via sysfs
static QString resolveUsbDevicePath(const QString &videoDevPath)
{
    // /dev/video0 -> video0
    QString devName = QFileInfo(videoDevPath).fileName();
    // Follow sysfs link: /sys/class/video4linux/video0/device -> ../../usb...
    QString sysPath = QString("/sys/class/video4linux/%1/device").arg(devName);
    QFileInfo info(sysPath);
    if (!info.exists() || !info.isSymLink())
        return {};
    return info.canonicalFilePath();
}

// Parse extension unit GUIDs from USB descriptors via sysfs
static QVector<UvcExtensionUnit> parseExtensionUnits(const QString &videoDevPath)
{
    QVector<UvcExtensionUnit> units;

    QString devName = QFileInfo(videoDevPath).fileName();
    QString sysDir = QString("/sys/class/video4linux/%1/device").arg(devName);
    QFileInfo info(sysDir);
    if (!info.exists())
        return units;

    QString usbDevPath = info.canonicalFilePath();

    // Walk up to find the USB device root that has descriptors
    // The descriptors file is at the USB device level (e.g., /sys/bus/usb/devices/1-2/)
    QString descPath;
    QString current = usbDevPath;
    for (int i = 0; i < 10; ++i) {
        QString candidate = current + "/descriptors";
        if (QFile::exists(candidate)) {
            descPath = candidate;
            break;
        }
        current = QFileInfo(current).path();
        if (current == "/" || current.isEmpty())
            break;
    }

    if (descPath.isEmpty())
        return units;

    QFile descFile(descPath);
    if (!descFile.open(QIODevice::ReadOnly))
        return units;

    QByteArray descriptors = descFile.readAll();
    const uint8_t *data = reinterpret_cast<const uint8_t *>(descriptors.constData());
    int len = descriptors.size();

    // Parse USB descriptors looking for UVC Extension Unit descriptors
    // UVC Extension Unit descriptor:
    //   bLength, bDescriptorType=0x24 (CS_INTERFACE), bDescriptorSubtype=0x06 (VC_EXTENSION_UNIT)
    //   bUnitID, guidExtensionCode[16], bNumControls, ...
    int pos = 0;
    while (pos < len - 2) {
        uint8_t bLength = data[pos];
        if (bLength < 2 || pos + bLength > len)
            break;

        uint8_t bDescriptorType = data[pos + 1];

        // CS_INTERFACE = 0x24, VC_EXTENSION_UNIT subtype = 0x06
        if (bDescriptorType == 0x24 && bLength >= 24) {
            uint8_t bDescriptorSubtype = data[pos + 2];
            if (bDescriptorSubtype == 0x06) {
                UvcExtensionUnit xu;
                xu.unitId = data[pos + 3];

                // GUID is at offset 4, 16 bytes, in mixed-endian USB format
                const uint8_t *g = &data[pos + 4];
                xu.guidEntity = QString("{%1%2%3%4-%5%6-%7%8-%9%10-%11%12%13%14%15%16}")
                    .arg(g[3], 2, 16, QChar('0'))
                    .arg(g[2], 2, 16, QChar('0'))
                    .arg(g[1], 2, 16, QChar('0'))
                    .arg(g[0], 2, 16, QChar('0'))
                    .arg(g[5], 2, 16, QChar('0'))
                    .arg(g[4], 2, 16, QChar('0'))
                    .arg(g[7], 2, 16, QChar('0'))
                    .arg(g[6], 2, 16, QChar('0'))
                    .arg(g[8], 2, 16, QChar('0'))
                    .arg(g[9], 2, 16, QChar('0'))
                    .arg(g[10], 2, 16, QChar('0'))
                    .arg(g[11], 2, 16, QChar('0'))
                    .arg(g[12], 2, 16, QChar('0'))
                    .arg(g[13], 2, 16, QChar('0'))
                    .arg(g[14], 2, 16, QChar('0'))
                    .arg(g[15], 2, 16, QChar('0'));

                xu.numControls = data[pos + 20];
                units.append(xu);
            }
        }

        pos += bLength;
    }

    return units;
}

static QByteArray xuQuery(int fd, uint8_t unit, uint8_t selector, uint8_t query, uint16_t size)
{
    QByteArray buf(size, 0);
    struct uvc_xu_control_query xq;
    memset(&xq, 0, sizeof(xq));
    xq.unit = unit;
    xq.selector = selector;
    xq.query = query;
    xq.size = size;
    xq.data = reinterpret_cast<uint8_t *>(buf.data());

    if (ioctl(fd, UVCIOC_CTRL_QUERY, &xq) < 0)
        return {};
    return buf;
}

UvcProbeReport probeUvcExtensionUnits(const QString &videoDevPath)
{
    UvcProbeReport report;
    report.devicePath = videoDevPath;

    if (videoDevPath.isEmpty()) {
        report.error = "No video device path";
        return report;
    }

    // Step 1: Parse extension units from USB descriptors
    report.extensionUnits = parseExtensionUnits(videoDevPath);

    // Step 2: Open the video device for ioctl probing
    int fd = open(videoDevPath.toUtf8().constData(), O_RDWR);
    if (fd < 0) {
        report.error = QString("Cannot open %1: %2").arg(videoDevPath, strerror(errno));
        // We may still have descriptor info even if we can't open the device
        report.success = !report.extensionUnits.isEmpty();
        return report;
    }

    // Step 3: For each extension unit, probe selectors 1..N
    for (const auto &xu : report.extensionUnits) {
        // Probe selectors 1 through numControls (and a few beyond, since numControls
        // in the descriptor may not reflect all supported selectors)
        int maxSelector = qMax(xu.numControls, 32);
        for (int sel = 1; sel <= maxSelector; ++sel) {
            // First try GET_LEN to determine control size
            uint8_t lenBuf[2] = {0};
            struct uvc_xu_control_query lenQuery;
            memset(&lenQuery, 0, sizeof(lenQuery));
            lenQuery.unit = xu.unitId;
            lenQuery.selector = static_cast<uint8_t>(sel);
            lenQuery.query = UVC_GET_LEN;
            lenQuery.size = 2;
            lenQuery.data = lenBuf;

            uint16_t ctrlLen = 0;
            if (ioctl(fd, UVCIOC_CTRL_QUERY, &lenQuery) == 0) {
                ctrlLen = lenBuf[0] | (lenBuf[1] << 8);
            }

            // Try GET_INFO to determine capabilities
            uint8_t infoBuf[1] = {0};
            struct uvc_xu_control_query infoQuery;
            memset(&infoQuery, 0, sizeof(infoQuery));
            infoQuery.unit = xu.unitId;
            infoQuery.selector = static_cast<uint8_t>(sel);
            infoQuery.query = UVC_GET_INFO;
            infoQuery.size = 1;
            infoQuery.data = infoBuf;

            bool hasInfo = (ioctl(fd, UVCIOC_CTRL_QUERY, &infoQuery) == 0);

            // If neither GET_LEN nor GET_INFO responded, this selector doesn't exist
            if (ctrlLen == 0 && !hasInfo)
                continue;

            if (ctrlLen == 0)
                ctrlLen = 1; // Fallback: try 1-byte reads

            // Cap at reasonable size for probing
            if (ctrlLen > 256)
                ctrlLen = 256;

            UvcControlInfo ctrl;
            ctrl.unitId = xu.unitId;
            ctrl.selector = static_cast<uint8_t>(sel);
            ctrl.length = ctrlLen;
            ctrl.infoFlags = hasInfo ? infoBuf[0] : 0;
            ctrl.getCur = hasInfo && (infoBuf[0] & UVC_CTRL_FLAG_GET_CUR);
            ctrl.setCur = hasInfo && (infoBuf[0] & UVC_CTRL_FLAG_SET_CUR);
            ctrl.getMin = hasInfo && (infoBuf[0] & UVC_CTRL_FLAG_GET_MIN);
            ctrl.getMax = hasInfo && (infoBuf[0] & UVC_CTRL_FLAG_GET_MAX);
            ctrl.getDef = hasInfo && (infoBuf[0] & UVC_CTRL_FLAG_GET_DEF);

            // Try reading values
            if (ctrl.getCur || !hasInfo) // If no info, try anyway
                ctrl.curValue = xuQuery(fd, xu.unitId, ctrl.selector, UVC_GET_CUR, ctrlLen);
            if (ctrl.getMin)
                ctrl.minValue = xuQuery(fd, xu.unitId, ctrl.selector, UVC_GET_MIN, ctrlLen);
            if (ctrl.getMax)
                ctrl.maxValue = xuQuery(fd, xu.unitId, ctrl.selector, UVC_GET_MAX, ctrlLen);
            if (ctrl.getDef)
                ctrl.defValue = xuQuery(fd, xu.unitId, ctrl.selector, UVC_GET_DEF, ctrlLen);

            report.controls.append(ctrl);
        }
    }

    close(fd);
    report.success = true;
    return report;
}

static QString hexDump(const QByteArray &data)
{
    if (data.isEmpty())
        return "(empty)";
    QStringList parts;
    for (int i = 0; i < data.size() && i < 32; ++i)
        parts << QString("%1").arg(static_cast<uint8_t>(data[i]), 2, 16, QChar('0'));
    QString result = parts.join(" ");
    if (data.size() > 32)
        result += QString(" ... (%1 bytes total)").arg(data.size());
    return result;
}

QString formatUvcProbeReport(const UvcProbeReport &report)
{
    QString text;
    QTextStream out(&text);

    out << "=== UVC Extension Unit Probe ===\n";
    out << "Device: " << report.devicePath << "\n";

    if (!report.error.isEmpty()) {
        out << "Error: " << report.error << "\n";
    }

    if (report.extensionUnits.isEmpty()) {
        out << "No extension units found in USB descriptors.\n";
    } else {
        out << "\nExtension Units from USB descriptors:\n";
        for (const auto &xu : report.extensionUnits) {
            out << "  Unit " << xu.unitId
                << " GUID=" << xu.guidEntity
                << " controls=" << xu.numControls << "\n";
        }
    }

    if (report.controls.isEmpty()) {
        out << "\nNo responsive controls found via ioctl probing.\n";
    } else {
        out << "\nDiscovered Controls (" << report.controls.size() << " total):\n";

        uint8_t lastUnit = 0;
        for (const auto &ctrl : report.controls) {
            if (ctrl.unitId != lastUnit) {
                out << "\n  --- Unit " << ctrl.unitId << " ---\n";
                lastUnit = ctrl.unitId;
            }

            out << "  Selector " << QString("%1").arg(ctrl.selector, 2)
                << " | len=" << QString("%1").arg(ctrl.length, 3)
                << " | flags=0x" << QString("%1").arg(ctrl.infoFlags, 2, 16, QChar('0'))
                << " [" << (ctrl.getCur ? "GET" : "   ")
                << " " << (ctrl.setCur ? "SET" : "   ")
                << " " << (ctrl.getMin ? "MIN" : "   ")
                << " " << (ctrl.getMax ? "MAX" : "   ")
                << " " << (ctrl.getDef ? "DEF" : "   ")
                << "]\n";

            if (!ctrl.curValue.isEmpty())
                out << "    CUR: " << hexDump(ctrl.curValue) << "\n";
            if (!ctrl.minValue.isEmpty())
                out << "    MIN: " << hexDump(ctrl.minValue) << "\n";
            if (!ctrl.maxValue.isEmpty())
                out << "    MAX: " << hexDump(ctrl.maxValue) << "\n";
            if (!ctrl.defValue.isEmpty())
                out << "    DEF: " << hexDump(ctrl.defValue) << "\n";
        }
    }

    // Summary
    int sdkMapped = 0; // We can't determine this without known GUIDs, but it's useful context
    out << "\nSummary: " << report.extensionUnits.size() << " extension unit(s), "
        << report.controls.size() << " responsive control(s)\n";

    return text;
}
