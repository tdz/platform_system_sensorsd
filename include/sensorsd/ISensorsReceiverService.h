/*
 * Copyright (C) 2015  Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <utils/RefBase.h>
#include <binder/IInterface.h>
#include <binder/Parcel.h>
#include <stddef.h>

#define SENSORS_RECEIVER_SERVICE_NAME "b2g.sensors.receiver"

namespace android {

using namespace android;

class ISensorsReceiverService : public IInterface
{
public:
  enum {
    HANDLE_NOTIFICATION = IBinder::FIRST_CALL_TRANSACTION
  };

  DECLARE_META_INTERFACE(SensorsReceiverService);

  virtual int32_t handleNotification(const char* buf, size_t len) = 0;
};


class BnSensorsReceiverService : public BnInterface<ISensorsReceiverService>
{
public:
  virtual status_t onTransact(uint32_t code, const Parcel& data, Parcel* reply,
                              uint32_t flags = 0);
};

} // namespace android
