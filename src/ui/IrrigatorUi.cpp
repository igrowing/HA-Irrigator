#include "IrrigatorUi.h"

#include "../drivers/Logger.h"
#include "board_config.h"

namespace irrigator::ui {

namespace {

const char* noText = "NO";
const char* yesText = "YES";
const int16_t noX = 35;
const int16_t yesX = 70;

void drawCentered(U8G2& display, uint8_t y, const char* text) {
    const uint16_t width = display.getStrWidth(text);
    const int16_t x = static_cast<int16_t>((128 - static_cast<int16_t>(width)) / 2);
    display.drawStr(x < 0 ? 0 : x, y, text);
}

// Deliberately narrower than IrrigationSnapshot::pumpOn (state_ != Idle, which also covers
// PumpGracePeriod — the pump lingering post-run with nothing actually active). The UI treats the
// grace tail as idle-equivalent: normal screen falls back to IDLE-READY/SCHEDULED, rotary ticks
// work again (a fresh pick hot-hands-off the still-spinning pump, same mechanism the queue
// already uses), and the menu drops "Stop"/"Stop all" since there's nothing left to abort.
bool isActivelyRunning(const ::irrigator::domain::IrrigationSnapshot& snapshot) {
    using ::irrigator::domain::IrrigationState;
    return snapshot.state == IrrigationState::Watering || snapshot.state == IrrigationState::PumpOnly;
}

}  // namespace

void IrrigatorUi::begin() {
    display_.begin();
    display_.setContrast(20);
    display_.clearBuffer();
    display_.sendBuffer();
    input_.begin();
    display_.setDisplayRotation(U8G2_R2);
    ::irrigator::drivers::Log.info("UI initialized (SSD1309 + rotary input)");
}

void IrrigatorUi::wake(uint32_t nowMs) {
    uiState_.wake(nowMs);
}

void IrrigatorUi::confirmFactoryReset(uint32_t nowMs) {
    uiState_.enterFactoryResetConfirm(nowMs);
}

void IrrigatorUi::showFactoryResetResult(bool success, uint32_t nowMs) {
    uiState_.showFactoryResetResult(success, nowMs);
    render(nowMs, RenderData{});
}

void IrrigatorUi::showCaptivePortal(const String& apSsid, const IPAddress& ip) {
    display_.clearBuffer();
    display_.setFont(u8g2_font_6x10_tr);
    drawCentered(display_, 10, "= HA-IRRIGATOR =");
    char line[40];
    snprintf(line, sizeof(line), "SSID: %s", apSsid.c_str());
    drawCentered(display_, 26, line);
    snprintf(line, sizeof(line), "IP: %s", ip.toString().c_str());
    drawCentered(display_, 40, line);
    drawCentered(display_, 60, "Connect&Configure");
    display_.sendBuffer();
}

const char* IrrigatorUi::zoneLabel(const RenderData& data, uint8_t zone) const {
    if (zone == 0) {
        return "PUMP";
    }
    if (data.settings != nullptr && zone >= 1 && zone <= irrigator::board::ZONE_COUNT) {
        snprintf(zoneLabelBuf_, sizeof(zoneLabelBuf_), "%u.%s", zone, data.settings->zoneNames[zone - 1]);
        return zoneLabelBuf_;
    }
    return "ZONE";
}

const char* IrrigatorUi::dayOfWeekName(uint8_t isoDay) {
    static const char* const names[] = {"", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return (isoDay >= 1 && isoDay <= 7) ? names[isoDay] : "?";
}

::irrigator::domain::UiIntent IrrigatorUi::poll(uint32_t nowMs, const RenderData& data) {
    const ::irrigator::drivers::RotaryEvent rotaryEvent = input_.poll();

    ::irrigator::domain::UiStateMachine::Context ctx;
    ctx.wateringActive = isActivelyRunning(data.snapshot);
    ctx.scheduleActive = data.scheduleActive;
    ctx.nextScheduledZone = data.nextScheduledZone;
    ctx.scheduleRecordCount = data.schedule != nullptr ? data.schedule->count : 0;
    ctx.wateringPercentage = data.settings != nullptr ? data.settings->wateringPercentage : 0;
    ctx.upperLimitPercent = data.settings != nullptr ? data.settings->upperLimitPercent : 0;
    ctx.lowerLimitPercent = data.settings != nullptr ? data.settings->lowerLimitPercent : 0;
    ctx.rainSkipProbabilityPercent = data.settings != nullptr ? data.settings->rainSkipProbabilityPercent : 0;
    ctx.rainSkipAmountMm = data.settings != nullptr ? data.settings->rainSkipAmountMm : 0;
    ctx.defaultManualDuration = data.settings != nullptr ? data.settings->defaultManualDuration : 0;
    if (data.settings != nullptr) {
        for (uint8_t i = 0; i < irrigator::board::ZONE_COUNT; ++i) {
            ctx.zoneEnabled[i] = data.settings->zoneEnabled[i];
        }
    }

    if (uiState_.screen() == ::irrigator::domain::UiScreen::MenuSchedule && data.schedule != nullptr) {
        const uint8_t recordIndex = static_cast<uint8_t>(
            uiState_.selectedIndex() / ::irrigator::domain::UiStateMachine::kScheduleFieldsPerRecord);
        if (recordIndex < data.schedule->count) {
            const auto& record = data.schedule->records[recordIndex];
            ctx.selectedScheduleZone = record.zone;
            ctx.selectedScheduleDay = record.dayOfWeek;
            ctx.selectedScheduleHour =
                static_cast<uint8_t>((record.startTime[0] - '0') * 10 + (record.startTime[1] - '0'));
            ctx.selectedScheduleMinute =
                static_cast<uint8_t>((record.startTime[3] - '0') * 10 + (record.startTime[4] - '0'));
            ctx.selectedScheduleDurationMin = record.durationMin;
            ctx.selectedScheduleActive = record.active;
        }
    }

    ::irrigator::domain::UiIntent intent;
    switch (rotaryEvent) {
        case ::irrigator::drivers::RotaryEvent::Clockwise:
            intent = uiState_.handleInput(::irrigator::domain::UiInputEvent::Clockwise, ctx, nowMs);
            break;
        case ::irrigator::drivers::RotaryEvent::CounterClockwise:
            intent =
                uiState_.handleInput(::irrigator::domain::UiInputEvent::CounterClockwise, ctx, nowMs);
            break;
        case ::irrigator::drivers::RotaryEvent::Click:
            intent = uiState_.handleInput(::irrigator::domain::UiInputEvent::Click, ctx, nowMs);
            break;
        case ::irrigator::drivers::RotaryEvent::LongClick:
            intent = uiState_.handleInput(::irrigator::domain::UiInputEvent::LongClick, ctx, nowMs);
            break;
        case ::irrigator::drivers::RotaryEvent::None:
        default:
            uiState_.tick(nowMs);
            break;
    }

    render(nowMs, data);
    return intent;
}

void IrrigatorUi::render(uint32_t nowMs, const RenderData& data) {
    const bool screenChanged = uiState_.screen() != lastRenderedScreen_;

    if (uiState_.screen() == ::irrigator::domain::UiScreen::DisplayOff) {
        if (screenChanged) {
            display_.clearBuffer();
            display_.sendBuffer();
            lastRenderedScreen_ = uiState_.screen();
        }
        return;
    }

    if (!screenChanged && nowMs - lastRenderMs_ < kRenderIntervalMs) {
        return;
    }
    lastRenderMs_ = nowMs;
    lastRenderedScreen_ = uiState_.screen();

    switch (uiState_.screen()) {
        case ::irrigator::domain::UiScreen::Normal:
            renderNormal(nowMs, data);
            break;
        case ::irrigator::domain::UiScreen::ZonePicker:
            renderZonePicker(data);
            break;
        case ::irrigator::domain::UiScreen::DurationPicker:
            renderDurationPicker(data);
            break;
        case ::irrigator::domain::UiScreen::StopConfirm:
            renderStopConfirm(data);
            break;
        case ::irrigator::domain::UiScreen::MenuMain:
            renderMenuMain(data);
            break;
        case ::irrigator::domain::UiScreen::MenuSchedule:
            renderMenuSchedule(data);
            break;
        case ::irrigator::domain::UiScreen::MenuSettings:
            renderMenuSettings(data);
            break;
        case ::irrigator::domain::UiScreen::MenuTestAll:
            renderMenuTestAll();
            break;
        case ::irrigator::domain::UiScreen::RestartConfirm:
            renderRestartConfirm();
            break;
        case ::irrigator::domain::UiScreen::FactoryResetConfirm:
            renderFactoryResetConfirm();
            break;
        case ::irrigator::domain::UiScreen::FactoryResetResult:
            renderFactoryResetResult();
            break;
        case ::irrigator::domain::UiScreen::DisplayOff:
        default:
            break;
    }
}

void IrrigatorUi::drawTopStatusLine(const RenderData& data) {
    display_.setFont(u8g2_font_5x7_tr);
    char upperLeft[16] = "NOSIG";
    if (data.deviceIp != IPAddress(0, 0, 0, 0)) {
        snprintf(upperLeft, sizeof(upperLeft), "IP.%u", data.deviceIp[3]);
    }
    display_.drawStr(0, board::DISPLAY_1ST_LINE_OFFSET, upperLeft);

    char upperRight[24] = "";
    if (data.hasWallClock) {
        snprintf(upperRight,
                sizeof(upperRight),
                "%04d-%02d-%02d %02d:%02d:%02d",
                data.wallClockLocal.tm_year + 1900,
                data.wallClockLocal.tm_mon + 1,
                data.wallClockLocal.tm_mday,
                data.wallClockLocal.tm_hour,
                data.wallClockLocal.tm_min,
                data.wallClockLocal.tm_sec);
    }
    const uint16_t rightWidth = display_.getStrWidth(upperRight);
    display_.drawStr(128 - static_cast<int16_t>(rightWidth), board::DISPLAY_1ST_LINE_OFFSET, upperRight);

    // Signal-quality dashes under the IP: 0 (miserable/no signal yet) to 4 (excellent), one dash
    // per irrigator::domain::WifiQuality step — deliberately just dashes, no numeric dB, so it
    // reads at a glance without competing with the rest of this already-packed line.
    char bars[5] = "";
    const uint8_t barCount = ::irrigator::domain::wifiQualityBarCount(
        ::irrigator::domain::wifiQualityFromRssi(data.wifiRssi));
    for (uint8_t i = 0; i < barCount; ++i) {
        bars[i] = '-';
    }
    bars[barCount] = '\0';
    display_.drawStr(0, board::DISPLAY_1ST_LINE_OFFSET + 4, bars);
}

void IrrigatorUi::renderNormal(uint32_t nowMs, const RenderData& data) {
    display_.clearBuffer();

    drawTopStatusLine(data);

    const bool scheduled = data.scheduleActive && data.nextScheduledZone != 0;
    const bool running = isActivelyRunning(data.snapshot);

    display_.setFont(u8g2_font_helvB14_tr);
    char mainText[20] = "";
    if (running) {
        snprintf(mainText, sizeof(mainText), "%s", zoneLabel(data, data.snapshot.activeZone));
    } else if (scheduled) {
        snprintf(mainText, sizeof(mainText), "%s", data.nextScheduledZoneName);
    } else {
        // A plain Click with no prior rotation defaults to pump-only (see
        // UiStateMachine::handleNormal) — show that here so the idle screen matches what
        // clicking actually starts, instead of a zone name the click won't touch.
        snprintf(mainText, sizeof(mainText), "%s", zoneLabel(data, 0));
    }
    drawCentered(display_, board::DISPLAY_2ND_LINE_OFFSET, mainText);

    display_.setFont(u8g2_font_helvB10_tr);
    char statusText[24] = "";
    if (running) {
        const uint32_t elapsedSec = data.snapshot.elapsedMs / 1000UL;
        const uint32_t remainingSec = data.snapshot.remainingMs / 1000UL;
        if ((nowMs / 3000UL) % 2 == 0) {
            snprintf(statusText,
                    sizeof(statusText),
                    "Watering %u:%02u",
                    static_cast<unsigned>(elapsedSec / 60UL),
                    static_cast<unsigned>(elapsedSec % 60UL));
        } else {
            snprintf(statusText,
                    sizeof(statusText),
                    "Remaining %u:%02u",
                    static_cast<unsigned>(remainingSec / 60UL),
                    static_cast<unsigned>(remainingSec % 60UL));
        }
    } else if (scheduled) {
        snprintf(statusText, sizeof(statusText), "SCHEDULED");
    } else {
        snprintf(statusText, sizeof(statusText), "IDLE-READY");
    }
    drawCentered(display_, board::DISPLAY_3RD_LINE_OFFSET, statusText);

    display_.setFont(u8g2_font_5x7_tr);
    if (running) {
        drawCentered(display_, board::DISPLAY_4TH_LINE_OFFSET, "LONG CLICK = MENU");
    } else {
        drawCentered(display_, board::DISPLAY_4TH_LINE_OFFSET, "< PREV. o SELECT NEXT >");
    }

    display_.sendBuffer();
}

void IrrigatorUi::renderZonePicker(const RenderData& data) {
    display_.clearBuffer();
    drawTopStatusLine(data);
    display_.setFont(u8g2_font_helvB14_tr);
    drawCentered(display_, board::DISPLAY_2ND_LINE_OFFSET, zoneLabel(data, uiState_.pendingZone()));
    display_.setFont(u8g2_font_helvB10_tr);
    drawCentered(display_, board::DISPLAY_3RD_LINE_OFFSET, "SELECT ZONE");
    display_.setFont(u8g2_font_5x7_tr);
    drawCentered(display_, board::DISPLAY_4TH_LINE_OFFSET, "< PREV. o SELECT NEXT >");
    display_.sendBuffer();
}

void IrrigatorUi::renderDurationPicker(const RenderData& data) {
    display_.clearBuffer();
    drawTopStatusLine(data);
    display_.setFont(u8g2_font_helvB14_tr);
    char text[16];
    snprintf(text, sizeof(text), "%u MINs", static_cast<unsigned>(uiState_.pendingDuration()));
    drawCentered(display_, board::DISPLAY_2ND_LINE_OFFSET, text);
    display_.setFont(u8g2_font_helvB10_tr);
    drawCentered(display_, board::DISPLAY_3RD_LINE_OFFSET, zoneLabel(data, uiState_.pendingZone()));
    display_.setFont(u8g2_font_5x7_tr);
    drawCentered(display_, board::DISPLAY_4TH_LINE_OFFSET, "< LESS o START MORE >");
    display_.sendBuffer();
}

void IrrigatorUi::renderStopConfirm(const RenderData& data) {
    (void)data;
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB08_tr);
    drawCentered(display_, 30, uiState_.stopAllPending() ? "STOP ALL?" : "STOP?");

    const uint16_t noWidth = display_.getStrWidth(noText);
    const uint16_t yesWidth = display_.getStrWidth(yesText);
    if (!uiState_.stopConfirmYesSelected()) {
        display_.drawFrame(noX - 4, 40, noWidth + 8, 16);
    } else {
        display_.drawFrame(yesX - 4, 40, yesWidth + 8, 16);
    }
    display_.drawStr(noX, 52, noText);
    display_.drawStr(yesX, 52, yesText);
    display_.sendBuffer();
}

void IrrigatorUi::renderRestartConfirm() {
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB08_tr);
    drawCentered(display_, 30, "RESTART?");

