// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#include "ble_wake.h"
#include "share_win.h"

#include <atomic>
#include <random>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

namespace lares {

using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using winrt::Windows::Storage::Streams::DataWriter;

struct BleWake::Impl {
    BluetoothLEAdvertisementPublisher publisher{ nullptr };
    winrt::event_token token{};
    std::atomic<BleWakeState> state{ BleWakeState::Off };
    bool active = false;
};

BleWake::BleWake() : impl_(new Impl) {}

BleWake::~BleWake()
{
    set_active(false);
    delete impl_;
}

BleWakeState BleWake::state() const
{
    return impl_->state.load();
}

void BleWake::set_active(bool on)
{
    if (on == impl_->active) return;
    impl_->active = on;
    try {
        if (!on) {
            if (impl_->publisher) {
                impl_->publisher.StatusChanged(impl_->token);
                impl_->publisher.Stop();
                impl_->publisher = nullptr;
            }
            impl_->state = BleWakeState::Off;
            return;
        }
        /* A new tag each time, as the advertisement on Linux has. */
        std::random_device rd;
        uint32_t r = rd();
        const uint8_t tag[3] = { static_cast<uint8_t>(r), static_cast<uint8_t>(r >> 8), static_cast<uint8_t>(r >> 16) };
        uint8_t data[kAirdropWakeBytes];
        airdrop_wake_data(tag, data);

        BluetoothLEManufacturerData apple;
        apple.CompanyId(kAppleCompanyId);
        DataWriter writer;
        writer.WriteBytes(data);
        apple.Data(writer.DetachBuffer());

        BluetoothLEAdvertisementPublisher publisher;
        publisher.Advertisement().ManufacturerData().Append(apple);
        Impl* impl = impl_;
        impl_->token = publisher.StatusChanged([impl](const BluetoothLEAdvertisementPublisher&,
                                                      const BluetoothLEAdvertisementPublisherStatusChangedEventArgs& e) {
            switch (e.Status()) {
            case BluetoothLEAdvertisementPublisherStatus::Started: impl->state = BleWakeState::On; break;
            case BluetoothLEAdvertisementPublisherStatus::Aborted: impl->state = BleWakeState::Failed; break;
            default: break;
            }
        });
        impl_->state = BleWakeState::Starting;
        publisher.Start();
        impl_->publisher = publisher;
    } catch (const winrt::hresult_error&) {
        impl_->publisher = nullptr;
        impl_->state = on ? BleWakeState::Failed : BleWakeState::Off;
    }
}

} // namespace lares
