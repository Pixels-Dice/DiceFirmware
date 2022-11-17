#include "battery_controller.h"
#include "bluetooth/bluetooth_messages.h"
#include "bluetooth/bluetooth_message_service.h"
#include "drivers_hw/battery.h"
#include "config/settings.h"
#include "nrf_log.h"
#include "app_timer.h"
#include "app_error.h"
#include "app_error_weak.h"
#include "die.h"
#include "drivers_nrf/timers.h"
#include "utils/utils.h"
#include "drivers_nrf/a2d.h"
#include "leds.h"
#include "animations\blink.h"

using namespace DriversHW;
using namespace DriversNRF;
using namespace Bluetooth;
using namespace Config;
using namespace Utils;

#define BATTERY_TIMER_MS (3000)	// ms
#define BATTERY_TIMER_MS_QUICK (100) //ms
#define MAX_BATTERY_CLIENTS 2
#define MAX_LEVEL_CLIENTS 2
#define OFF_VCOIL_THRESHOLD (0.2f) //0.2V
#define CHARGE_VCOIL_THRESHOLD (4.6f) //4.6V
#define VBAT_LOOKUP_SIZE 11
#define BATTERY_ALMOST_EMPTY_PCT 0.1f // 10%
#define BATTERY_ALMOST_FULL_PCT 0.9f // 90%

namespace Modules::BatteryController
{
    void getBatteryLevel(void* context, const Message* msg);
    void update(void* context);
    void onBatteryEventHandler(void* context, Battery::ChargingEvent evt);
    void onLEDPowerEventHandler(void* context, bool powerOn);
    float lookupCapacity(float voltage, bool isCharging);
    BatteryState computeState(float batt_capacity, float vcoil, bool is_charging);

    static BatteryState currentBatteryState = BatteryState_Unknown;
    static uint32_t lastUpdateTime = 0;

	static DelegateArray<BatteryStateChangeHandler, MAX_BATTERY_CLIENTS> clients;
    static DelegateArray<BatteryLevelChangeHandler, MAX_LEVEL_CLIENTS> levelClients;

    static float vBat = 0.0f;
    static float vCoil = 0.0f;
    static float capacity = 0.0f;
    static bool charging = false;

	APP_TIMER_DEF(batteryControllerTimer);

    struct VoltageAndLevels
    {
        uint16_t voltageTimes1000;
        uint8_t levelTimes100[2]; // index 0 is when discharging, index 1 is when charging
    };

    // This lookup table defines our voltage to capacity curves, both when charging (values are higher)
    // and discharging (values are lower).
    static const VoltageAndLevels lookup[VBAT_LOOKUP_SIZE] =
    {
        {4100, {100, 100}},
        {4000, {100,  97}},
        {3900, { 93,  88}},
        {3800, { 80,  70}},
        {3700, { 60,  48}},
        {3600, { 33,  14}},
        {3500, { 16,   6}},
        {3400, {  9,   3}},
        {3300, {  5,   2}},
        {3200, {  3,   1}},
        {3000, {  0,   0}},
    };

    void init() {
        MessageService::RegisterMessageHandler(Message::MessageType_RequestBatteryLevel, nullptr, getBatteryLevel);

        // Grab initial values from the battery driver
        vBat = Battery::checkVBat();
        vCoil = Battery::checkVCoil();
        charging = Battery::checkCharging();
        capacity = lookupCapacity(vBat, charging);

        // Register for battery events
        Battery::hook(onBatteryEventHandler, nullptr);

        // Register for led events
        LEDs::hookPowerState(onLEDPowerEventHandler, nullptr);

        // Set initial battery state
        currentBatteryState = computeState(capacity, vCoil, charging);

		ret_code_t ret_code = app_timer_create(&batteryControllerTimer, APP_TIMER_MODE_SINGLE_SHOT, update);
		APP_ERROR_CHECK(ret_code);

		ret_code = app_timer_start(batteryControllerTimer, APP_TIMER_TICKS(BATTERY_TIMER_MS), NULL);
		APP_ERROR_CHECK(ret_code);

        lastUpdateTime = DriversNRF::Timers::millis();

        NRF_LOG_INFO("Battery controller initialized");
        NRF_LOG_INFO("    Battery capacity " NRF_LOG_FLOAT_MARKER, NRF_LOG_FLOAT(capacity * 100));
        // Other values (voltage, vcoil) already displayed by Battery::init()
    }

	BatteryState getCurrentChargeState() {
        return currentBatteryState;
    }