    const uint16_t noWidth = display_.getStrWidth(noText);
    const uint16_t yesWidth = display_.getStrWidth(yesText);
    if (!uiState_.restartConfirmYesSelected()) {
        display_.drawFrame(noX - 4, 40, noWidth + 8, 16);
    } else {
        display_.drawFrame(yesX - 4, 40, yesWidth + 8, 16);
    }
    display_.drawStr(noX, 52, noText);
    display_.drawStr(yesX, 52, yesText);
    display_.sendBuffer();
}

void IrrigatorUi::renderFactoryResetConfirm() {
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB08_tr);
    drawCentered(display_, 20, "FACTORY RESET?");
    display_.setFont(u8g2_font_6x10_tr);
    drawCentered(display_, 32, "Erase config/settings");
    drawCentered(display_, 42, "/schedule, restart.");

    display_.setFont(u8g2_font_helvB10_tr);

    const uint16_t noWidth = display_.getStrWidth(noText);
    const uint16_t yesWidth = display_.getStrWidth(yesText);
    if (!uiState_.factoryResetConfirmYesSelected()) {
        display_.drawFrame(noX - 4, 48, noWidth + 8, 16);
    } else {
        display_.drawFrame(yesX - 4, 48, yesWidth + 8, 16);
    }
    display_.drawStr(noX, 60, noText);
    display_.drawStr(yesX, 60, yesText);
    display_.sendBuffer();
}

