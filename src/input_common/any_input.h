// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "common/input.h"
#include "input_common/main.h"

namespace InputCommon {

class AnyInputFactory final : public Common::Input::Factory<Common::Input::InputDevice> {
public:
    using DeviceList = std::function<std::vector<Common::ParamPackage>()>;
    using Buttons = std::function<ButtonMapping(const Common::ParamPackage&)>;
    using Analogs = std::function<AnalogMapping(const Common::ParamPackage&)>;
    using Motions = std::function<MotionMapping(const Common::ParamPackage&)>;

    AnyInputFactory(DeviceList devices, Buttons buttons, Analogs analogs, Motions motions);
    ~AnyInputFactory() override;
    std::unique_ptr<Common::Input::InputDevice> Create(const Common::ParamPackage& params) override;
    void Pump();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace InputCommon