    float getCurrentLevel() {
        return lookupCapacity(vBat, charging);
    }

    BatteryState computeState(float batt_capacity, float vcoil, bool is_charging) {
        BatteryState ret = BatteryState_Unknown;

        // Figure out a battery charge level
        enum CapacityState
        {
            AlmostEmpty,    // Battery is low
            Average,
            AlmostFull
        };
        CapacityState capacityState = CapacityState::Average;
        if (batt_capacity < BATTERY_ALMOST_EMPTY_PCT) {
            capacityState = CapacityState::AlmostEmpty;
        } else if (batt_capacity > BATTERY_ALMOST_FULL_PCT) {
            capacityState = CapacityState::AlmostFull;
        }

        enum CoilState
        {
            NotOnCoil,
            OnCoil_Error,
            OnCoil
        };
        CoilState coilState = CoilState::OnCoil_Error;
        if (vcoil < OFF_VCOIL_THRESHOLD) {
            coilState = CoilState::NotOnCoil;
        } else if (vcoil > CHARGE_VCOIL_THRESHOLD) {
            coilState = CoilState::OnCoil;
        }

        switch (coilState) {
            case CoilState::NotOnCoil:
            default:
                if (is_charging) {
                    // Battery is charging but we're not detecting any coil voltage? How is that possible?
                    NRF_LOG_ERROR("Battery Controller: Not on Coil yet still charging?");
                    ret = BatteryState::BatteryState_Error;
                } else {
                    // Not on charger, not charging, that's perfectly normal, just check the battery level
                    switch (capacityState) {
                        case CapacityState::AlmostEmpty:
                            ret = BatteryState::BatteryState_Low;
                            break;
                        case CapacityState::AlmostFull:
                        case CapacityState::Average:
                        default:
                            ret = BatteryState::BatteryState_Ok;
                            break;
                    }
                }
                break;
            case CoilState::OnCoil:
                if (is_charging) {
                    // On charger and charging, good!
                    ret = BatteryState::BatteryState_Charging;
                } else {
                    // On coil but not charging. It's not necessarily an error if charging hasn't started yet or is complete
                    // So check battery level now.
                    switch (capacityState) {
                        case CapacityState::AlmostEmpty:
                            ret = BatteryState::BatteryState_Low;
                            break;
                        case CapacityState::Average:
                        default:
                            ret = BatteryState::BatteryState_Ok;
                            break;
                        case CapacityState::AlmostFull:
                            // On coil, full and not charging? Probably finished charging
                            ret = BatteryState::BatteryState_Done;
                            break;
                    }
                }
                break;
            case CoilState::OnCoil_Error:
                // Incorrectly placed on coil it seems
                ret = BatteryState::BatteryState_BadCharging;
                break;
        }

        return ret;
    }

    void getBatteryLevel(void* context, const Message* msg) {
        // Fetch battery level
        MessageBatteryLevel lvl;
        lvl.voltage = vBat;
        lvl.level = capacity;
        lvl.charging = currentBatteryState == BatteryState_Charging ? 1 : 0;
        NRF_LOG_INFO("Received Battery Level Request, returning " NRF_LOG_FLOAT_MARKER " (" NRF_LOG_FLOAT_MARKER "v)", NRF_LOG_FLOAT(capacity), NRF_LOG_FLOAT(vBat));
        MessageService::SendMessage(&lvl);
    }

    void update(void* context) {
        // // DEBUG
        // Battery::printA2DReadings();
        // // DEBUG

        // Measure new values
        vBat = Battery::checkVBat();
        vCoil = Battery::checkVCoil();
        charging = Battery::checkCharging();
        capacity = lookupCapacity(vBat, charging);

        auto newState = computeState(capacity, vCoil, charging);
        //if (newState != currentBatteryState)
        {
            switch (newState) {
                case BatteryState_Done:
                    NRF_LOG_INFO("Battery finished charging");
                    break;
                case BatteryState_Ok:
                    NRF_LOG_INFO("Battery is now Ok");
                    break;
                case BatteryState_Charging:
                    NRF_LOG_INFO("Battery is now Charging");
                    break;
                case BatteryState_BadCharging:
                    NRF_LOG_ERROR("Battery is now Charging Transition");
                    break;
                case BatteryState_Low:
                    NRF_LOG_INFO("Battery is Low");
                    break;
                case BatteryState_Error:
                    NRF_LOG_INFO("Battery is in an error state");
                    break;
                default:
                    NRF_LOG_INFO("Battery state is Unknown");
                    break;
            }
            NRF_LOG_INFO("    vBat = " NRF_LOG_FLOAT_MARKER, NRF_LOG_FLOAT(vBat));
            NRF_LOG_INFO("    vCoil = " NRF_LOG_FLOAT_MARKER, NRF_LOG_FLOAT(vCoil));
            NRF_LOG_INFO("    charging = %d", charging);
            NRF_LOG_INFO("    Battery capacity " NRF_LOG_FLOAT_MARKER, NRF_LOG_FLOAT(capacity * 100));

            currentBatteryState = newState;
            for (int i = 0; i < clients.Count(); ++i) {
    			clients[i].handler(clients[i].token, newState);
            }
        }

        for (int i = 0; i < levelClients.Count(); ++i) {
            levelClients[i].handler(levelClients[i].token, capacity);
        }

	    app_timer_start(batteryControllerTimer, APP_TIMER_TICKS(BATTERY_TIMER_MS), NULL);
    }

