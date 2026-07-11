#include "IrrigatorUi.h"

#include "../drivers/Logger.h"
#include "board_config.h"

namespace irrigator::ui {

namespace {

void drawCentered(U8G2& display, uint8_t y, const char* text) {
    const uint16_t width = display.getStrWidth(text);
    const int16_t x = static_cast<int16_t>((128 - static_cast<int16_t>(width)) / 2);
    display.drawStr(x < 0 ? 0 : x, y, text);
}

}  // namespace

void IrrigatorUi::begin() {
    display_.begin();
    display_.setContrast(40);
    display_.clearBuffer();
    display_.sendBuffer();
    input_.begin();
    ::irrigator::drivers::Log.info("UI initialized (SSD1309 + rotary input)");
}

void IrrigatorUi::showCaptivePortal(const String& apSsid, const IPAddress& ip) {
    display_.clearBuffer();
    display_.setFont(u8g2_font_6x10_tr);
    display_.drawStr(0, 10, "CAPTIVE PORTAL");
    char line[40];
    snprintf(line, sizeof(line), "SSID: %s", apSsid.c_str());
    display_.drawStr(0, 26, line);
    snprintf(line, sizeof(line), "IP: %s", ip.toString().c_str());
    display_.drawStr(0, 40, line);
    display_.sendBuffer();
}

const char* IrrigatorUi::zoneLabel(const RenderData& data, uint8_t zone) const {
    if (zone == 0) {
        return "PUMP";
    }
    if (data.settings != nullptr && zone >= 1 && zone <= irrigator::board::ZONE_COUNT) {
        return data.settings->zoneNames[zone - 1];
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
    ctx.wateringActive = data.snapshot.pumpOn;
    ctx.scheduleActive = data.scheduleActive;
    ctx.scheduleRecordCount = data.schedule != nullptr ? data.schedule->count : 0;
    ctx.wateringPercentage = data.settings != nullptr ? data.settings->wateringPercentage : 0;
    ctx.defaultManualDuration = data.settings != nullptr ? data.settings->defaultManualDuration : 0;

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
        case ::irrigator::domain::UiScreen::DisplayOff:
        default:
            break;
    }
}

void IrrigatorUi::renderNormal(uint32_t nowMs, const RenderData& data) {
    display_.clearBuffer();

    display_.setFont(u8g2_font_6x10_tr);
    char upperLeft[16] = "";
    if (data.deviceIp != IPAddress(0, 0, 0, 0)) {
        snprintf(upperLeft, sizeof(upperLeft), "IP.%u", data.deviceIp[3]);
    }
    display_.drawStr(0, 8, upperLeft);

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
    display_.drawStr(128 - static_cast<int16_t>(rightWidth), 8, upperRight);

    display_.setFont(u8g2_font_helvB18_tr);
    char mainText[20] = "";
    if (data.snapshot.pumpOn) {
        snprintf(mainText, sizeof(mainText), "%s", zoneLabel(data, data.snapshot.activeZone));
    } else if (data.scheduleActive && data.nextScheduledZone != 0) {
        snprintf(mainText, sizeof(mainText), "%s", data.nextScheduledZoneName);
    }
    drawCentered(display_, 40, mainText);

    display_.setFont(u8g2_font_helvB12_tr);
    char statusText[24] = "";
    if (data.snapshot.pumpOn) {
        const uint32_t elapsedMin = data.snapshot.elapsedMs / 60000UL;
        const uint32_t remainingMin = data.snapshot.remainingMs / 60000UL;
        if ((nowMs / 3000UL) % 2 == 0) {
            snprintf(statusText, sizeof(statusText), "WATERING %uM", static_cast<unsigned>(elapsedMin));
        } else {
            snprintf(
                statusText, sizeof(statusText), "REMAINING %uM", static_cast<unsigned>(remainingMin));
        }
    } else if (data.scheduleActive) {
        snprintf(statusText, sizeof(statusText), "SCHEDULED");
    } else {
        snprintf(statusText, sizeof(statusText), "IDLE");
    }
    drawCentered(display_, 54, statusText);

    display_.setFont(u8g2_font_6x10_tr);
    if (data.snapshot.pumpOn) {
        drawCentered(display_, 63, "LONG CLICK = MENU");
    } else {
        drawCentered(display_, 63, "< DECR. o SELECT INCR. >");
    }

    display_.sendBuffer();
}

void IrrigatorUi::renderZonePicker(const RenderData& data) {
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB18_tr);
    drawCentered(display_, 40, zoneLabel(data, uiState_.pendingZone()));
    display_.setFont(u8g2_font_helvB12_tr);
    drawCentered(display_, 54, "SELECT ZONE");
    display_.setFont(u8g2_font_6x10_tr);
    drawCentered(display_, 63, "< DECR. o SELECT INCR. >");
    display_.sendBuffer();
}

void IrrigatorUi::renderDurationPicker(const RenderData& data) {
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB18_tr);
    char text[16];
    snprintf(text, sizeof(text), "%u MINs", static_cast<unsigned>(uiState_.pendingDuration()));
    drawCentered(display_, 40, text);
    display_.setFont(u8g2_font_helvB12_tr);
    drawCentered(display_, 54, zoneLabel(data, uiState_.pendingZone()));
    display_.setFont(u8g2_font_6x10_tr);
    drawCentered(display_, 63, "< DECR. o START INCR. >");
    display_.sendBuffer();
}

void IrrigatorUi::renderStopConfirm(const RenderData& data) {
    (void)data;
    display_.clearBuffer();
    display_.setFont(u8g2_font_helvB12_tr);
    drawCentered(display_, 30, "STOP?");

    const char* noText = "NO";
    const char* yesText = "YES";
    const int16_t noX = 40;
    const int16_t yesX = 80;
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
    const bool wateringActive = data.snapshot.pumpOn;
    const uint8_t itemCount = static_cast<uint8_t>(wateringActive ? 4 : 3);

    drawMenuList(
        "MENU", itemCount, uiState_.selectedIndex(), [this, &data, wateringActive](uint8_t index, char* buffer, size_t size) {
            uint8_t i = index;
            if (wateringActive) {
                if (i == 0) {
                    snprintf(buffer, size, "Stop %s", zoneLabel(data, data.snapshot.activeZone));
                    return;
                }
                --i;
            }
            switch (i) {
                case 0:
                    snprintf(buffer, size, "Schedule");
                    break;
                case 1:
                    snprintf(buffer, size, "Settings");
                    break;
                case 2:
                    snprintf(buffer, size, "BACK");
                    break;
                default:
                    buffer[0] = '\0';
                    break;
            }
        });
}

void IrrigatorUi::renderMenuSettings(const RenderData& data) {
    const int16_t percentage = uiState_.editing()
                                   ? uiState_.pendingPercentage()
                                   : (data.settings != nullptr ? data.settings->wateringPercentage : 0);

    drawMenuList(
        "SETTINGS", 5, uiState_.selectedIndex(), [this, &data, percentage](uint8_t index, char* buffer, size_t size) {
            switch (index) {
                case 0:
                    snprintf(buffer, size, "Adjust: %d %%", percentage);
                    break;
                case 1:
                    snprintf(buffer, size, "Schedule: %s", data.scheduleActive ? "active" : "paused");
                    break;
                case 2:
                    snprintf(buffer, size, "IP: %s", data.deviceIp.toString().c_str());
                    break;
                case 3:
                    snprintf(buffer, size, "MQTT IP: %s", data.mqttBrokerText.c_str());
                    break;
                case 4:
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
    const uint8_t itemCount = static_cast<uint8_t>(recordCount * kFieldsPerRecord + 1);
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
