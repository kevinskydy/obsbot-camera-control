#ifndef UVCPROBE_H
#define UVCPROBE_H

#include <QString>
#include <QVector>
#include <cstdint>

struct UvcExtensionUnit {
    uint8_t unitId;
    QString guidEntity;
    int numControls;
};

struct UvcControlInfo {
    uint8_t unitId;
    uint8_t selector;
    uint16_t length;
    uint8_t infoFlags;
    bool getCur;
    bool setCur;
    bool getMin;
    bool getMax;
    bool getDef;
    QByteArray curValue;
    QByteArray minValue;
    QByteArray maxValue;
    QByteArray defValue;
};

struct UvcProbeReport {
    QString devicePath;
    QVector<UvcExtensionUnit> extensionUnits;
    QVector<UvcControlInfo> controls;
    QString rawDescriptors;
    bool success = false;
    QString error;
};

/**
 * Probe UVC extension units via sysfs and UVCIOC_CTRL_QUERY ioctls.
 * @param videoDevPath  e.g. "/dev/video0"
 * @return probe report with discovered extension units and controls
 */
UvcProbeReport probeUvcExtensionUnits(const QString &videoDevPath);

/**
 * Format a UVC probe report as human-readable text for diagnostics output.
 */
QString formatUvcProbeReport(const UvcProbeReport &report);

#endif // UVCPROBE_H
