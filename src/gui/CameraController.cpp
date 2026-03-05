#include "CameraController.h"
#include <QThread>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <algorithm>

CameraController::CameraController(QObject *parent)
    : QObject(parent)
    , m_connected(false)
    , m_settlingTimer(nullptr)
{
    m_currentState = {};
    m_cachedState = {};
    m_currentState.whiteBalanceKelvin = 5000;
    m_cachedState.whiteBalanceKelvin = 5000;
    m_lastRequestedWhiteBalance = static_cast<int>(Device::DevWhiteBalanceAuto);
    m_whiteBalanceFallbackActive = false;
    m_fallbackWhiteBalanceMode = static_cast<int>(Device::DevWhiteBalanceAuto);

    // Create settling timer
    m_settlingTimer = new QTimer(this);
    m_settlingTimer->setSingleShot(true);

    resetControlRanges();
}

CameraController::~CameraController()
{
}

void CameraController::connectToCamera()
{
    // Setup device detection callback
    auto onDevChanged = [this](std::string dev_sn, bool connected, void *param) {
        if (connected) {
            auto dev_list = Devices::get().getDevList();
            if (!dev_list.empty()) {
                m_device = dev_list.front();
                m_connected = true;
                handleDeviceConnected();
            }
        } else {
            m_connected = false;
            m_cameraInfo.connected = false;
            resetControlRanges();
            emit cameraDisconnected();
        }
    };

    Devices::get().setDevChangedCallback(onDevChanged, nullptr);
    Devices::get().setEnableMdnsScan(false);  // USB only

    // Actively check for existing devices (handles reconnection scenario)
    // The callback only fires on connect/disconnect events, so if the device
    // is already connected (e.g., after window restore), we need to connect directly
    auto dev_list = Devices::get().getDevList();
    if (!dev_list.empty() && !m_connected) {
        m_device = dev_list.front();
        m_connected = true;
        handleDeviceConnected();
    }
}

void CameraController::handleDeviceConnected()
{
    m_cameraInfo.name = QString::fromStdString(m_device->devName());
    m_cameraInfo.serialNumber = QString::fromStdString(m_device->devSn());
    m_cameraInfo.version = QString::fromStdString(m_device->devVersion());
    m_cameraInfo.productType = m_device->productType();
    m_cameraInfo.connected = true;

    runDiagnostics();
    refreshControlRanges();
    emit cameraConnected(m_cameraInfo);
    updateState();
}

void CameraController::disconnectFromCamera()
{
    if (m_connected) {
        // Release our device handle - this allows other apps to access the camera
        m_device.reset();
        m_connected = false;
        m_cameraInfo.connected = false;
        resetControlRanges();

        emit cameraDisconnected();
    }
}

QString CameraController::getVideoDevicePath() const
{
    if (m_connected && m_device) {
        return QString::fromStdString(m_device->videoDevPath());
    }
    return QString();
}

CameraController::CameraState CameraController::getCurrentState()
{
    if (m_connected && !isSettling()) {
        updateState();
    }
    // Return cached state during settling, actual state otherwise
    return isSettling() ? m_cachedState : m_currentState;
}

bool CameraController::hasTiny2Capabilities() const
{
    return isTiny2Family();
}

bool CameraController::enableAutoFraming(bool enabled)
{
    if (!m_connected) return false;

    if (enabled) {
        // Step 1: Set MediaMode to AutoFrame
        if (!executeCommand("Set MediaMode to AutoFrame", [this]() {
            return m_device->cameraSetMediaModeU(Device::MediaModeAutoFrame);
        })) {
            return false;
        }

        // Step 2: Set auto-framing mode after a brief delay (non-blocking)
        QTimer::singleShot(500, [this]() {
            executeCommand("Set AutoFraming mode", [this]() {
                return m_device->cameraSetAutoFramingModeU(Device::AutoFrmSingle, Device::AutoFrmUpperBody);
            });
        });

        // Restore auto focus when auto-framing is enabled
        setFocusAbsolute(0, true);

        m_currentState.autoFramingEnabled = true;
        emit stateChanged(m_currentState);
        return true;  // First command succeeded, second is pending
    } else {
        bool success = executeCommand("Disable AutoFraming", [this]() {
            return m_device->cameraSetMediaModeU(Device::MediaModeNormal);
        });
        if (success) {
            // Switch to manual focus when auto-framing is disabled
            setFocusAbsolute(m_currentState.manualFocusValue, false);

            m_currentState.autoFramingEnabled = false;
            emit stateChanged(m_currentState);
        }
        return success;
    }
}

