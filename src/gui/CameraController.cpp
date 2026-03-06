#include "CameraController.h"
#include "UvcProbe.h"
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
            if (!m_basicDiagRunning.load()) {
                m_device.reset();
            }
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

    emit cameraDetected(m_cameraInfo);

    if (m_basicDiagRunning.load()) return;
    m_basicDiagRunning.store(true);

    auto *thread = QThread::create([this]() {
        runDiagnostics();
        refreshControlRanges();

        m_basicDiagRunning.store(false);

        if (!m_connected) {
            // Disconnected during probing - clean up deferred device handle
            QMetaObject::invokeMethod(this, [this]() {
                m_device.reset();
            }, Qt::QueuedConnection);
            return;
        }

        QMetaObject::invokeMethod(this, [this]() {
            emit cameraConnected(m_cameraInfo);
            updateState();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CameraController::disconnectFromCamera()
{
    if (m_connected) {
        m_connected = false;
        m_cameraInfo.connected = false;
        // Defer device handle release if basic diagnostics worker is still using it
        if (!m_basicDiagRunning.load()) {
            m_device.reset();
        }
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
    return m_capabilities.aiModes;
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
    // Basic diagnostics — read-only queries only, runs automatically on camera connect.
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

    auto probeInt = [&](const QString &category, const QString &name,
                        int32_t (Device::*getter)(int32_t &)) {
        int32_t val = 0;
        int32_t ret = (m_device.get()->*getter)(val);
        addResult(category, name, ret, ret == 0 ? QString::number(val) : QString());
    };

    auto probeFloat = [&](const QString &category, const QString &name,
                          int32_t (Device::*getter)(float &)) {
        float val = 0;
        int32_t ret = (m_device.get()->*getter)(val);
        addResult(category, name, ret, ret == 0 ? QString::number(val, 'f', 2) : QString());
    };

    // === 1. Camera Status ===
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

    // === 2. Video Device ===
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

        bool v4l2loopbackLoaded = QFile::exists("/sys/module/v4l2loopback");
        addResult("Video Device", "virtual_camera_module", v4l2loopbackLoaded ? 0 : -1,
                  v4l2loopbackLoaded ? "v4l2loopback loaded" : "v4l2loopback not loaded");
    }

    // === 3. PTZ (read-only) ===
    probeRange("PTZ", "zoom_range", &Device::cameraGetRangeZoomAbsoluteR);
    probeFloat("PTZ", "zoom_current", &Device::cameraGetZoomAbsoluteR);

    // === 4. AI Status (read-only) ===
    {
        Device::AiStatus aiStatus{};
        int32_t ret = m_device->aiGetAiStatusR(&aiStatus);
        addResult("AI Modes", "ai_status", ret, ret == 0 ? "OK" : QString());
    }
    if (hasStatus) {
        addResult("AI Modes", "current_mode", 0,
                  QString("ai_mode=%1 sub_mode=%2").arg(camStatus.tiny.ai_mode).arg(camStatus.tiny.ai_sub_mode));
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

    // === 6. Face AE (read-only) ===
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

    // === 9. Exposure & Anti-Flicker (basic reads only) ===
    probeInt("Exposure", "exposure_mode", &Device::cameraGetExposureModeR);
    probeInt("Exposure", "anti_flicker", &Device::cameraGetAntiFlickR);
    probeRange("Exposure", "anti_flicker_range", &Device::cameraGetRangeAntiFlickR);

    // === 10. Focus (basic reads only) ===
    {
        Device::DevAutoFocusType focusType;
        int32_t ret = m_device->cameraGetAutoFocusModeR(focusType);
        addResult("Focus", "autofocus_mode", ret,
                  ret == 0 ? QString::number(static_cast<int>(focusType)) : QString());
    }
    probeInt("Focus", "focus_position", &Device::cameraGetFocusPosR);

    // === 11. Mirror/Flip ===
    probeInt("Mirror/Flip", "mirror_flip", &Device::cameraGetMirrorFlipR);

    // === 12. Classification probes (quick read-only checks to identify device family) ===
    {
        Device::AiGimbalStateInfo gimInfo{};
        int32_t ret = m_device->aiGetGimbalStateR(&gimInfo);
        addResult("Gimbal", "gimbal_state", ret, ret == 0 ? "responsive" : QString());
    }
    {
        Device::DevDataArray ids{};
        int32_t ret = m_device->aiGetZonePresetListR(&ids);
        addResult("Zone Tracking", "zone_preset_list", ret, ret == 0 ? "responsive" : QString());
    }
    {
        Device::RtspOrNdiEnabled ndiRtsp;
        int32_t ret = m_device->cameraGetSelectNdiOrRtspR(ndiRtsp);
        addResult("Streaming", "ndi_rtsp_selection", ret,
                  ret == 0 ? QString::number(static_cast<int>(ndiRtsp)) : QString());
    }

    populateCapabilities();
    classifyFromDiagnostics();

    m_diagnosticsReport.completed = true;
    saveDiagnosticsToFile();
    QMetaObject::invokeMethod(this, [this]() {
        emit diagnosticsCompleted(m_diagnosticsReport);
    }, Qt::QueuedConnection);
}

void CameraController::populateCapabilities()
{
    m_capabilities = {};

    auto ok = [this](const QString &category, const QString &name) -> bool {
        auto it = m_diagnosticsReport.categories.find(category);
        if (it == m_diagnosticsReport.categories.end()) return false;
        for (const auto &probe : it.value()) {
            if (probe.first == name) return probe.second.supported;
        }
        return false;
    };

    m_capabilities.aiModes = ok("Zone Tracking", "zone_preset_list");
    m_capabilities.aiStatus = ok("AI Modes", "ai_status");
    m_capabilities.panTilt = ok("PTZ", "pan_tilt");
    m_capabilities.zoom = ok("PTZ", "zoom_range");
    m_capabilities.hdrGet = ok("HDR & FOV", "wdr_mode");
    m_capabilities.fov = ok("HDR & FOV", "fov_current");
    m_capabilities.faceAE = ok("Face AE & Focus", "face_ae");
    m_capabilities.faceFocus = ok("Face AE & Focus", "face_focus_settable");
    m_capabilities.imageControls = ok("Image Controls", "brightness_range");
    m_capabilities.whiteBalance = ok("White Balance", "wb_current");
    m_capabilities.autoZoom = m_capabilities.aiModes;
    m_capabilities.trackSpeed = m_capabilities.aiModes;
    m_capabilities.audioAutoGain = m_capabilities.aiModes;
    m_capabilities.antiFlicker = ok("Exposure", "anti_flicker");
    m_capabilities.autofocus = ok("Focus", "autofocus_mode");
    m_capabilities.videoDevice = ok("Video Device", "video_device_path");
    m_capabilities.virtualCamera = ok("Video Device", "virtual_camera_module");
    // These are populated by extended diagnostics (if run)
    m_capabilities.gimbal = ok("Gimbal", "gimbal_state");
    m_capabilities.gestureControl = ok("Gesture Control", "gesture_main");
    m_capabilities.zoneTracking = ok("Zone Tracking", "zone_preset_list");
    m_capabilities.recording = ok("Recording", "main_encoder_format");
    m_capabilities.streaming = ok("Streaming", "ndi_rtsp_selection");
    m_capabilities.aiControlParams = ok("AI Control Params", "human_motion");
    m_capabilities.uvcExtensionUnits = ok("UVC Extension Units", "probe_status");
}

void CameraController::classifyFromDiagnostics()
{
    auto addResult = [this](const QString &category, const QString &name, int32_t ret, const QString &details = {}) {
        DiagnosticResult r;
        r.supported = (ret == 0);
        r.errorCode = ret;
        r.details = details;
        m_diagnosticsReport.categories[category].append({name, r});
    };

    // Map product type enum to display name
    static const QMap<int, QString> productNames = {
        {ObsbotProdTiny, "Tiny"}, {ObsbotProdTiny4k, "Tiny 4K"},
        {ObsbotProdTiny2, "Tiny 2"}, {ObsbotProdTiny2Lite, "Tiny 2 Lite"},
        {ObsbotProdTinySE, "Tiny SE"}, {ObsbotProdMeet, "Meet"},
        {ObsbotProdMeet4k, "Meet 4K"}, {ObsbotProdMeet2, "Meet 2"},
        {ObsbotProdMeetSE, "Meet SE"}, {ObsbotProdTailAir, "Tail Air"},
        {ObsbotProdTail2, "Tail 2"}, {ObsbotProdTail2S, "Tail 2S"},
        {ObsbotProdMe, "Me"}, {ObsbotProdHDMIBox, "HDMI Box"},
        {ObsbotProdNDIBox, "NDI Box"},
    };
    QString sdkName = productNames.value(m_diagnosticsReport.productType, "Unknown");

    // Infer family from diagnostic probe results
    QString inferredFamily;
    if (m_capabilities.zoneTracking) {
        inferredFamily = "Tiny 2 Series";
    } else if (m_capabilities.streaming) {
        inferredFamily = "Tail Air";
    } else if (m_capabilities.gimbal && m_capabilities.aiStatus) {
        inferredFamily = "Tail Series";
    } else if (m_capabilities.panTilt && !m_capabilities.aiStatus) {
        inferredFamily = "Meet Series";
    } else if (m_capabilities.aiStatus && m_capabilities.zoom) {
        inferredFamily = "Tiny / Tiny 4K";
    } else {
        inferredFamily = "Unknown";
    }

    addResult("  Classification", "sdk_product_type", 0,
              QString("%1 (enum %2)").arg(sdkName).arg(m_diagnosticsReport.productType));
    addResult("  Classification", "inferred_family", 0, inferredFamily);

    bool match = inferredFamily == "Unknown"
        || sdkName.contains(inferredFamily.split(" ").first(), Qt::CaseInsensitive)
        || inferredFamily.contains(sdkName.split(" ").first(), Qt::CaseInsensitive);
    addResult("  Classification", "match", match ? 0 : -1,
              match ? "SDK type matches diagnostics" : "MISMATCH - diagnostics suggest " + inferredFamily);
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

    // Append full UVC probe report
    if (!m_uvcProbeText.isEmpty()) {
        out << "\n" << m_uvcProbeText << "\n";
    }
}

void CameraController::runExtendedDiagnostics()
{
    if (!m_device || m_extendedDiagRunning.load()) return;

    m_extendedDiagRunning.store(true);
    m_extendedDiagCancelled.store(false);

    auto *thread = QThread::create([this]() {
        runExtendedDiagnosticsWorker();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void CameraController::cancelExtendedDiagnostics()
{
    m_extendedDiagCancelled.store(true);
}

void CameraController::runExtendedDiagnosticsWorker()
{
    auto addResult = [this](const QString &category, const QString &name, int32_t ret, const QString &details = {}) {
        DiagnosticResult r;
        r.supported = (ret == 0);
        r.errorCode = ret;
        r.details = details;
        m_diagnosticsReport.categories[category].append({name, r});
    };

    auto cancelled = [this]() { return m_extendedDiagCancelled.load(); };

    auto probeInt = [&](const QString &category, const QString &name,
                        int32_t (Device::*getter)(int32_t &)) {
        if (cancelled()) return;
        int32_t val = 0;
        int32_t ret = (m_device.get()->*getter)(val);
        addResult(category, name, ret, ret == 0 ? QString::number(val) : QString());
    };

    auto probeRange = [&](const QString &category, const QString &name,
                          int32_t (Device::*getter)(Device::UvcParamRange &)) {
        if (cancelled()) return;
        Device::UvcParamRange range{};
        int32_t ret = (m_device.get()->*getter)(range);
        QString details;
        if (ret == 0) {
            details = QString("min=%1 max=%2 step=%3 default=%4")
                .arg(range.min_).arg(range.max_).arg(range.step_).arg(range.default_);
        }
        addResult(category, name, ret, details);
    };

    // ========================================================================
    // Extended Exposure probes (beyond basic)
    // ========================================================================
    if (!cancelled()) {
        int32_t shutterTime = 0;
        bool autoEnabled = false;
        int32_t ret = m_device->cameraGetExposureAbsolute(shutterTime, autoEnabled);
        addResult("Exposure", "exposure_absolute", ret,
                  ret == 0 ? QString("shutter=%1 auto=%2").arg(shutterTime).arg(autoEnabled) : QString());
    }
    probeRange("Exposure", "exposure_absolute_range", &Device::cameraGetRangeExposureAbsolute);
    if (!cancelled()) {
        int32_t evBias = 0;
        int32_t ret = m_device->cameraGetPAEEvBiasR(evBias);
        addResult("Exposure", "pae_ev_bias", ret, ret == 0 ? QString::number(evBias) : QString());
    }
    probeRange("Exposure", "pae_ev_bias_range", &Device::cameraGetRangePAEEvBiasR);
    probeInt("Exposure", "sae_shutter", &Device::cameraGetSAEShutterR);
    if (!cancelled()) {
        Device::DevAEEvBiasType evBias;
        int32_t ret = m_device->cameraGetAAEEvBiasR(evBias);
        addResult("Exposure", "aae_ev_bias", ret,
                  ret == 0 ? QString::number(static_cast<int>(evBias)) : QString());
    }
    probeInt("Exposure", "mae_shutter", &Device::cameraGetMAEShutterR);
    probeInt("Exposure", "mae_iso", &Device::cameraGetMAEIsoR);
    probeRange("Exposure", "mae_iso_range", &Device::cameraGetRangeMAEIsoR);
    if (!cancelled()) {
        uint32_t minIso = 0, maxIso = 0;
        int32_t ret = m_device->cameraGetISOLimitR(minIso, maxIso);
        addResult("Exposure", "iso_limit", ret,
                  ret == 0 ? QString("min=%1 max=%2").arg(minIso).arg(maxIso) : QString());
    }
    if (!cancelled()) {
        bool locked = false;
        int32_t ret = m_device->cameraGetAELockR(locked);
        addResult("Exposure", "ae_lock", ret,
                  ret == 0 ? (locked ? "locked" : "unlocked") : QString());
    }

    // ========================================================================
    // Extended Focus probes
    // ========================================================================
    if (!cancelled()) {
        int32_t focus = 0;
        bool autoFocus = false;
        int32_t ret = m_device->cameraGetFocusAbsolute(focus, autoFocus);
        addResult("Focus", "focus_absolute", ret,
                  ret == 0 ? QString("pos=%1 auto=%2").arg(focus).arg(autoFocus) : QString());
    }
    probeRange("Focus", "focus_absolute_range", &Device::cameraGetRangeFocusAbsolute);
    if (!cancelled()) {
        Device::DevAFCType afcType;
        int32_t ret = m_device->cameraGetAFCTrackModeR(afcType);
        addResult("Focus", "afc_track_mode", ret,
                  ret == 0 ? QString::number(static_cast<int>(afcType)) : QString());
    }

    // ========================================================================
    // Rotation
    // ========================================================================
    if (!cancelled()) {
        int32_t rotation = 0;
        int32_t ret = m_device->cameraGetRotationDegree(rotation);
        addResult("Mirror/Flip", "rotation_degree", ret,
                  ret == 0 ? QString::number(rotation) : QString());
    }

    // ========================================================================
    // Gimbal
    // ========================================================================
    if (!cancelled()) {
        Device::AiGimbalStateInfo gimInfo{};
        int32_t ret = m_device->aiGetGimbalStateR(&gimInfo);
        addResult("Gimbal", "gimbal_state", ret, ret == 0 ? "responsive" : QString());
    }
    if (!cancelled()) {
        Device::PresetPosInfo bootPos{};
        int32_t ret = m_device->aiGetGimbalBootPosR(&bootPos);
        addResult("Gimbal", "gimbal_boot_pos", ret, ret == 0 ? "responsive" : QString());
    }
    if (!cancelled()) {
        Device::DevDataArray ids{};
        int32_t ret = m_device->aiGetGimbalPresetListR(&ids);
        addResult("Gimbal", "gimbal_preset_list", ret, ret == 0 ? "responsive" : QString());
    }
    if (!cancelled()) {
        struct { Device::DevGimbalParaType type; const char *name; } gimbalFloatParams[] = {
            {Device::DevGimbalParaTypePanMin, "gimbal_pan_min"},
            {Device::DevGimbalParaTypePanMax, "gimbal_pan_max"},
            {Device::DevGimbalParaTypePitchMin, "gimbal_pitch_min"},
            {Device::DevGimbalParaTypePitchMax, "gimbal_pitch_max"},
            {Device::DevGimbalParaTypePresetSpeed, "gimbal_preset_speed"},
            {Device::DevGimbalParaTypeRollBias, "gimbal_roll_bias"},
        };
        for (const auto &p : gimbalFloatParams) {
            if (cancelled()) break;
            float val = 0;
            int32_t ret = m_device->aiGetGimbalParaR(p.type, val);
            addResult("Gimbal", p.name, ret, ret == 0 ? QString::number(val, 'f', 2) : QString());
        }
    }
    if (!cancelled()) {
        bool panReverse = false;
        int32_t ret = m_device->aiGetGimbalParaR(Device::DevGimbalParaTypePanReverse, panReverse);
        addResult("Gimbal", "gimbal_pan_reverse", ret,
                  ret == 0 ? (panReverse ? "reversed" : "normal") : QString());
    }

    // ========================================================================
    // Zone Tracking
    // ========================================================================
    if (!cancelled()) {
        Device::DevDataArray ids{};
        int32_t ret = m_device->aiGetZonePresetListR(&ids);
        addResult("Zone Tracking", "zone_preset_list", ret, ret == 0 ? "responsive" : QString());
    }
    if (!cancelled()) {
        bool enabled = false;
        int32_t ret = m_device->aiGetLimitedZoneTrackEnabledR(enabled);
        addResult("Zone Tracking", "limited_zone_enabled", ret,
                  ret == 0 ? (enabled ? "enabled" : "disabled") : QString());
    }
    if (!cancelled()) {
        bool autoSelect = false;
        int32_t ret = m_device->aiGetLimitedZoneTrackAutoSelectR(autoSelect);
        addResult("Zone Tracking", "zone_auto_select", ret,
                  ret == 0 ? (autoSelect ? "enabled" : "disabled") : QString());
    }

    // ========================================================================
    // Gesture Control
    // ========================================================================
    if (!cancelled()) {
        struct { Device::DevGestureParaType type; const char *name; } gestureParams[] = {
            {Device::DevGestureParaTypeGesture, "gesture_main"},
            {Device::DevGestureParaTypeTargetSelection, "gesture_target_select"},
            {Device::DevGestureParaTypeZoom, "gesture_zoom"},
            {Device::DevGestureParaTypeDynamicZoom, "gesture_dynamic_zoom"},
            {Device::DevGestureParaTypeRecord, "gesture_record"},
            {Device::DevGestureParaTypeSnapshot, "gesture_snapshot"},
            {Device::DevGestureParaTypeRolling, "gesture_rolling"},
            {Device::DevGestureParaTypeMirror, "gesture_mirror"},
        };
        for (const auto &p : gestureParams) {
            if (cancelled()) break;
            bool val = false;
            int32_t ret = m_device->aiGetGestureParaR(p.type, val);
            addResult("Gesture Control", p.name, ret,
                      ret == 0 ? (val ? "enabled" : "disabled") : QString());
        }
    }
    if (!cancelled()) {
        float zoomFactor = 0;
        int32_t ret = m_device->aiGetGestureParaR(Device::DevGestureParaTypeZoomFactor, zoomFactor);
        addResult("Gesture Control", "gesture_zoom_factor", ret,
                  ret == 0 ? QString::number(zoomFactor, 'f', 1) : QString());
    }

    // ========================================================================
    // Gesture Track Parameters
    // ========================================================================
    if (!cancelled()) {
        struct { Device::DevGestureTrackParaType type; const char *name; } gestureTrackFloat[] = {
            {Device::DevGestureTrackParaTypePanMin, "gesture_track_pan_min"},
            {Device::DevGestureTrackParaTypePanMax, "gesture_track_pan_max"},
            {Device::DevGestureTrackParaTypePitchMin, "gesture_track_pitch_min"},
            {Device::DevGestureTrackParaTypePitchMax, "gesture_track_pitch_max"},
            {Device::DevGestureTrackParaTypeRestSeconds, "gesture_track_rest_seconds"},
        };
        for (const auto &p : gestureTrackFloat) {
            if (cancelled()) break;
            float val = 0;
            int32_t ret = m_device->aiGetGestureTrackParaR(p.type, val);
            addResult("Gesture Track", p.name, ret,
                      ret == 0 ? QString::number(val, 'f', 2) : QString());
        }
    }
    if (!cancelled()) {
        bool handType = false;
        int32_t ret = m_device->aiGetGestureTrackParaR(Device::DevGestureTrackParaTypeHandType, handType);
        addResult("Gesture Track", "gesture_track_hand_type", ret,
                  ret == 0 ? (handType ? "left" : "right") : QString());
    }
    if (!cancelled()) {
        int trackSpeed = 0;
        int32_t ret = m_device->aiGetGestureTrackParaR(Device::DevGestureTrackParaTypeTrackSpeed, trackSpeed);
        addResult("Gesture Track", "gesture_track_speed", ret,
                  ret == 0 ? QString::number(trackSpeed) : QString());
    }
    if (!cancelled()) {
        struct { Device::DevGestureTrackParaType type; const char *name; } gestureTrackBool[] = {
            {Device::DevGestureTrackParaTypePanEnabled, "gesture_track_pan_enabled"},
            {Device::DevGestureTrackParaTypePitchEnabled, "gesture_track_pitch_enabled"},
        };
        for (const auto &p : gestureTrackBool) {
            if (cancelled()) break;
            bool val = false;
            int32_t ret = m_device->aiGetGestureTrackParaR(p.type, val);
            addResult("Gesture Track", p.name, ret,
                      ret == 0 ? (val ? "enabled" : "disabled") : QString());
        }
    }

    // ========================================================================
    // AI Control Parameters
    // ========================================================================
    if (!cancelled()) {
        Device::DevControlTargetType targets[] = {
            Device::DevControlTargetTypeHuman,
            Device::DevControlTargetTypeAnimal,
            Device::DevControlTargetTypeObject,
        };
        const char *targetNames[] = {"human", "animal", "object"};

        struct { Device::DevControlParaType type; const char *suffix; char dataType; } controlParams[] = {
            {Device::DevControlParaTypeMotion, "motion", 'b'},
            {Device::DevControlParaTypeForeTrack, "fore_track", 'b'},
            {Device::DevControlParaTypeComposition, "composition", 'b'},
            {Device::DevControlParaTypeTrackerType, "tracker_type", 'i'},
            {Device::DevControlParaTypeGimCtrlMode, "gim_ctrl_mode", 'i'},
            {Device::DevControlParaTypeGimCtrlSpeedMode, "gim_ctrl_speed", 'i'},
            {Device::DevControlParaTypePanGainAdaptive, "pan_gain_adaptive", 'b'},
            {Device::DevControlParaTypePanGainValue, "pan_gain_value", 'f'},
            {Device::DevControlParaTypePanLocked, "pan_locked", 'b'},
            {Device::DevControlParaTypePitchGainAdaptive, "pitch_gain_adaptive", 'b'},
            {Device::DevControlParaTypePitchGainValue, "pitch_gain_value", 'f'},
            {Device::DevControlParaTypePitchLocked, "pitch_locked", 'b'},
            {Device::DevControlParaTypeAutoZoomCustomized, "autozoom_custom", 'i'},
            {Device::DevControlParaTypeAutoZoomSpeed, "autozoom_speed", 'i'},
            {Device::DevControlParaTypeOffsetAdaptiveX, "offset_adaptive_x", 'b'},
            {Device::DevControlParaTypeOffsetX, "offset_x", 'f'},
            {Device::DevControlParaTypeOffsetAdaptiveY, "offset_adaptive_y", 'b'},
            {Device::DevControlParaTypeOffsetY, "offset_y", 'f'},
        };

        for (int t = 0; t < 3 && !cancelled(); ++t) {
            for (const auto &p : controlParams) {
                if (cancelled()) break;
                QString name = QString("%1_%2").arg(targetNames[t], p.suffix);
                int32_t ret;
                QString details;
                if (p.dataType == 'b') {
                    bool val = false;
                    ret = m_device->aiGetControlParaR(targets[t], p.type, val);
                    details = ret == 0 ? (val ? "true" : "false") : QString();
                } else if (p.dataType == 'f') {
                    float val = 0;
                    ret = m_device->aiGetControlParaR(targets[t], p.type, val);
                    details = ret == 0 ? QString::number(val, 'f', 2) : QString();
                } else {
                    int val = 0;
                    ret = m_device->aiGetControlParaR(targets[t], p.type, val);
                    details = ret == 0 ? QString::number(val) : QString();
                }
                addResult("AI Control Params", name, ret, details);
            }
        }
    }

    // ========================================================================
    // AI Offset
    // ========================================================================
    if (!cancelled()) {
        bool autoOffset = false;
        int32_t ret = m_device->aiGetAutoOffsetEnable(autoOffset);
        addResult("AI Offset", "auto_offset", ret,
                  ret == 0 ? (autoOffset ? "enabled" : "disabled") : QString());
    }
    if (!cancelled()) {
        float hOff = 0, vOff = 0;
        int32_t retH = m_device->aiGetHorizontalOffset(hOff);
        addResult("AI Offset", "horizontal_offset", retH,
                  retH == 0 ? QString::number(hOff, 'f', 2) : QString());
        int32_t retV = m_device->aiGetVerticalOffset(vOff);
        addResult("AI Offset", "vertical_offset", retV,
                  retV == 0 ? QString::number(vOff, 'f', 2) : QString());
    }

    // ========================================================================
    // Video Recording & Encoding
    // ========================================================================
    if (!cancelled()) {
        Device::DevVideoEncoderFormat format;
        int32_t ret = m_device->cameraGetMainVideoEncoderFormatR(format);
        addResult("Recording", "main_encoder_format", ret,
                  ret == 0 ? QString::number(static_cast<int>(format)) : QString());
    }
    if (!cancelled()) {
        Device::DevVideoBitLevelType bitLevel;
        int32_t ret = m_device->cameraGetMainVideoBitrateLevelR(bitLevel);
        addResult("Recording", "main_bitrate_level", ret,
                  ret == 0 ? QString::number(static_cast<int>(bitLevel)) : QString());
    }
    if (!cancelled()) {
        Device::DevVideoSplitSizeType splitType;
        int32_t ret = m_device->cameraGetRecordSplitSizeR(splitType);
        addResult("Recording", "record_split_size", ret,
                  ret == 0 ? QString::number(static_cast<int>(splitType)) : QString());
    }
    if (!cancelled()) {
        Device::DevMediaEncodeParam encParam{};
        int32_t ret = m_device->cameraGetRecordEncodeParamR(encParam);
        addResult("Recording", "record_encode_param", ret,
                  ret == 0 ? QString("w=%1 h=%2").arg(encParam.width).arg(encParam.height) : QString());
    }
    if (!cancelled()) {
        Device::DevMediaEncodeParam encParam{};
        int32_t ret = m_device->cameraGetOutputEncodeParamR(encParam);
        addResult("Recording", "output_encode_param", ret,
                  ret == 0 ? QString("w=%1 h=%2").arg(encParam.width).arg(encParam.height) : QString());
    }
    if (!cancelled()) {
        uint32_t delay = 0;
        int32_t ret = m_device->cameraGetDelayTimeInTimelapse(delay);
        addResult("Recording", "timelapse_delay", ret,
                  ret == 0 ? QString::number(delay) : QString());
    }
    if (!cancelled()) {
        bool bootEnabled = false;
        uint32_t mainMode = 0, subMode = 0, action = 0;
        int32_t ret = m_device->cameraGetBootStatus(bootEnabled, mainMode, subMode, action);
        addResult("Recording", "boot_status", ret,
                  ret == 0 ? QString("enabled=%1 main=%2 sub=%3 action=%4")
                    .arg(bootEnabled).arg(mainMode).arg(subMode).arg(action) : QString());
    }

    // ========================================================================
    // NDI / RTSP / Streaming
    // ========================================================================
    if (!cancelled()) {
        Device::RtspOrNdiEnabled ndiRtsp;
        int32_t ret = m_device->cameraGetSelectNdiOrRtspR(ndiRtsp);
        addResult("Streaming", "ndi_rtsp_selection", ret,
                  ret == 0 ? QString::number(static_cast<int>(ndiRtsp)) : QString());
    }
    if (!cancelled()) {
        Device::DevVideoEncoderFormat format;
        int32_t ret = m_device->cameraGetNdiRtspEncoderFormatR(format);
        addResult("Streaming", "ndi_rtsp_encoder", ret,
                  ret == 0 ? QString::number(static_cast<int>(format)) : QString());
    }
    if (!cancelled()) {
        Device::DevVideoBitLevelType bitLevel;
        int32_t ret = m_device->cameraGetNdiRtspBitrateLevelR(bitLevel);
        addResult("Streaming", "ndi_rtsp_bitrate", ret,
                  ret == 0 ? QString::number(static_cast<int>(bitLevel)) : QString());
    }
    if (!cancelled()) {
        bool activate = false;
        int32_t ret = m_device->cameraGetModuleActiveR(Device::DevActivateModuleTypeNdi, activate);
        addResult("Streaming", "ndi_module_active", ret,
                  ret == 0 ? (activate ? "activated" : "not activated") : QString());
    }

    // ========================================================================
    // HDMI & Watermark
    // ========================================================================
    if (!cancelled()) {
        Device::HdmiInfo hdmi{};
        int32_t ret = m_device->cameraGetHdmiInfoR(hdmi);
        addResult("HDMI & Watermark", "hdmi_info", ret, ret == 0 ? "responsive" : QString());
    }
    if (!cancelled()) {
        bool watermark = false;
        int32_t ret = m_device->cameraGetWatermarkAttributeR(watermark);
        addResult("HDMI & Watermark", "watermark", ret,
                  ret == 0 ? (watermark ? "enabled" : "disabled") : QString());
    }

    // ========================================================================
    // System Management
    // ========================================================================
    if (!cancelled()) {
        Device::DevBuzzerStatus buzzer;
        int32_t ret = m_device->sysMgGetBuzzerEnabledR(buzzer);
        addResult("System", "buzzer", ret,
                  ret == 0 ? QString::number(static_cast<int>(buzzer)) : QString());
    }
    if (!cancelled()) {
        std::string devName;
        int32_t ret = m_device->sysMgGetDeviceNameR(devName);
        addResult("System", "device_name", ret,
                  ret == 0 ? QString::fromStdString(devName) : QString());
    }

    // ========================================================================
    // UVC Extension Unit Probe (encapsulated)
    // ========================================================================
    UvcProbeReport uvcReport;
    if (!cancelled()) {
        QString devPath = QString::fromStdString(m_device->videoDevPath());
        uvcReport = probeUvcExtensionUnits(devPath);

        addResult("UVC Extension Units", "probe_status",
                  uvcReport.success ? 0 : -1,
                  uvcReport.success ? QString("%1 XU(s), %2 control(s)")
                      .arg(uvcReport.extensionUnits.size())
                      .arg(uvcReport.controls.size())
                  : uvcReport.error);

        for (const auto &xu : uvcReport.extensionUnits) {
            addResult("UVC Extension Units",
                      QString("xu_%1_guid").arg(xu.unitId), 0,
                      QString("%1 (%2 controls)").arg(xu.guidEntity).arg(xu.numControls));
        }

        for (const auto &ctrl : uvcReport.controls) {
            if (cancelled()) break;
            QString name = QString("xu%1_sel%2").arg(ctrl.unitId).arg(ctrl.selector, 2, 10, QChar('0'));
            QString flags;
            if (ctrl.getCur) flags += "GET ";
            if (ctrl.setCur) flags += "SET ";
            if (ctrl.getMin) flags += "MIN ";
            if (ctrl.getMax) flags += "MAX ";
            if (ctrl.getDef) flags += "DEF ";

            QString details = QString("len=%1 flags=[%2]").arg(ctrl.length).arg(flags.trimmed());

            if (!ctrl.curValue.isEmpty()) {
                QStringList hex;
                for (int i = 0; i < ctrl.curValue.size() && i < 16; ++i)
                    hex << QString("%1").arg(static_cast<uint8_t>(ctrl.curValue[i]), 2, 16, QChar('0'));
                details += QString(" cur=[%1]").arg(hex.join(" "));
            }

            addResult("UVC Extension Units", name, 0, details);
        }

        m_uvcProbeText = formatUvcProbeReport(uvcReport);
    }

    bool wasCancelled = cancelled();

    // Update capabilities and save (these touch m_diagnosticsReport so do it here
    // while still on the worker thread, before signalling the main thread)
    populateCapabilities();
    saveDiagnosticsToFile();

    m_extendedDiagRunning.store(false);

    // Signal completion on the main thread
    QMetaObject::invokeMethod(this, [this, wasCancelled, uvcReport]() {
        emit extendedDiagnosticsCompleted(!wasCancelled && uvcReport.success,
                                          uvcReport.controls.size());
    }, Qt::QueuedConnection);
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

    if (m_capabilities.aiModes) {
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
    if (m_capabilities.aiModes) {
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