void IrrigatorUi::renderFactoryResetResult() {
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB10_tr);
    drawCentered(display_, 30, "FACTORY RESET");
    display_.setFont(u8g2_font_helvB14_tr);
    drawCentered(display_, 52, uiState_.factoryResetSucceeded() ? "SUCCESS" : "FAILURE");
    display_.sendBuffer();
}

void IrrigatorUi::drawMenuList(const char* title,
                               uint8_t itemCount,
                               uint8_t selectedIndex,
                               const MenuLabelBuilder& labelBuilder) {
    display_.clearBuffer();
    display_.setFont(u8g2_font_6x10_tr);
    display_.drawStr(0, 8, title);

    constexpr uint8_t kMaxVisible = 5;
    constexpr uint8_t kRowHeight = 11;
    constexpr uint8_t kStartY = 20;

    uint8_t startIdx = 0;
    if (selectedIndex >= kMaxVisible) {
        startIdx = static_cast<uint8_t>(selectedIndex - kMaxVisible + 1);
    }
    uint8_t endIdx = static_cast<uint8_t>(startIdx + kMaxVisible);
    if (endIdx > itemCount) {
        endIdx = itemCount;
        startIdx = endIdx > kMaxVisible ? static_cast<uint8_t>(endIdx - kMaxVisible) : 0;
    }

    char label[24];
    for (uint8_t i = startIdx; i < endIdx; ++i) {
        labelBuilder(i, label, sizeof(label));
        const uint8_t rowY = static_cast<uint8_t>(kStartY + (i - startIdx) * kRowHeight);
        if (i == selectedIndex) {
            display_.setDrawColor(1);
            display_.drawBox(0, rowY - 9, 128, kRowHeight);
            display_.setDrawColor(0);
        }
        display_.drawStr(4, rowY, label);
        if (i == selectedIndex) {
            display_.setDrawColor(1);
        }
    }

    display_.sendBuffer();
}

