// Copyright 2021 The Dawn Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "dawn_wire/client/Adapter.h"

namespace dawn_wire { namespace client {

    bool Adapter::GetLimits(WGPUSupportedLimits* limits) const {
        UNREACHABLE();
    }

    void Adapter::GetProperties(WGPUAdapterProperties* properties) const {
        UNREACHABLE();
    }

    bool Adapter::HasFeature(WGPUFeatureName feature) const {
        UNREACHABLE();
    }

    void Adapter::RequestDevice(const WGPUDeviceDescriptor* descriptor,
                                WGPURequestDeviceCallback callback,
                                void* userdata) {
        callback(WGPURequestDeviceStatus_Error, nullptr, "Not implemented", nullptr);
    }

}}  // namespace dawn_wire::client