    void onBatteryEventHandler(void* context, Battery::ChargingEvent evt) {
        update(nullptr);
    }

    void onLEDPowerEventHandler(void* context, bool powerOn) {
        if (powerOn) {
            // Stop reading battery voltage as it may significantly drop when LEDs are turned on
            app_timer_stop(batteryControllerTimer);
        } else {
            app_timer_stop(batteryControllerTimer);

            // If it's been too long since we checked, check right away
            uint32_t delay = BATTERY_TIMER_MS;
            if (DriversNRF::Timers::millis() - lastUpdateTime > BATTERY_TIMER_MS) {
                delay = BATTERY_TIMER_MS_QUICK;
            }
            // Restart the timer
		    app_timer_start(batteryControllerTimer, APP_TIMER_TICKS(delay), NULL);
        }
    }

	/// <summary>
	/// Method used by clients to request timer callbacks when accelerometer readings are in
	/// </summary>
	void hook(BatteryStateChangeHandler callback, void* parameter)
	{
		if (!clients.Register(parameter, callback))
		{
			NRF_LOG_ERROR("Too many battery state hooks registered.");
		}
	}

	/// <summary>
	/// </summary>
	void unHook(BatteryStateChangeHandler callback)
	{
		clients.UnregisterWithHandler(callback);
	}

	/// <summary>
	/// </summary>
	void unHookWithParam(void* param)
	{
		clients.UnregisterWithToken(param);
	}

	/// <summary>
	/// </summary>
	void hookLevel(BatteryLevelChangeHandler callback, void* parameter)
	{
		if (!levelClients.Register(parameter, callback))
		{
			NRF_LOG_ERROR("Too many battery state hooks registered.");
		}
	}

	/// <summary>
	/// </summary>
	void unHookLevel(BatteryLevelChangeHandler callback)
	{
		levelClients.UnregisterWithHandler(callback);
	}

	/// <summary>
	/// </summary>
	void unHookLevelWithParam(void* param)
	{
		levelClients.UnregisterWithToken(param);
	}

    float lookupCapacity(float voltage, bool isCharging)
    {
        // Convert voltage to integer so we can quickly compare it with the lookup table
        int voltageTimes1000 = (int)(voltage * 1000.0f);
        int chargingOffset = isCharging ? 1 : 0;

		// Find the first voltage that is greater than the measured voltage
        // Because voltages are sprted, we know that we can then linearly interpolate the charge level
        // using the previous and next entries in the lookup table.
		int nextIndex = 0;
        while (nextIndex < VBAT_LOOKUP_SIZE && lookup[nextIndex].voltageTimes1000 >= voltageTimes1000) {
            nextIndex++;
        }

		int levelTimes100 = 0;
		if (nextIndex == 0) {
			levelTimes100 = 100;
		} else if (nextIndex == VBAT_LOOKUP_SIZE) {
			levelTimes100 = 0;
		} else {
			// Grab the prev and next keyframes
            auto next = lookup[nextIndex];
            auto prev = lookup[nextIndex - 1];


			// Compute the interpolation parameter
    		int percentTimes1000 = ((int)prev.voltageTimes1000 - (int)voltageTimes1000) * 1000 / ((int)prev.voltageTimes1000 - (int)next.voltageTimes1000);
            levelTimes100 = ((int)prev.levelTimes100[chargingOffset] * (1000 - percentTimes1000) + (int)next.levelTimes100[chargingOffset] * percentTimes1000) / 1000;
		}

		return (float)(levelTimes100) / 100.0f;
    }

}