void IrrigatorUi::renderMenuMain(const RenderData& data) {
    const bool wateringActive = isActivelyRunning(data.snapshot);
    const auto layout = ::irrigator::domain::UiStateMachine::menuMainLayout(wateringActive);

    drawMenuList(
        "MENU", layout.itemCount, uiState_.selectedIndex(), [this, &data, wateringActive, layout](uint8_t index, char* buffer, size_t size) {
            if (wateringActive && index == 0) {
                snprintf(buffer, size, "Stop %s", zoneLabel(data, data.snapshot.activeZone));
            } else if (wateringActive && index == 1) {
                snprintf(buffer, size, "Stop all");
            } else if (index == layout.scheduleIndex) {
                snprintf(buffer, size, "Schedule");
            } else if (index == layout.testAllIndex) {
                snprintf(buffer, size, "Test all zones");
            } else if (index == layout.settingsIndex) {
                snprintf(buffer, size, "Settings");
            } else if (index == layout.backIndex) {
                snprintf(buffer, size, "BACK");
            } else {
                buffer[0] = '\0';
            }
        });
}

void IrrigatorUi::renderMenuSettings(const RenderData& data) {
    using Settings = ::irrigator::domain::UiStateMachine;

    // Shows the pending (still-being-ticked) value while that exact row is mid-edit, otherwise
    // the live settings value — one place for the 5 editable-int16-row displays instead of each
    // repeating the same "am I the row currently being edited" check.
    auto displayValue = [this](uint8_t index, int16_t liveValue, int16_t pendingValue) {
        return (uiState_.editing() && uiState_.selectedIndex() == index) ? pendingValue : liveValue;
    };

    const int16_t percentage = displayValue(Settings::kSettingsIndexPercentage,
                                            data.settings != nullptr ? data.settings->wateringPercentage : 0,
                                            uiState_.pendingPercentage());
    const int16_t upperLimit = displayValue(Settings::kSettingsIndexUpperLimit,
                                            data.settings != nullptr ? data.settings->upperLimitPercent : 0,
                                            uiState_.pendingUpperLimit());
    const int16_t lowerLimit = displayValue(Settings::kSettingsIndexLowerLimit,
                                            data.settings != nullptr ? data.settings->lowerLimitPercent : 0,
                                            uiState_.pendingLowerLimit());
    const int16_t rainSkipProbability =
        displayValue(Settings::kSettingsIndexRainSkipProbability,
                    data.settings != nullptr ? data.settings->rainSkipProbabilityPercent : 0,
                    uiState_.pendingRainSkipProbability());
    const int16_t rainSkipAmount = displayValue(Settings::kSettingsIndexRainSkipAmount,
                                                data.settings != nullptr ? data.settings->rainSkipAmountMm : 0,
                                                uiState_.pendingRainSkipAmount());

    drawMenuList(
        "SETTINGS", Settings::kSettingsItemCount, uiState_.selectedIndex(),
        [this, &data, percentage, upperLimit, lowerLimit, rainSkipProbability, rainSkipAmount](
            uint8_t index, char* buffer, size_t size) {
            switch (index) {
                case Settings::kSettingsIndexPercentage:
                    snprintf(buffer, size, "Adjust: %d %%", percentage);
                    break;
                case Settings::kSettingsIndexUpperLimit:
                    snprintf(buffer, size, "Upper limit: %d %%", upperLimit);
                    break;
                case Settings::kSettingsIndexLowerLimit:
                    snprintf(buffer, size, "Lower limit: %d %%", lowerLimit);
                    break;
                case Settings::kSettingsIndexRainSkipProbability:
                    snprintf(buffer, size, "Skip rain prob: %d %%", rainSkipProbability);
                    break;
                case Settings::kSettingsIndexRainSkipAmount:
                    snprintf(buffer, size, "Skip rain amt: %d mm", rainSkipAmount);
                    break;
                case Settings::kSettingsIndexScheduleActive:
                    snprintf(buffer, size, "Schedule: %s", data.scheduleActive ? "active" : "paused");
                    break;
                case Settings::kSettingsIndexWifi: {
                    const auto quality = ::irrigator::domain::wifiQualityFromRssi(data.wifiRssi);
                    snprintf(buffer,
                            size,
                            "WiFi: %s (%ddB)",
                            ::irrigator::domain::toString(quality),
                            static_cast<int>(data.wifiRssi));
                    break;
                }
                case Settings::kSettingsIndexIp:
                    snprintf(buffer, size, "IP: %s", data.deviceIp.toString().c_str());
                    break;
                case Settings::kSettingsIndexMqtt:
                    if (data.mqttBrokerText.length() == 0) {
                        snprintf(buffer, size, "MQTT: disabled");
                    } else {
                        snprintf(buffer, size, "MQTT: %s", data.mqttBrokerText.c_str());
                    }
                    break;
                case Settings::kSettingsIndexTopic:
                    if (data.mqttBrokerText.length() == 0) {
                        snprintf(buffer, size, "Topic: -");
                    } else {
                        snprintf(buffer, size, "Topic: %s", data.mqttTopicText.c_str());
                    }
                    break;
                case Settings::kSettingsIndexCity:
                    snprintf(buffer,
                            size,
                            "City: %s",
                            (data.settings != nullptr && data.settings->city[0] != '\0')
                                ? data.settings->city
                                : "-");
                    break;
                case Settings::kSettingsIndexCoordinates:
                    if (data.settings != nullptr && data.settings->locationKnown) {
                        snprintf(buffer, size, "Loc: %.2f,%.2f", data.settings->latitude, data.settings->longitude);
                    } else {
                        snprintf(buffer, size, "Loc: -");
                    }
                    break;
                case Settings::kSettingsIndexLastUpdate:
                    snprintf(buffer,
                            size,
                            "Last update: %s",
                            (data.lastWeatherUpdateDate != nullptr && data.lastWeatherUpdateDate[0] != '\0')
                                ? data.lastWeatherUpdateDate
                                : "-");
                    break;
                case Settings::kSettingsIndexFirmware:
                    snprintf(buffer, size, "FW: v.%s", irrigator::board::FIRMWARE_VERSION);
                    break;
                case Settings::kSettingsIndexRestart:
                    snprintf(buffer, size, "Restart");
                    break;
                case Settings::kSettingsIndexBack:
                    snprintf(buffer, size, "BACK");
                    break;
                default:
                    buffer[0] = '\0';
                    break;
            }
        });
}

