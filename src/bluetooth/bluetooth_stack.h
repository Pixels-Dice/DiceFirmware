#pragma once

#include "stdint.h"

namespace Bluetooth
{
    namespace Stack
    {
        void init();
        void initAdvertising();
        void updateCustomAdvertisingData(uint8_t* data, uint16_t size);
        void disconnect();
        void startAdvertising();
        void disableAdvertisingOnDisconnect();
        void enableAdvertisingOnDisconnect();
        bool canSend();
        void resetOnDisconnect();

        enum SendResult
        {
            SendResult_Ok = 0,
            SendResult_Busy,            // Should try again later
            SendResult_NotConnected,    // No connection
            SendResult_Error,           // Any other error
        };

        SendResult send(uint16_t handle, const uint8_t* data, uint16_t len);
        void slowAdvertising();
        void stopAdvertising();
        bool isConnected();

        typedef void(*ConnectionEventMethod)(void* param, bool connected);
        void hook(ConnectionEventMethod method, void* param);
        void unHook(ConnectionEventMethod client);
        void unHookWithParam(void* param);

        typedef void(*RssiEventMethod)(void* param, int8_t rssi, uint8_t channelIndex);
        void hookRssi(RssiEventMethod method, void* param);
        void unHookRssi(RssiEventMethod client);
    }
}