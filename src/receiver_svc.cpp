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

#include "receiver_svc.h"
#include <binder/IServiceManager.h>
#include <sensorsd/ISensorsReceiverService.h>
#include "log.h"

namespace android {

class BpSensorsReceiverService : public BpInterface<ISensorsReceiverService>
{
public:
  BpSensorsReceiverService(const sp<IBinder>& impl)
    : BpInterface<ISensorsReceiverService>(impl)
  { }

  virtual int32_t handleNotification(const char* buf, size_t len)
  {
      Parcel data, reply;
      data.writeInterfaceToken(ISensorsReceiverService::getInterfaceDescriptor());
      data.writeInt32(len);
      void* outbuf = data.writeInplace(len);
      memcpy(outbuf, buf, len);
      status_t status = remote()->transact(
        BnSensorsReceiverService::HANDLE_NOTIFICATION, data, &reply);
      if (status != NO_ERROR) {
          ALOGD("import() could not contact remote: %d\n", status);
          return -1;
      }
      int32_t err = reply.readExceptionCode();
      if (err < 0) {
          ALOGE("exception %d", err);
          return -1;
      }
      return 0;
  }
};

IMPLEMENT_META_INTERFACE(SensorsReceiverService, SENSORS_RECEIVER_SERVICE_NAME);

} // namespace android

using namespace android;

static sp<ISensorsReceiverService> g_sensors_receiver;

int
init_receiver_service()
{
  sp<IServiceManager> sm = defaultServiceManager();
  sp<IBinder> binder = sm->getService(String16(SENSORS_RECEIVER_SERVICE_NAME));

  ALOGE("got sensor receiver service at %p", (const void*)binder.get());

  g_sensors_receiver = interface_cast<ISensorsReceiverService>(binder);

  ALOGE("got sensor receiver service instance at %p", (const void*)g_sensors_receiver.get());

  return 0;
}

int
send_notification_to_receiver(const char* buf, size_t len)
{
  assert(g_sensors_receiver);

#if 0
  ALOGE("calling sensor receiver service instance at %p", (const void*)g_sensors_receiver.get());
#endif

  g_sensors_receiver->handleNotification(buf, len);

#if 0
  ALOGE("Send sensors notification of %d bytes", (int)len);
#endif

  return 0;
}