void IrrigatorUi::renderMenuTestAll() {
    using TestAll = ::irrigator::domain::UiStateMachine;
    const uint16_t durationMin =
        uiState_.editing() ? uiState_.pendingDuration() : uiState_.testAllZonesDurationMin();

    drawMenuList("TEST ALL ZONES", TestAll::kTestAllItemCount, uiState_.selectedIndex(), [durationMin](uint8_t index, char* buffer, size_t size) {
        switch (index) {
            case TestAll::kTestAllIndexRun:
                snprintf(buffer, size, "Run");
                break;
            case TestAll::kTestAllIndexDuration:
                snprintf(buffer, size, "Dur./zone: %u min", static_cast<unsigned>(durationMin));
                break;
            case TestAll::kTestAllIndexBack:
                snprintf(buffer, size, "BACK");
                break;
            default:
                buffer[0] = '\0';
                break;
        }
    });
}

void IrrigatorUi::renderMenuSchedule(const RenderData& data) {
    const uint8_t recordCount = data.schedule != nullptr ? data.schedule->count : 0;
    constexpr uint8_t kFieldsPerRecord = ::irrigator::domain::UiStateMachine::kScheduleFieldsPerRecord;
    const uint8_t itemCount = ::irrigator::domain::UiStateMachine::scheduleItemCount(recordCount);
    const uint8_t editingIndex = uiState_.selectedIndex();
    const bool editing = uiState_.editing();

    drawMenuList("SCHEDULE",
                itemCount,
                uiState_.selectedIndex(),
                [this, &data, recordCount, editing, editingIndex](uint8_t index, char* buffer, size_t size) {
                    const uint8_t recordIndex = static_cast<uint8_t>(index / kFieldsPerRecord);
                    if (recordIndex >= recordCount) {
                        snprintf(buffer, size, "BACK");
                        return;
                    }
                    const auto& record = data.schedule->records[recordIndex];
                    const uint8_t field = static_cast<uint8_t>(index % kFieldsPerRecord);
                    const unsigned pn = static_cast<unsigned>(recordIndex + 1);
                    const bool isEditingThisRow = editing && editingIndex == index;
                    switch (field) {
                        case ::irrigator::domain::UiStateMachine::kScheduleFieldZone: {
                            const uint8_t zone = isEditingThisRow ? uiState_.pendingScheduleZone() : record.zone;
                            snprintf(buffer, size, "%u:%s", pn, zoneLabel(data, zone));
                            break;
                        }
                        case ::irrigator::domain::UiStateMachine::kScheduleFieldDay: {
                            const uint8_t day = isEditingThisRow ? uiState_.pendingScheduleDay() : record.dayOfWeek;
                            snprintf(buffer, size, "%u:Start %s", pn, dayOfWeekName(day));
                            break;
                        }
                        case ::irrigator::domain::UiStateMachine::kScheduleFieldTime:
                            if (isEditingThisRow) {
                                snprintf(buffer,
                                        size,
                                        "%u:On %02u:%02u%s",
                                        pn,
                                        static_cast<unsigned>(uiState_.pendingScheduleHour()),
                                        static_cast<unsigned>(uiState_.pendingScheduleMinute()),
                                        uiState_.editingScheduleTimeMinuteStep() ? "" : " (hr)");
                            } else {
                                snprintf(buffer, size, "%u:On %s", pn, record.startTime);
                            }
                            break;
                        case ::irrigator::domain::UiStateMachine::kScheduleFieldDuration: {
                            const uint16_t duration = (editing && editingIndex == index)
                                                          ? uiState_.pendingDuration()
                                                          : record.durationMin;
                            snprintf(buffer, size, "%u:For %umin", pn, static_cast<unsigned>(duration));
                            break;
                        }
                        case ::irrigator::domain::UiStateMachine::kScheduleFieldActive:
                            snprintf(buffer, size, "%u:Status: %s", pn, record.active ? "active" : "paused");
                            break;
                        default:
                            buffer[0] = '\0';
                            break;
                    }
                });
}

}  // namespace irrigator::ui
