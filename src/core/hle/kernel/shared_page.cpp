// Copyright 2015 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cstring>
#include "common/archives.h"
#include "common/assert.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/shared_page.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/movie.h"
#include "core/settings.h"

////////////////////////////////////////////////////////////////////////////////////////////////////

SERIALIZE_EXPORT_IMPL(SharedPage::Handler)

namespace boost::serialization {

template <class Archive>
void load_construct_data(Archive& ar, SharedPage::Handler* t, const unsigned int) {
    ::new (t) SharedPage::Handler(Core::System::GetInstance().CoreTiming());
}
template void load_construct_data<iarchive>(iarchive& ar, SharedPage::Handler* t,
                                            const unsigned int);

} // namespace boost::serialization

namespace SharedPage {

static std::chrono::seconds GetInitTime() {
    const u64 override_init_time = Core::Movie::GetInstance().GetOverrideInitTime();
    if (override_init_time != 0) {
        // Override the clock init time with the one in the movie
        return std::chrono::seconds(override_init_time);
    }

    switch (Settings::values.init_clock) {
    case Settings::InitClock::SystemTime: {
        auto now = std::chrono::system_clock::now();
        // If the system time is in daylight saving, we give an additional hour to console time
        std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);
        std::tm* now_tm = std::localtime(&now_time_t);
        if (now_tm && now_tm->tm_isdst > 0)
            now = now + std::chrono::hours(1);
        return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch());
    }
    case Settings::InitClock::FixedTime:
        return std::chrono::seconds(Settings::values.init_time);
    default:
        UNREACHABLE_MSG("Invalid InitClock value ({})",
                        static_cast<u32>(Settings::values.init_clock));
    }
}

Handler::Handler(Core::Timing& timing) : timing(timing) {
    std::memset(&shared_page, 0, sizeof(shared_page));

    shared_page.running_hw = 0x1; // product

    // Some games wait until this value becomes 0x1, before asking running_hw
    shared_page.unknown_value = 0x1;

    // Set to a completely full battery
    shared_page.battery_state.charge_level.Assign(
        static_cast<u8>(Service::PTM::ChargeLevels::CompletelyFull));
    shared_page.battery_state.is_adapter_connected.Assign(1);
    shared_page.battery_state.is_charging.Assign(1);

    init_time = GetInitTime();

    using namespace std::placeholders;
    // update_time_event = timing.RegisterEvent("SharedPage::UpdateTimeCallback",
    //                                          std::bind(&Handler::UpdateTimeCallback, this, _1,
    //                                          _2));
    timing.ScheduleEvent(this, Ticks(0));

    float slidestate = Settings::values.factor_3d / 100.0f;
    shared_page.sliderstate_3d = static_cast<float_le>(slidestate);
}

/// Gets system time in 3DS format. The epoch is Jan 1900.
std::chrono::milliseconds Handler::GetSystemTime() const {
    const auto powered_on_time = std::chrono::duration_cast<milliseconds>(timing.Time_LB());
    // init_time is since 1970-01-01; 3DS uses since 1900-01-01
    const std::chrono::hours diff_1900_1970{24 * 25567};
    auto now = diff_1900_1970 + init_time + powered_on_time;

    // 3DS system does't allow user to set a time before Jan 1 2000,
    // so we can clamp the reported time to the duration of 1900-2000
    const std::chrono::hours minimum_system_time{24 * 36524};
    if (now < minimum_system_time) {
        return minimum_system_time;
    } else {
        return now;
    }
}

const std::string& Handler::Name() const {
    static const std::string name = "SharedPage::UpdateTimeCallback";
    return name;
}

void Handler::Execute(Core::Timing& parent, u64 userdata, Ticks cycles_late) {
    DateTime& date_time =
        shared_page.date_time_counter % 2 ? shared_page.date_time_0 : shared_page.date_time_1;

    date_time.date_time = GetSystemTime().count();
    date_time.update_tick = timing.Time_LB().count();
    date_time.tick_to_second_coefficient = BASE_CLOCK_RATE_ARM11;
    date_time.tick_offset = 0;

    ++shared_page.date_time_counter;

    // system time is updated hourly
    timing.ScheduleEvent(this, Ticks(std::chrono::hours(1)) - cycles_late);
}

void Handler::SetMacAddress(const MacAddress& addr) {
    std::memcpy(shared_page.wifi_macaddr, addr.data(), sizeof(MacAddress));
}

void Handler::SetWifiLinkLevel(WifiLinkLevel level) {
    shared_page.wifi_link_level = static_cast<u8>(level);
}

void Handler::Set3DLed(u8 state) {
    shared_page.ledstate_3d = state;
}

void Handler::Set3DSlider(float slidestate) {
    shared_page.sliderstate_3d = static_cast<float_le>(slidestate);
}

SharedPageDef& Handler::GetSharedPage() {
    return shared_page;
}

} // namespace SharedPage