bool CameraController::setAiMode(int mode, int subMode)
{
    if (!m_connected) return false;

    auto workMode = static_cast<Device::AiWorkModeType>(mode);
    bool success = executeCommand("Set AI Mode", [this, workMode, subMode]() {
        return m_device->cameraSetAiModeU(workMode, subMode);
    });

    if (success) {
        m_currentState.aiMode = mode;
        m_currentState.aiSubMode = subMode;
        m_currentState.autoFramingEnabled = (mode != Device::AiWorkModeNone);
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::setAutoZoom(bool enabled)
{
    if (!m_connected) return false;

    bool success = executeCommand(enabled ? "Enable Auto Zoom" : "Disable Auto Zoom", [this, enabled]() {
        return m_device->aiSetAiAutoZoomR(enabled);
    });

    if (success) {
        m_currentState.autoZoomEnabled = enabled;
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::setTrackSpeed(int speedMode)
{
    if (!m_connected) return false;

    auto speed = static_cast<Device::AiTrackSpeedType>(speedMode);
    bool success = executeCommand("Set Tracking Speed", [this, speed]() {
        return m_device->aiSetTrackSpeedTypeR(speed);
    });

    if (success) {
        m_currentState.trackSpeedMode = speedMode;
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::setAudioAutoGain(bool enabled)
{
    if (!m_connected) return false;

    bool success = executeCommand(enabled ? "Enable Audio Auto Gain" : "Disable Audio Auto Gain", [this, enabled]() {
        return m_device->cameraSetAudioAutoGainU(enabled);
    });

    if (success) {
        m_currentState.audioAutoGainEnabled = enabled;
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::setPanTilt(double pan, double tilt)
{
    if (!m_connected) return false;

    // Clamp values
    pan = qBound(-1.0, pan, 1.0);
    tilt = qBound(-1.0, tilt, 1.0);

    bool success = executeCommand("Set Pan/Tilt", [this, pan, tilt]() {
        return m_device->cameraSetPanTiltAbsolute(pan, tilt);
    });

    if (success) {
        m_currentState.pan = pan;
        m_currentState.tilt = tilt;
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::adjustPan(double delta)
{
    double newPan = m_currentState.pan + delta;
    return setPanTilt(newPan, m_currentState.tilt);
}

bool CameraController::adjustTilt(double delta)
{
    double newTilt = m_currentState.tilt + delta;
    return setPanTilt(m_currentState.pan, newTilt);
}

bool CameraController::setZoom(double zoom)
{
    if (!m_connected) return false;

    // Clamp to valid range (1.0 - 2.0)
    zoom = qBound(1.0, zoom, 2.0);

    bool success = executeCommand("Set Zoom", [this, zoom]() {
        return m_device->cameraSetZoomAbsoluteR(zoom);
    });

    if (success) {
        m_currentState.zoom = zoom;
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::centerView()
{
    return setPanTilt(0.0, 0.0);
}

bool CameraController::setHDR(bool enabled)
{
    if (!m_connected) return false;

    return executeCommand(enabled ? "Enable HDR" : "Disable HDR", [this, enabled]() {
        return m_device->cameraSetWdrR(enabled ? Device::DevWdrModeDol2TO1 : Device::DevWdrModeNone);
    });
}

bool CameraController::setFOV(int fovMode)
{
    if (!m_connected) return false;

    Device::FovType fov;
    switch (fovMode) {
        case 0: fov = Device::FovType86; break;
        case 1: fov = Device::FovType78; break;
        case 2: fov = Device::FovType65; break;
        default: return false;
    }

    return executeCommand("Set FOV", [this, fov]() {
        return m_device->cameraSetFovU(fov);
    });
}

bool CameraController::setFaceAE(bool enabled)
{
    if (!m_connected) return false;

    return executeCommand(enabled ? "Enable Face AE" : "Disable Face AE", [this, enabled]() {
        return m_device->cameraSetFaceAER(enabled);
    });
}

bool CameraController::setFaceFocus(bool enabled)
{
    if (!m_connected) return false;

    return executeCommand(enabled ? "Enable Face Focus" : "Disable Face Focus", [this, enabled]() {
        return m_device->cameraSetFaceFocusR(enabled);
    });
}

bool CameraController::setFocusAbsolute(int position, bool autoFocus)
{
    if (!m_connected) return false;
    position = qBound(0, position, 100);
    bool success = executeCommand("Set Focus", [this, position, autoFocus]() {
        return m_device->cameraSetFocusAbsolute(position, autoFocus);
    });
    if (success) {
        m_currentState.autoFocusEnabled = autoFocus;
        m_currentState.manualFocusValue = position;
        emit stateChanged(m_currentState);
    }
    return success;
}

bool CameraController::setBrightness(int value)
{
    if (!m_connected) return false;

    // Don't send command if in auto mode
    if (m_currentState.brightnessAuto) {
        return true;
    }

    int clamped = clampToRange(value, m_brightnessRange, 0, 255);
    bool success = executeCommand("Set Brightness", [this, clamped]() {
        return m_device->cameraSetImageBrightnessR(clamped);
    });
    if (success) {
        m_currentState.brightness = clamped;
        emit stateChanged(m_currentState);
    }
    return success;
}

bool CameraController::setContrast(int value)
{
    if (!m_connected) return false;

    // Don't send command if in auto mode
    if (m_currentState.contrastAuto) {
        return true;
    }

    int clamped = clampToRange(value, m_contrastRange, 0, 255);
    bool success = executeCommand("Set Contrast", [this, clamped]() {
        return m_device->cameraSetImageContrastR(clamped);
    });
    if (success) {
        m_currentState.contrast = clamped;
        emit stateChanged(m_currentState);
    }
    return success;
}

bool CameraController::setSaturation(int value)
{
    if (!m_connected) return false;

    // Don't send command if in auto mode
    if (m_currentState.saturationAuto) {
        return true;
    }

    int clamped = clampToRange(value, m_saturationRange, 0, 255);
    bool success = executeCommand("Set Saturation", [this, clamped]() {
        return m_device->cameraSetImageSaturationR(clamped);
    });
    if (success) {
        m_currentState.saturation = clamped;
        emit stateChanged(m_currentState);
    }
    return success;
}

bool CameraController::setWhiteBalance(int mode)
{
    if (!m_connected) return false;

    m_lastRequestedWhiteBalance = mode;

    if (mode == static_cast<int>(Device::DevWhiteBalanceManual)) {
        m_whiteBalanceFallbackActive = false;
        m_fallbackWhiteBalanceMode = mode;
        return applyManualWhiteBalance(m_currentState.whiteBalanceKelvin, mode);
    }

    if (mode == static_cast<int>(Device::DevWhiteBalanceAuto)) {
        m_whiteBalanceFallbackActive = false;
        m_fallbackWhiteBalanceMode = mode;
        bool success = executeCommand("Set White Balance", [this]() {
            return m_device->cameraSetWhiteBalanceR(Device::DevWhiteBalanceAuto, 0);
        });
        if (success) {
            m_currentState.whiteBalance = mode;
            if (m_whiteBalanceKelvinRange.valid) {
                m_currentState.whiteBalanceKelvin = clampToRange(m_whiteBalanceKelvinRange.defaultValue, m_whiteBalanceKelvinRange, 2000, 10000);
            }
            emit stateChanged(m_currentState);
        }
        return success;
    }

    auto wbType = static_cast<Device::DevWhiteBalanceType>(mode);
    bool attemptDirect = m_supportedWhiteBalanceTypes.empty() ||
        isWhiteBalanceTypeSupported(mode);
    bool success = false;

    if (attemptDirect) {
        success = executeCommand("Set White Balance", [this, wbType]() {
            return m_device->cameraSetWhiteBalanceR(wbType, 0);
        });

        if (success) {
            Device::DevWhiteBalanceType readType;
            int32_t readParam = 0;
            if (m_device->cameraGetWhiteBalanceR(readType, readParam) == 0 && readType == wbType) {
                m_whiteBalanceFallbackActive = false;
                m_fallbackWhiteBalanceMode = mode;
                m_currentState.whiteBalance = mode;
                if (m_whiteBalanceKelvinRange.valid) {
                    m_currentState.whiteBalanceKelvin = clampToRange(readParam, m_whiteBalanceKelvinRange, 2000, 10000);
                }
                emit stateChanged(m_currentState);
                return true;
            }
        }
    }

    int kelvin = whiteBalancePresetToKelvin(mode);
    if (kelvin > 0 && m_whiteBalanceKelvinRange.valid) {
        m_whiteBalanceFallbackActive = true;
        m_fallbackWhiteBalanceMode = mode;
        return applyManualWhiteBalance(kelvin, mode);
    }

    return success;
}
bool CameraController::setWhiteBalanceManual(int kelvin)
{
    if (!m_connected) return false;

    m_lastRequestedWhiteBalance = static_cast<int>(Device::DevWhiteBalanceManual);
    m_whiteBalanceFallbackActive = false;
    m_fallbackWhiteBalanceMode = static_cast<int>(Device::DevWhiteBalanceManual);
    return applyManualWhiteBalance(kelvin, static_cast<int>(Device::DevWhiteBalanceManual));
}

bool CameraController::executeCommand(const QString &description, std::function<int32_t()> command)
{
    int32_t ret = command();
    if (ret != 0) {
        emit commandFailed(description, ret);
        return false;
    }
    return true;
}

void CameraController::runDiagnostics()
{
    if (!m_device) return;

    m_diagnosticsReport = {};
    m_diagnosticsReport.cameraName = m_cameraInfo.name;
    m_diagnosticsReport.serialNumber = m_cameraInfo.serialNumber;
    m_diagnosticsReport.productType = m_cameraInfo.productType;
    m_diagnosticsReport.timestamp = QDateTime::currentDateTime();

    auto addResult = [this](const QString &category, const QString &name, int32_t ret, const QString &details = {}) {
        DiagnosticResult r;
        r.supported = (ret == 0);
        r.errorCode = ret;
        r.details = details;
        m_diagnosticsReport.categories[category].append({name, r});
    };

    // Helper for UvcParamRange probes
    auto probeRange = [&](const QString &category, const QString &name,
                          int32_t (Device::*getter)(Device::UvcParamRange &)) {
        Device::UvcParamRange range{};
        int32_t ret = (m_device.get()->*getter)(range);
        QString details;
        if (ret == 0) {
            details = QString("min=%1 max=%2 step=%3 default=%4")
                .arg(range.min_).arg(range.max_).arg(range.step_).arg(range.default_);
        }
        addResult(category, name, ret, details);
    };

    // Helper for int32_t value probes
    auto probeInt = [&](const QString &category, const QString &name,
                        int32_t (Device::*getter)(int32_t &)) {
        int32_t val = 0;
        int32_t ret = (m_device.get()->*getter)(val);
        addResult(category, name, ret, ret == 0 ? QString::number(val) : QString());
    };

    // Helper for float value probes
    auto probeFloat = [&](const QString &category, const QString &name,
                          int32_t (Device::*getter)(float &)) {
        float val = 0;
        int32_t ret = (m_device.get()->*getter)(val);
        addResult(category, name, ret, ret == 0 ? QString::number(val, 'f', 2) : QString());
    };

    // === 1. Camera Status (foundation for all other checks) ===
    Device::CameraStatus camStatus{};
    bool hasStatus = false;
    {
        int32_t ret = m_device->cameraGetCameraStatusU(camStatus);
        hasStatus = (ret == 0);
        QString details;
        if (hasStatus) {
            const char *statusNames[] = {"Run", "Sleep", "Privacy"};
            int devSt = camStatus.tiny.dev_status;
            details = QString("device_status=%1 ai_mode=%2 fov=%3 hdr=%4")
                .arg(devSt >= 0 && devSt <= 2 ? statusNames[devSt] : QString::number(devSt))
                .arg(camStatus.tiny.ai_mode)
                .arg(camStatus.tiny.fov)
                .arg(camStatus.tiny.hdr);
        }
        addResult("Camera Status", "camera_status", ret, details);
    }

    // === 2. Video Device & Preview ===
    {
        QString devPath = QString::fromStdString(m_device->videoDevPath());
        bool pathValid = !devPath.isEmpty();
        addResult("Video Device", "video_device_path", pathValid ? 0 : -1,
                  pathValid ? devPath : QString());

        if (pathValid) {
            bool deviceExists = QFile::exists(devPath);
            addResult("Video Device", "preview_capable", deviceExists ? 0 : -1,
                      deviceExists ? "device node exists" : "device node missing");
        }

        // v4l2loopback module check for virtual camera
        bool v4l2loopbackLoaded = QFile::exists("/sys/module/v4l2loopback");
        addResult("Video Device", "virtual_camera_module", v4l2loopbackLoaded ? 0 : -1,
                  v4l2loopbackLoaded ? "v4l2loopback loaded" : "v4l2loopback not loaded");
    }

    // === 3. PTZ (Pan/Tilt/Zoom) ===
    probeRange("PTZ", "zoom_range", &Device::cameraGetRangeZoomAbsoluteR);
    probeFloat("PTZ", "zoom_current", &Device::cameraGetZoomAbsoluteR);
    {
        // Test pan/tilt by commanding center position (0,0) — harmless no-op
        int32_t ret = m_device->cameraSetPanTiltAbsolute(0.0, 0.0);
        addResult("PTZ", "pan_tilt", ret, ret == 0 ? "responsive" : QString());
    }

    // === 4. AI Modes ===
    {
        Device::AiStatus aiStatus{};
        int32_t ret = m_device->aiGetAiStatusR(&aiStatus);
        addResult("AI Modes", "ai_status", ret, ret == 0 ? "OK" : QString());
    }
    {
        // Probe which AI work modes are accepted by setting each, then restoring original
        int originalMode = hasStatus ? camStatus.tiny.ai_mode : 0;
        int originalSubMode = hasStatus ? camStatus.tiny.ai_sub_mode : 0;

        struct { Device::AiWorkModeType mode; const char *name; } aiModes[] = {
            {Device::AiWorkModeNone, "None"},
            {Device::AiWorkModeGroup, "Group"},
            {Device::AiWorkModeHuman, "Human"},
            {Device::AiWorkModeHand, "Hand"},
            {Device::AiWorkModeWhiteBoard, "WhiteBoard"},
            {Device::AiWorkModeDesk, "Desk"},
        };

        QStringList accepted, rejected;
        for (const auto &m : aiModes) {
            int32_t ret = m_device->cameraSetAiModeU(m.mode, 0);
            if (ret == 0) {
                accepted << m.name;
            } else {
                rejected << m.name;
            }
        }

        // Restore original mode
        m_device->cameraSetAiModeU(static_cast<Device::AiWorkModeType>(originalMode), originalSubMode);

        addResult("AI Modes", "accepted_modes", accepted.isEmpty() ? -1 : 0,
                  QString("accepted=[%1]").arg(accepted.join(", ")));
        if (!rejected.isEmpty()) {
            addResult("AI Modes", "rejected_modes", -1,
                      QString("rejected=[%1]").arg(rejected.join(", ")));
        }
    }

    // === 5. HDR & FOV ===
    probeInt("HDR & FOV", "wdr_mode", &Device::cameraGetWdrR);
    {
        std::vector<int32_t> wdrList;
        int32_t ret = m_device->cameraGetWdrListR(wdrList);
        QString details;
        if (ret == 0) {
            QStringList modes;
            for (auto m : wdrList) modes << QString::number(m);
            details = QString("modes=[%1]").arg(modes.join(", "));
        }
        addResult("HDR & FOV", "wdr_list", ret, details);
    }
    if (hasStatus) {
        const char *fovNames[] = {"86 (wide)", "78 (medium)", "65 (narrow)"};
        int fov = camStatus.tiny.fov;
        addResult("HDR & FOV", "fov_current", 0,
                  fov >= 0 && fov <= 2 ? fovNames[fov] : QString::number(fov));
    }

    // === 6. Face AE & Face Focus ===
    {
        bool enabled = false;
        int32_t ret = m_device->cameraGetFaceAER(enabled);
        addResult("Face AE & Focus", "face_ae", ret,
                  ret == 0 ? (enabled ? "enabled" : "disabled") : QString());
    }
    if (hasStatus) {
        addResult("Face AE & Focus", "face_auto_focus", 0,
                  camStatus.tiny.face_auto_focus ? "enabled" : "disabled");
    }
    {
        // Probe face focus setter (set to current value, effectively a no-op)
        bool currentFaceFocus = hasStatus ? camStatus.tiny.face_auto_focus : false;
        int32_t ret = m_device->cameraSetFaceFocusR(currentFaceFocus);
        addResult("Face AE & Focus", "face_focus_settable", ret,
                  ret == 0 ? "responsive" : QString());
    }

    // === 7. Image Controls ===
    probeRange("Image Controls", "brightness_range", &Device::cameraGetRangeImageBrightnessR);
    probeInt("Image Controls", "brightness", &Device::cameraGetImageBrightnessR);
    probeRange("Image Controls", "contrast_range", &Device::cameraGetRangeImageContrastR);
    probeInt("Image Controls", "contrast", &Device::cameraGetImageContrastR);
    probeRange("Image Controls", "saturation_range", &Device::cameraGetRangeImageSaturationR);
    probeInt("Image Controls", "saturation", &Device::cameraGetImageSaturationR);
    probeRange("Image Controls", "sharpness_range", &Device::cameraGetRangeImageSharpR);
    probeInt("Image Controls", "sharpness", &Device::cameraGetImageSharpR);
    probeRange("Image Controls", "hue_range", &Device::cameraGetRangeImageHueR);
    probeInt("Image Controls", "hue", &Device::cameraGetImageHueR);

    // === 8. White Balance ===
    {
        Device::DevWhiteBalanceType wbType;
        int32_t wbParam = 0;
        int32_t ret = m_device->cameraGetWhiteBalanceR(wbType, wbParam);
        QString details;
        if (ret == 0) {
            details = QString("type=%1 param=%2").arg(static_cast<int>(wbType)).arg(wbParam);
        }
        addResult("White Balance", "wb_current", ret, details);
    }
    {
        std::vector<int32_t> wbList;
        int32_t wbMin = 0, wbMax = 0;
        int32_t ret = m_device->cameraGetWhiteBalanceListR(wbList, wbMin, wbMax);
        QString details;
        if (ret == 0) {
            QStringList types;
            for (auto t : wbList) types << QString::number(t);
            details = QString("types=[%1] min=%2 max=%3").arg(types.join(", ")).arg(wbMin).arg(wbMax);
        }
        addResult("White Balance", "wb_list", ret, details);
    }
    probeRange("White Balance", "wb_kelvin_range", &Device::cameraGetRangeWhiteBalanceR);

    // === 9. Exposure & Anti-Flicker ===
    probeInt("Exposure", "exposure_mode", &Device::cameraGetExposureModeR);
    probeInt("Exposure", "anti_flicker", &Device::cameraGetAntiFlickR);
    probeRange("Exposure", "anti_flicker_range", &Device::cameraGetRangeAntiFlickR);

    // === 10. Focus ===
    {
        Device::DevAutoFocusType focusType;
        int32_t ret = m_device->cameraGetAutoFocusModeR(focusType);
        addResult("Focus", "autofocus_mode", ret,
                  ret == 0 ? QString::number(static_cast<int>(focusType)) : QString());
    }
    probeInt("Focus", "focus_position", &Device::cameraGetFocusPosR);

    // === 11. Mirror/Flip ===
    probeInt("Mirror/Flip", "mirror_flip", &Device::cameraGetMirrorFlipR);

    m_diagnosticsReport.completed = true;
    saveDiagnosticsToFile();
    emit diagnosticsCompleted(m_diagnosticsReport);
}

void CameraController::saveDiagnosticsToFile()
{
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + "/obsbot-control";
    QDir().mkpath(configDir);

    QString filename = QString("%1/diagnostics-%2.txt")
        .arg(configDir)
        .arg(m_diagnosticsReport.serialNumber.isEmpty() ? "unknown" : m_diagnosticsReport.serialNumber);

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream out(&file);
    out << "Camera: " << m_diagnosticsReport.cameraName
        << " (Product Type: " << m_diagnosticsReport.productType << ")\n";
    out << "Serial: " << m_diagnosticsReport.serialNumber << "\n";
    out << "Probed: " << m_diagnosticsReport.timestamp.toString("yyyy-MM-dd hh:mm:ss") << "\n\n";

    for (auto it = m_diagnosticsReport.categories.constBegin();
         it != m_diagnosticsReport.categories.constEnd(); ++it) {
        out << "=== " << it.key() << " ===\n";
        for (const auto &probe : it.value()) {
            const auto &name = probe.first;
            const auto &result = probe.second;
            if (result.supported) {
                out << "  [OK]   " << name;
                if (!result.details.isEmpty()) out << ": " << result.details;
                out << "\n";
            } else {
                out << "  [FAIL] " << name << ": error code " << result.errorCode << "\n";
            }
        }
        out << "\n";
    }
}

void CameraController::updateState()
{
    if (!m_connected) return;

    // Don't update from camera during settling period
    if (isSettling()) {
        return;
    }

    auto status = m_device->cameraStatus();

    m_currentState.aiMode = status.tiny.ai_mode;
    m_currentState.aiSubMode = status.tiny.ai_sub_mode;
    m_currentState.zoomRatio = status.tiny.zoom_ratio;
    m_currentState.hdrEnabled = status.tiny.hdr;
    m_currentState.faceAEEnabled = status.tiny.face_ae;
    m_currentState.faceFocusEnabled = status.tiny.face_auto_focus;
    m_currentState.autoFocusEnabled = status.tiny.auto_focus;
    m_currentState.manualFocusValue = status.tiny.manual_focus_value;
    m_currentState.fovMode = status.tiny.fov;
    m_currentState.devStatus = status.tiny.dev_status;
    m_currentState.autoFramingEnabled = (m_currentState.aiMode != Device::AiWorkModeNone);
    m_currentState.trackSpeedMode = status.tiny.ai_tracker_speed;
    m_currentState.audioAutoGainEnabled = status.tiny.audio_auto_gain;

    // Image controls - read current values from camera
    // Note: Preserve auto mode flags - camera doesn't have concept of "auto" for these
    bool preservedBrightnessAuto = m_currentState.brightnessAuto;
    bool preservedContrastAuto = m_currentState.contrastAuto;
    bool preservedSaturationAuto = m_currentState.saturationAuto;

    int32_t brightness, contrast, saturation;
    Device::DevWhiteBalanceType wbType;
    int32_t wbParam;

    if (m_device->cameraGetImageBrightnessR(brightness) == 0) {
        m_currentState.brightness = clampToRange(brightness, m_brightnessRange, 0, 255);
    }
    if (m_device->cameraGetImageContrastR(contrast) == 0) {
        m_currentState.contrast = clampToRange(contrast, m_contrastRange, 0, 255);
    }
    if (m_device->cameraGetImageSaturationR(saturation) == 0) {
        m_currentState.saturation = clampToRange(saturation, m_saturationRange, 0, 255);
    }
    if (m_device->cameraGetWhiteBalanceR(wbType, wbParam) == 0) {
        m_currentState.whiteBalance = static_cast<int>(wbType);
        if (wbType == Device::DevWhiteBalanceManual) {
            m_currentState.whiteBalanceKelvin = clampToRange(wbParam, m_whiteBalanceKelvinRange, 2000, 10000);
        } else if (m_whiteBalanceKelvinRange.valid) {
            m_currentState.whiteBalanceKelvin = clampToRange(m_whiteBalanceKelvinRange.defaultValue, m_whiteBalanceKelvinRange, 2000, 10000);
        }
    }

    // Restore auto mode flags (not stored in camera)
    m_currentState.brightnessAuto = preservedBrightnessAuto;
    m_currentState.contrastAuto = preservedContrastAuto;
    m_currentState.saturationAuto = preservedSaturationAuto;

    if (m_whiteBalanceFallbackActive) {
        m_currentState.whiteBalance = m_fallbackWhiteBalanceMode;
    } else {
        m_lastRequestedWhiteBalance = m_currentState.whiteBalance;
    }

    emit stateChanged(m_currentState);
}

void CameraController::beginSettling(int durationMs)
{
    // Cache the current intended state
    m_cachedState = m_currentState;
    m_settlingTimer->start(durationMs);
}

bool CameraController::loadConfig(std::vector<Config::ValidationError> &errors)
{
    return m_config.load(errors);
}

bool CameraController::saveConfig()
{
    // Update config with current camera state before saving
    saveCurrentStateToConfig();
    return m_config.save();
}

void CameraController::applyConfigToCamera()
{
    if (!m_connected) return;

    auto settings = m_config.getSettings();

    // Initialize auto mode flags from config
    m_currentState.brightnessAuto = settings.brightnessAuto;
    m_currentState.contrastAuto = settings.contrastAuto;
    m_currentState.saturationAuto = settings.saturationAuto;

    // Apply all settings to the camera
    enableAutoFraming(settings.faceTracking);
    setHDR(settings.hdr);
    setFOV(settings.fov);
    setFaceAE(settings.faceAE);
    setFaceFocus(settings.faceFocus);
    setZoom(settings.zoom);
    setPanTilt(settings.pan, settings.tilt);

    if (isTiny2Family()) {
        setAiMode(settings.aiMode, settings.aiSubMode);
        setAutoZoom(settings.autoZoom);
        setTrackSpeed(settings.trackSpeed);
        setAudioAutoGain(settings.audioAutoGain);
    }

    // Image controls
    setBrightness(settings.brightness);
    setContrast(settings.contrast);
    setSaturation(settings.saturation);
    if (settings.whiteBalance == static_cast<int>(Device::DevWhiteBalanceManual)) {
        setWhiteBalanceManual(settings.whiteBalanceKelvin);
    } else {
        setWhiteBalance(settings.whiteBalance);
    }

    emit configLoaded();
}

void CameraController::applyCurrentStateToCamera(const CameraState &uiState)
{
    if (!m_connected) return;

    // Update current state with UI state (including auto mode flags)
    m_currentState.brightnessAuto = uiState.brightnessAuto;
    m_currentState.contrastAuto = uiState.contrastAuto;
    m_currentState.saturationAuto = uiState.saturationAuto;

    // Cache the intended state
    m_cachedState = uiState;

    // Begin settling period - block status updates for 2 seconds
    beginSettling(2000);

    // Apply the current UI state to camera (respects user changes)
    enableAutoFraming(uiState.autoFramingEnabled);
    if (isTiny2Family()) {
        setAiMode(uiState.aiMode, uiState.aiSubMode);
        setAutoZoom(uiState.autoZoomEnabled);
        setTrackSpeed(uiState.trackSpeedMode);
        setAudioAutoGain(uiState.audioAutoGainEnabled);
    }
    setHDR(uiState.hdrEnabled);
    setFOV(uiState.fovMode);
    setFaceAE(uiState.faceAEEnabled);
    setFaceFocus(uiState.faceFocusEnabled);
    setZoom(uiState.zoom);
    setPanTilt(uiState.pan, uiState.tilt);

    // Image controls
    setBrightness(uiState.brightness);
    setContrast(uiState.contrast);
    setSaturation(uiState.saturation);
    if (uiState.whiteBalance == static_cast<int>(Device::DevWhiteBalanceManual)) {
        setWhiteBalanceManual(uiState.whiteBalanceKelvin);
    } else {
        setWhiteBalance(uiState.whiteBalance);
    }
}

void CameraController::saveCurrentStateToConfig()
{
    // Get current settings to preserve app settings (like startMinimized)
    Config::CameraSettings settings = m_config.getSettings();

    // Update only camera-related settings from current state
    settings.faceTracking = m_currentState.autoFramingEnabled;
    settings.hdr = m_currentState.hdrEnabled;
    settings.fov = m_currentState.fovMode;
    settings.faceAE = m_currentState.faceAEEnabled;
    settings.faceFocus = m_currentState.faceFocusEnabled;
    settings.zoom = m_currentState.zoom;
    settings.pan = m_currentState.pan;
    settings.tilt = m_currentState.tilt;
    settings.aiMode = m_currentState.aiMode;
    settings.aiSubMode = m_currentState.aiSubMode;
    settings.autoZoom = m_currentState.autoZoomEnabled;
    settings.trackSpeed = m_currentState.trackSpeedMode;
    settings.audioAutoGain = m_currentState.audioAutoGainEnabled;

    // Image controls
    settings.brightnessAuto = m_currentState.brightnessAuto;
    settings.brightness = m_currentState.brightness;
    settings.contrastAuto = m_currentState.contrastAuto;
    settings.contrast = m_currentState.contrast;
    settings.saturationAuto = m_currentState.saturationAuto;
    settings.saturation = m_currentState.saturation;
    settings.whiteBalance = m_currentState.whiteBalance;
    settings.whiteBalanceKelvin = m_currentState.whiteBalanceKelvin;

    m_config.setSettings(settings);
}

bool CameraController::isTiny2Family() const
{
    return m_cameraInfo.productType == ObsbotProdTiny2 ||
           m_cameraInfo.productType == ObsbotProdTiny2Lite ||
           m_cameraInfo.productType == ObsbotProdTinySE;
}

void CameraController::refreshControlRanges()
{
    if (!m_device) {
        resetControlRanges();
        return;
    }

    auto fetchRange = [this](int32_t (Device::*getter)(Device::UvcParamRange &), ParamRange &target) {
        Device::UvcParamRange sdkRange{};
        if ((m_device.get()->*getter)(sdkRange) == 0) {
            target.min = sdkRange.min_;
            target.max = sdkRange.max_;
            target.step = sdkRange.step_ == 0 ? 1 : sdkRange.step_;
            target.defaultValue = sdkRange.default_;
            target.valid = true;
        } else {
            target = {};
        }
    };

    fetchRange(&Device::cameraGetRangeImageBrightnessR, m_brightnessRange);
    fetchRange(&Device::cameraGetRangeImageContrastR, m_contrastRange);
    fetchRange(&Device::cameraGetRangeImageSaturationR, m_saturationRange);
    fetchRange(&Device::cameraGetRangeWhiteBalanceR, m_whiteBalanceKelvinRange);

    m_supportedWhiteBalanceTypes.clear();
    std::vector<int32_t> wbList;
    int32_t wbMin = 0;
    int32_t wbMax = 0;
    if (m_device->cameraGetWhiteBalanceListR(wbList, wbMin, wbMax) == 0) {
        m_supportedWhiteBalanceTypes.assign(wbList.begin(), wbList.end());
    }

    if (m_whiteBalanceKelvinRange.valid) {
        int clampedCurrent = clampToRange(
            m_currentState.whiteBalanceKelvin == 0 ? m_whiteBalanceKelvinRange.defaultValue : m_currentState.whiteBalanceKelvin,
            m_whiteBalanceKelvinRange, 2000, 10000);
        m_currentState.whiteBalanceKelvin = clampedCurrent;
        m_cachedState.whiteBalanceKelvin = clampToRange(
            m_cachedState.whiteBalanceKelvin == 0 ? m_whiteBalanceKelvinRange.defaultValue : m_cachedState.whiteBalanceKelvin,
            m_whiteBalanceKelvinRange, 2000, 10000);
    }
}

void CameraController::resetControlRanges()
{
    m_brightnessRange = {};
    m_contrastRange = {};
    m_saturationRange = {};
    m_whiteBalanceKelvinRange = {};
    m_supportedWhiteBalanceTypes.clear();
    m_whiteBalanceFallbackActive = false;
    m_fallbackWhiteBalanceMode = static_cast<int>(Device::DevWhiteBalanceAuto);
}

int CameraController::clampToRange(int value, const ParamRange &range, int fallbackMin, int fallbackMax) const
{
    if (range.valid && range.min <= range.max) {
        return std::clamp(value, range.min, range.max);
    }
    return std::clamp(value, fallbackMin, fallbackMax);
}

int CameraController::whiteBalancePresetToKelvin(int mode) const
{
    switch (mode) {
    case static_cast<int>(Device::DevWhiteBalanceDaylight):
        return 5500;
    case static_cast<int>(Device::DevWhiteBalanceFluorescent):
        return 4200;
    case static_cast<int>(Device::DevWhiteBalanceTungsten):
        return 3200;
    case static_cast<int>(Device::DevWhiteBalanceFlash):
        return 6000;
    case static_cast<int>(Device::DevWhiteBalanceFine):
        return 5000;
    case static_cast<int>(Device::DevWhiteBalanceCloudy):
        return 6500;
    case static_cast<int>(Device::DevWhiteBalanceShade):
        return 7500;
    case static_cast<int>(Device::DevWhiteBalanceDayLightFluorescent):
        return 5000;
    case static_cast<int>(Device::DevWhiteBalanceDayWhiteFluorescent):
        return 4500;
    case static_cast<int>(Device::DevWhiteBalanceCoolWhiteFluorescent):
        return 4000;
    case static_cast<int>(Device::DevWhiteBalanceWhiteFluorescent):
        return 3600;
    case static_cast<int>(Device::DevWhiteBalanceWarmWhiteFluorescent):
        return 3000;
    case static_cast<int>(Device::DevWhiteBalanceStandardLightA):
        return 2850;
    case static_cast<int>(Device::DevWhiteBalanceStandardLightB):
        return 3200;
    case static_cast<int>(Device::DevWhiteBalanceStandardLightC):
        return 6500;
    case static_cast<int>(Device::DevWhiteBalance55):
        return 5500;
    case static_cast<int>(Device::DevWhiteBalance65):
        return 6500;
    case static_cast<int>(Device::DevWhiteBalanceD75):
        return 7500;
    case static_cast<int>(Device::DevWhiteBalanceD50):
        return 5000;
    case static_cast<int>(Device::DevWhiteBalanceIsoStudioTungsten):
        return 3200;
    default:
        return 0;
    }
}

bool CameraController::applyManualWhiteBalance(int kelvin, int displayMode)
{
    int clamped = clampToRange(kelvin, m_whiteBalanceKelvinRange, 2000, 10000);
    bool success = executeCommand("Set White Balance (Manual)", [this, clamped]() {
        return m_device->cameraSetWhiteBalanceR(Device::DevWhiteBalanceManual, clamped);
    });

    if (success) {
        m_lastRequestedWhiteBalance = displayMode;
        m_currentState.whiteBalance = displayMode;
        m_currentState.whiteBalanceKelvin = clamped;
        emit stateChanged(m_currentState);
    }

    return success;
}

bool CameraController::isWhiteBalanceTypeSupported(int mode) const
{
    if (mode == static_cast<int>(Device::DevWhiteBalanceAuto) ||
        mode == static_cast<int>(Device::DevWhiteBalanceManual)) {
        return true;
    }

    if (m_supportedWhiteBalanceTypes.empty()) {
        return false;
    }

    return std::find(m_supportedWhiteBalanceTypes.begin(), m_supportedWhiteBalanceTypes.end(), mode)
        != m_supportedWhiteBalanceTypes.end();
}
