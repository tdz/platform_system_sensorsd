/*
 * Copyright (C) 2015-2016  Mozilla Foundation
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

/*
 * This file contains the implementation of the Poll service for Android
 * sensors.
 *
 * Android sensors are documented at [1] and [2].
 *
 * Android's API mixes up degree and radiant. For consistency, we
 * generally transfer rotations in radiant, which is an SI unit.
 *
 * We also filter out meta-data events, as they are not real sensor
 * events. If necessary, we send notifications for individual meta-
 * data.
 *
 *  [1] https://developer.android.com/guide/topics/sensors/index.html
 *  [2] https://source.android.com/devices/sensors/sensor-types.html
 */

#include "poll.h"

#include <assert.h>
#include <fdio/task.h>
#include <hardware/sensors.h>
#include <math.h>
#include <pdu/pdubuf.h>
#include <pthread.h>
#include <stdlib.h>

#include "compiler.h"
#include "log.h"
#include "memptr.h"
#include "pdu.h"

/*
 * NUM_SENSOR_TYPES sets the number of type values for sensors. You'll
 * have to increase this value to support new sensor types, and update
 * the related arrays in this file.
 */
#if ANDROID_VERSION >= 23
#define NUM_SENSOR_TYPES  (SENSOR_TYPE_WRIST_TILT_GESTURE + 1)
#elif ANDROID_VERSION >= 21
#define NUM_SENSOR_TYPES  (SENSOR_TYPE_PICK_UP_GESTURE + 1)
#elif ANDROID_VERSION >= 18
#define NUM_SENSOR_TYPES  (SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR + 1)
#else
#define NUM_SENSOR_TYPES  (SENSOR_TYPE_AMBIENT_TEMPERATURE + 1)
#endif

#ifndef SENSOR_TYPE_META_DATA
#define SENSOR_TYPE_META_DATA (0)
#endif /* SENSOR_TYPE_META_DATA */

#ifndef SENSOR_TYPE_GEOMAGNETIC_FIELD
#define SENSOR_TYPE_GEOMAGNETIC_FIELD SENSOR_TYPE_MAGNETIC_FIELD
#endif /* SENSOR_TYPE_GEOMAGNETIC_FIELD */

#ifndef SENSORS_DEVICE_API_VERSION_1_3

/*
 * The Android Sensors API before version 1.3 did not support
 * sensor flags. We emulate them by storing the old default
 * values here.
 */

#define SENSOR_FLAG_WAKE_UP                 (0x1)
#define SENSOR_FLAG_CONTINUOUS_MODE         (0x0)
#define SENSOR_FLAG_ON_CHANGE_MODE          (0x2)
#define SENSOR_FLAG_ONE_SHOT_MODE           (0x4)
#define SENSOR_FLAG_SPECIAL_REPORTING_MODE  (0x6)
#define REPORTING_MODE_MASK   (0xe)
#define REPORTING_MODE_SHIFT  (1)

static const uint8_t g_default_sensor_flags[NUM_SENSOR_TYPES] = {
  [SENSOR_TYPE_META_DATA] = 0,
  [SENSOR_TYPE_ACCELEROMETER] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_GEOMAGNETIC_FIELD] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_ORIENTATION] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_GYROSCOPE] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_LIGHT] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_PRESSURE] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_TEMPERATURE] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_PROXIMITY] = SENSOR_FLAG_ON_CHANGE_MODE |
                            SENSOR_FLAG_WAKE_UP,
  [SENSOR_TYPE_GRAVITY]= SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_LINEAR_ACCELERATION] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_ROTATION_VECTOR] = SENSOR_FLAG_CONTINUOUS_MODE,
  [SENSOR_TYPE_RELATIVE_HUMIDITY] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_AMBIENT_TEMPERATURE] = SENSOR_FLAG_ON_CHANGE_MODE,
#if ANDROID_VERSION >= 18
  [SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_GAME_ROTATION_VECTOR] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_GYROSCOPE_UNCALIBRATED] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_SIGNIFICANT_MOTION] = SENSOR_FLAG_ONE_SHOT_MODE |
                                     SENSOR_FLAG_WAKE_UP,
  [SENSOR_TYPE_STEP_DETECTOR] = SENSOR_FLAG_SPECIAL_REPORTING_MODE,
  [SENSOR_TYPE_STEP_COUNTER] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR] = SENSOR_FLAG_CONTINUOUS_MODE,
#endif
#if ANDROID_VERSION >= 21
  [SENSOR_TYPE_HEART_RATE] = SENSOR_FLAG_ON_CHANGE_MODE,
  [SENSOR_TYPE_TILT_DETECTOR] = SENSOR_FLAG_SPECIAL_REPORTING_MODE,
  [SENSOR_TYPE_WAKE_GESTURE] = SENSOR_FLAG_ONE_SHOT_MODE,
  [SENSOR_TYPE_GLANCE_GESTURE] = SENSOR_FLAG_ONE_SHOT_MODE,
  [SENSOR_TYPE_PICK_UP_GESTURE] = SENSOR_FLAG_ONE_SHOT_MODE,
#endif
#if ANDROID_VERSION >= 23
  [SENSOR_TYPE_WRIST_TILT_GESTURE] = SENSOR_FLAG_SPECIAL_REPORTING_MODE
#endif
};
#endif /* SENSORS_DEVICE_API_VERSION_1_3 */

enum {
  /* commands/responses */
  OPCODE_ENABLE_SENSOR = 0x01,
  OPCODE_DISABLE_SENSOR = 0x02,
  OPCODE_SET_PERIOD = 0x03,
  /* notifications */
  OPCODE_ERROR_NTF = 0x80,
  OPCODE_SENSOR_DETECTED_NTF = 0x81,
  OPCODE_SENSOR_LOST_NTF = 0x82,
  OPCODE_EVENT_NTF = 0x83
};

enum {
  IPC_FIELD_SIZE_ACCURACY = 4,
  IPC_FIELD_SIZE_DELIVERY_MODE = 1,
  IPC_FIELD_SIZE_ERROR = 1,
  IPC_FIELD_SIZE_MAX_DELAY = 4,
  IPC_FIELD_SIZE_MIN_DELAY = 4,
  IPC_FIELD_SIZE_POWER_CONSUMPTION = 4,
  IPC_FIELD_SIZE_RANGE = 4,
  IPC_FIELD_SIZE_RESOLUTION = 4,
  IPC_FIELD_SIZE_SENSOR_ID = 4,
  IPC_FIELD_SIZE_TIMESTAMP = 8,
  IPC_FIELD_SIZE_TRIGGER_MODE = 1,
  IPC_FIELD_SIZE_TYPE = 4
};

#define OPCODE_ERROR_NTF_SIZE \
  IPC_FIELD_SIZE_ERROR  /* error code */

#define OPCODE_SENSOR_DETECTED_NTF_SIZE \
  ( IPC_FIELD_SIZE_SENSOR_ID + /* sensor id*/ \
    IPC_FIELD_SIZE_TYPE + /* type */ \
    IPC_FIELD_SIZE_RANGE + /* range */ \
    IPC_FIELD_SIZE_RESOLUTION + /* resolution */ \
    IPC_FIELD_SIZE_POWER_CONSUMPTION + /* power consumption */ \
    IPC_FIELD_SIZE_MIN_DELAY + /* min delay */ \
    IPC_FIELD_SIZE_MAX_DELAY + /* max delay*/ \
    IPC_FIELD_SIZE_TRIGGER_MODE + /* sensor trigger mode */ \
    IPC_FIELD_SIZE_DELIVERY_MODE /* sensor delivery mode */ \
  )

#define OPCODE_EVENT_NTF_SIZE(_data_size) \
  ( IPC_FIELD_SIZE_SENSOR_ID + /* sensor id */ \
    IPC_FIELD_SIZE_TYPE + /* type */ \
    IPC_FIELD_SIZE_TIMESTAMP + /* timestamp */ \
    IPC_FIELD_SIZE_ACCURACY + /* accuracy */ \
    (_data_size) + /* data */ \
    IPC_FIELD_SIZE_DELIVERY_MODE /* sensor delivery mode */ \
  )

static void (*g_send_pdu)(struct pdu_wbuf* wbuf);
static int g_is_registered;
static struct sensors_module_t* g_sensors_module;
static struct sensors_poll_device_t* g_poll_device;
static pthread_t g_poll_thread;

/* |send_pdu| takes a PDU write buffer and hands it over to the send
 * callback. It's save to call this function even if no send callback
 * has been installed.
 */
static void
send_pdu(struct pdu_wbuf* wbuf)
{
  if (!g_send_pdu) {
    ALOGE("g_send_pdu is NULL");
  }

  g_send_pdu(wbuf);
}

/* |send_ntf_pdu| is a variant of |send_ntf_pdu| that checks if the
 * Poll service is still registered. We don't want to send out any
 * notifications after |unregister_poll| has run.
 */
static void
send_ntf_pdu(struct pdu_wbuf* wbuf)
{
  if (!g_is_registered) {
    /* stop sending notifications after unregistering service */
    destroy_pdu_wbuf(wbuf);
  }

  send_pdu(wbuf);
}

/* Task callback for |send_ntf_pdu| */
static enum ioresult
send_ntf_pdu_cb(void* data)
{
  send_ntf_pdu(data);
  return IO_OK;
}

/* |disable_all_sensors| is a helper to start polling with a clean state
 * or clean up when the Poll service switches off.
 */
static void
disable_all_sensors(void)
{
  int len, i;
  const struct sensor_t* sensors;

  assert(g_sensors_module);
  assert(g_poll_device);

  len = g_sensors_module->get_sensors_list(g_sensors_module, &sensors);

  for (i = 0; i < len; ++i) {
    g_poll_device->activate(g_poll_device, sensors[i].handle, 0);
  }
}

/* |deg2radf| takes an angle in degrees and returns its value in radiant. */
static float
deg2radf(float deg)
{
  return M_PI * deg / 180.0;
}

/* |quatlenf| returns the length of a quaternion (q3), or computes the
 * length from q[0,1,2].
 *
 * q3 is optional until Android API level 18. It can be computed from
 * q[0,1,2], but it's better to take the actual value if one is provided.
 *
 * The computation is
 *
 *  v = 1 - (q0 * q0) - (q1 * q1) - (q2 * q2)
 *  q3 = v > 0 ? sqrt(v) : 0;
 *
 * We're assuming that it will be 0 if it's not passed in. (The values
 * form a unit quaternion, so the angle can be computed from the
 * direction vector.)
 */
static float
quatlenf(float q0, float q1, float q2, float q3)
{
  double v;
  if (fpclassify(q3) != FP_ZERO) {
    return q3;
  }

  v = 1.0 - q0 * q0 - q1 * q1 - q2 * q2;

  return v > 0.0 ? sqrt(v) : 0.0;
}

/*
 * Sensors lists
 *
 * Sensorsd maintains a list of known sensors. This is required to work
 * around broken systems that do not include the sensor's type in the
 * generated events. This bug can be observed on emulator targets.
 *
 * You can create a sensors list by calling |create_sensors_list|, which
 * returns a newly allocated list of all available sensors. You are
 * supposed to free the list with a call to |free_sensors_list|. To search
 * for a sensor by id, call |find_sensor_by_id|. It takes a sensors list
 * and the id and returns the sensor's entry in the list. The value of |id|
 * must be less than SENSORS_HANDLE_COUNT. If the sensor doesn't exist, the
 * returned entry's field |handle| will by 0.
 */

static struct sensor_t*
find_sensor_by_id(struct sensor_t* sensors, int32_t id)
{
  assert(id < SENSORS_HANDLE_COUNT);

  return sensors + id - SENSORS_HANDLE_BASE;
}

static struct sensor_t*
create_sensors_list(void)
{
  struct sensor_t* all_sensors;
  const struct sensor_t* sensors;
  int len, i;

  all_sensors = calloc(SENSORS_HANDLE_COUNT, sizeof(*all_sensors));

  if (!all_sensors) {
    ALOGE_ERRNO("calloc");
    return NULL;
  }

  len = g_sensors_module->get_sensors_list(g_sensors_module, &sensors);

  for (i = 0; i < len; ++i) {
    memcpy(find_sensor_by_id(all_sensors, sensors[i].handle), sensors + i,
           sizeof(*all_sensors));
  }

  return all_sensors;
}

static void
free_sensors_list(struct sensor_t* sensors)
{
  free(sensors);
}

/*
 * Protocol helpers
 *
 * These helper functions append sensor-specific data to a PDU. They
 * are build on top of the helpers in libpdu, which pack single data
 * fields.
 *
 * Each helper receives the PDU structure and the sensors value. They
 * return the new data offset in the PDU on success, or -1 otherwise.
 *
 * Calls to |append_sensors_t| take values of |struct sensor_t| and
 * calls to |append_sensor_event_t| take values of |struct sensors_event_t|.
 *
 * Sometimes data structures or their semantics change between Android
 * releases. The helpers try very hard to always send out meaningful
 * values. When you port sensorsd to a new Android release, please ensure
 * that these functions are still up to date. Differences should be handled
 * here instead of changing the IPC protocol.
 *
 * Also try to keep the #ifdefs for Android versions located within
 * these functions.
 */

static long
append_sensors_t(struct pdu* pdu, const struct sensor_t* sensor)
{
  uint8_t trigger, delivery;
  int32_t max_delay;
  uint32_t flags;

#ifdef SENSORS_DEVICE_API_VERSION_1_3
  flags = sensor->flags;
  max_delay = sensor->maxDelay;
#else
  assert(sensor->type < (ssize_t)ARRAY_LENGTH(g_default_sensor_flags));

  flags = g_default_sensor_flags[sensor->type];
  max_delay = INT32_MAX;
#endif


  trigger = (flags & REPORTING_MODE_MASK) >> REPORTING_MODE_SHIFT;
  delivery = !!(flags & SENSOR_FLAG_WAKE_UP);

  return append_to_pdu(pdu, "iIfffiiCC", (int32_t)sensor->handle,
                                         (uint32_t)sensor->type,
                                         (float)sensor->maxRange,
                                         (float)sensor->resolution,
                                         (float)sensor->power,
                                         (int32_t)sensor->minDelay,
                                         max_delay,
                                         trigger,
                                         delivery);
}

static long
append_sensors_event_t(struct pdu* pdu, const struct sensors_event_t* ev)
{
  long off;

  off = append_to_pdu(pdu, "iIl", (int32_t)ev->sensor,
                                  (uint32_t)ev->type,
                                  (int64_t)ev->timestamp);
  if (off < 0) {
    return -1;
  }

  switch (ev->type) {
#if ANDROID_VERSION >= 18
  case SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED:
    /* 6 data values */
    off = append_to_pdu(pdu, "iffffff", (int32_t)SENSOR_STATUS_UNRELIABLE,
                                        ev->uncalibrated_magnetic.x_uncalib,
                                        ev->uncalibrated_magnetic.y_uncalib,
                                        ev->uncalibrated_magnetic.z_uncalib,
                                        ev->uncalibrated_magnetic.x_bias,
                                        ev->uncalibrated_magnetic.y_bias,
                                        ev->uncalibrated_magnetic.z_bias);
    break;
  case SENSOR_TYPE_GYROSCOPE_UNCALIBRATED:
    /* 6 data values */
    off = append_to_pdu(pdu, "iffffff", (int32_t)SENSOR_STATUS_UNRELIABLE,
                                        ev->uncalibrated_gyro.x_uncalib,
                                        ev->uncalibrated_gyro.y_uncalib,
                                        ev->uncalibrated_gyro.z_uncalib,
                                        ev->uncalibrated_gyro.x_bias,
                                        ev->uncalibrated_gyro.y_bias,
                                        ev->uncalibrated_gyro.z_bias);
    break;
#endif
  case SENSOR_TYPE_ROTATION_VECTOR:
#if ANDROID_VERSION >= 18
  case SENSOR_TYPE_GAME_ROTATION_VECTOR:
  case SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR:
#endif
    {
      /* 5 data values */
      off = append_to_pdu(pdu, "iffff", (int32_t)SENSOR_STATUS_UNRELIABLE,
                                        ev->data[0],
                                        ev->data[1],
                                        ev->data[2],
                                        quatlenf(ev->data[0],
                                                 ev->data[1],
                                                 ev->data[2],
                                                 ev->data[3]),
                                        ev->data[4]);
    }
    break;
  case SENSOR_TYPE_ACCELEROMETER:
  case SENSOR_TYPE_GRAVITY:
  case SENSOR_TYPE_LINEAR_ACCELERATION:
    /* 3 data values with status */
    off = append_to_pdu(pdu, "ifff", (int32_t)ev->acceleration.status,
                                     ev->acceleration.x,
                                     ev->acceleration.y,
                                     ev->acceleration.z);
    break;
  case SENSOR_TYPE_GEOMAGNETIC_FIELD:
    /* 3 data values with status */
    off = append_to_pdu(pdu, "ifff", (int32_t)ev->magnetic.status,
                                     ev->magnetic.x,
                                     ev->magnetic.y,
                                     ev->magnetic.z);
    break;
  case SENSOR_TYPE_ORIENTATION:
    /* 3 data values; converted to radiant */
    off = append_to_pdu(pdu, "ifff", (int32_t)ev->orientation.status,
                                     deg2radf(ev->orientation.azimuth),
                                     deg2radf(ev->orientation.pitch),
                                     deg2radf(ev->orientation.roll));
    break;
  case SENSOR_TYPE_GYROSCOPE:
    /* 3 data values */
    off = append_to_pdu(pdu, "ifff", (int32_t)ev->gyro.status,
                                     ev->gyro.azimuth,
                                     ev->gyro.pitch,
                                     ev->gyro.roll);
    break;
  case SENSOR_TYPE_LIGHT:
  case SENSOR_TYPE_PRESSURE:
  case SENSOR_TYPE_TEMPERATURE:
  case SENSOR_TYPE_PROXIMITY:
  case SENSOR_TYPE_RELATIVE_HUMIDITY:
  case SENSOR_TYPE_AMBIENT_TEMPERATURE:
#if ANDROID_VERSION >= 18
  case SENSOR_TYPE_SIGNIFICANT_MOTION:
  case SENSOR_TYPE_STEP_DETECTOR:
#endif
#if ANDROID_VERSION >= 21
  case SENSOR_TYPE_TILT_DETECTOR:
  case SENSOR_TYPE_WAKE_GESTURE:
  case SENSOR_TYPE_GLANCE_GESTURE:
  case SENSOR_TYPE_PICK_UP_GESTURE:
#endif
#if ANDROID_VERSION >= 23
  case SENSOR_TYPE_WRIST_TILT_GESTURE:
#endif
    /* 1 data value */
    off = append_to_pdu(pdu, "if", (int32_t)SENSOR_STATUS_UNRELIABLE,
                                   ev->data[0]);
    break;
#if ANDROID_VERSION >= 19
  case SENSOR_TYPE_STEP_COUNTER:
    /* 1 data value */
    off = append_to_pdu(pdu, "iL", (int32_t)SENSOR_STATUS_UNRELIABLE,
                                   ev->u64.step_counter);
    break;
#elif ANDROID_VERSION >= 18
  case SENSOR_TYPE_STEP_COUNTER:
    /* 1 data value */
    off = append_to_pdu(pdu, "iL", (int32_t)SENSOR_STATUS_UNRELIABLE,
                                   ev->step_counter);
    break;
#endif
#if ANDROID_VERSION >= 21
  case SENSOR_TYPE_HEART_RATE:
    /* 1 data value with status */
    off = append_to_pdu(pdu, "if", (int32_t)ev->heart_rate.status,
                                   ev->heart_rate.bpm);
    break;
#endif
  default:
    return -1;
  }

  if (off < 0) {
    return -1;
  }

  return off;
}

/*
 * Notifications
 *
 * Notifications are sent from sensorsd to its client to inform the client
 * about events in the Sensors driver. There are currently notifications
 * implemented for detected sensors, sensor events, and errors.
 *
 * The IPC protocol also defines a notification for removed sensors, but
 * this is currently not implemented. The Android Sensors API doesn't support
 * hotplugging, so sensors never get removed.
 *
 * The function |sensor_detected_ntf| creates the corresponding notification
 * from a sensor structure; while |event_ntf| creates a notification from a
 * sensor-event structure. The latter function applies a number of workarounds
 * to deal with different versions of Android. Each of these functions expects
 * to run on the poll thread.
 *
 * Finally there's |error_ntf|, which sends out an error notifications. If the
 * client receives this notification, it can try to fix the problem (i.e, free
 * memory), or at least cleanly restart the daemon process. Error notifications
 * are not guaranteed to succeed. If we observe a failure when sending them,
 * we abort the daemon immediately. This prevents error cascades and free's
 * up resources for other components. |error_ntf| takes the error code as
 * parameter, although only ERROR_NOMEM is currently used.
 */

static void
error_ntf(uint8_t error)
{
  struct pdu_wbuf* wbuf;

  wbuf = create_pdu_wbuf(OPCODE_ERROR_NTF_SIZE, 0, NULL);

  if (!wbuf) {
    ALOGE("Could not allocate error PDU; aborting immediately");
    abort();
  }

  init_pdu(&wbuf->buf.pdu, SERVICE_POLL, OPCODE_ERROR_NTF);

  if (append_to_pdu(&wbuf->buf.pdu, "C", error) < 0) {
    ALOGE("Could not setup error PDU; aborting immediately");
    abort();
  }

  if (run_task(send_ntf_pdu_cb, wbuf) < 0) {
    ALOGE("Could not send error PDU; aborting immediately");
    abort();
  }
}

static void
sensor_detected_ntf(const struct sensor_t* sensor)
{
  struct pdu_wbuf* wbuf;

  assert(sensor);

  if (sensor->type >= NUM_SENSOR_TYPES) {
    return; /* Unknown sensor type; ignore */
  }

  wbuf = create_pdu_wbuf(OPCODE_SENSOR_DETECTED_NTF_SIZE, 0, NULL);

  if (!wbuf) {
    error_ntf(ERROR_NOMEM);
    return;
  }

  init_pdu(&wbuf->buf.pdu, SERVICE_POLL, OPCODE_SENSOR_DETECTED_NTF);

  if (append_sensors_t(&wbuf->buf.pdu, sensor) < 0) {
    goto cleanup;
  }

  if (run_task(send_ntf_pdu_cb, wbuf) < 0) {
    goto cleanup;
  }

  return;

cleanup:
  destroy_pdu_wbuf(wbuf);
}

static void
event_ntf(const struct sensors_event_t* ev, uint8_t delivery)
{
  static const uint8_t data_size[NUM_SENSOR_TYPES] = {
    [SENSOR_TYPE_META_DATA] = 0,
    [SENSOR_TYPE_ACCELEROMETER] = 12,
    [SENSOR_TYPE_GEOMAGNETIC_FIELD] = 12,
    [SENSOR_TYPE_ORIENTATION] = 12,
    [SENSOR_TYPE_GYROSCOPE] = 12,
    [SENSOR_TYPE_LIGHT] = 4,
    [SENSOR_TYPE_PRESSURE] = 4,
    [SENSOR_TYPE_TEMPERATURE] = 4,
    [SENSOR_TYPE_PROXIMITY] = 4,
    [SENSOR_TYPE_GRAVITY]= 12,
    [SENSOR_TYPE_LINEAR_ACCELERATION] = 12,
    [SENSOR_TYPE_ROTATION_VECTOR] = 16,
    [SENSOR_TYPE_RELATIVE_HUMIDITY] = 4,
    [SENSOR_TYPE_AMBIENT_TEMPERATURE] = 4,
#if ANDROID_VERSION >= 18
    [SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED] = 24,
    [SENSOR_TYPE_GAME_ROTATION_VECTOR] = 20,
    [SENSOR_TYPE_GYROSCOPE_UNCALIBRATED] = 24,
    [SENSOR_TYPE_SIGNIFICANT_MOTION] = 4,
    [SENSOR_TYPE_STEP_DETECTOR] = 4,
    [SENSOR_TYPE_STEP_COUNTER] = 8,
    [SENSOR_TYPE_GEOMAGNETIC_ROTATION_VECTOR] = 12,
#endif
#if ANDROID_VERSION >= 21
    [SENSOR_TYPE_HEART_RATE] = 4,
    [SENSOR_TYPE_TILT_DETECTOR] = 4,
    [SENSOR_TYPE_WAKE_GESTURE] = 4,
    [SENSOR_TYPE_GLANCE_GESTURE] = 4,
    [SENSOR_TYPE_PICK_UP_GESTURE] = 4,
#endif
#if ANDROID_VERSION >= 23
    [SENSOR_TYPE_WRIST_TILT_GESTURE] = 4
#endif
  };

  struct pdu_wbuf* wbuf;

  assert(ev);

  if (ev->version != sizeof(*ev)) {
    ALOGE("sensor_event_t has invalid size");
    return;
  }

  if (ev->type == SENSOR_TYPE_META_DATA) {
    /* We ignore meta-data events. */
    return;
  }

#if ANDROID_VERSION >= 21
  if (ev->type >= SENSOR_TYPE_DEVICE_PRIVATE_BASE) {
    /* We ignore private events. */
    return;
  }
#endif

  if ((ev->type < 0) || (ev->type >= (ssize_t)ARRAY_LENGTH(data_size))) {
    /* We ignore events of unsupported types. */
    return;
  }

  wbuf = create_pdu_wbuf(OPCODE_EVENT_NTF_SIZE(data_size[ev->type]), 0, NULL);

  if (!wbuf) {
    error_ntf(ERROR_NOMEM);
    return;
  }

  init_pdu(&wbuf->buf.pdu, SERVICE_POLL, OPCODE_EVENT_NTF);

  if ((append_sensors_event_t(&wbuf->buf.pdu, ev) < 0) ||
      (append_to_pdu(&wbuf->buf.pdu, "C", delivery) < 0)) {
    goto cleanup;
  }

  if (run_task(send_ntf_pdu_cb, wbuf) < 0) {
    goto cleanup;
  }

  return;

cleanup:
  destroy_pdu_wbuf(wbuf);
}

/*
 * Commands/Responses
 *
 * Commands are received from the client program and responses are sent
 * 'in response' to commands. There's an opcode for each pair.
 *
 * In the poll module, clients can currently enable and disable sensors
 * with calls to |enable_sensor| and |disable_sensor|, and set a sensor's
 * trigger period with |set_period|.
 *
 * PDUs that have been sent by the client are recevied by a command handler.
 * Picking the correct handler for each service and opcode is implemented by
 * the generic protocol code.
 *
 * The command handler decodes and validates the arguments in the PDU and
 * and executes the operation. Usually this means calling a function of the
 * Android driver.
 *
 * Whatever result the operation returns is packed into a response PDU and
 * sent to the client. Errors are returned from the handler and packed by
 * the protocol framework.
 */

static int
enable_sensor(const struct pdu* cmd)
{
  int32_t id;
  struct pdu_wbuf* wbuf;
  int res;

  assert(g_poll_device);
  assert(g_poll_device->activate);

  if (read_pdu_at(cmd, 0, "i", &id) < 0) {
    return ERROR_PARM_INVALID;
  }

  wbuf = create_pdu_wbuf(0, 0, NULL);

  if (!wbuf) {
    return ERROR_NOMEM;
  }

  res = g_poll_device->activate(g_poll_device, id, 1);

  if (res < 0) {
    ALOGE_ERRNO_NUM("sensors_poll_device_t::activate", -res);
    goto err_poll_device_activate;
  }

  init_pdu(&wbuf->buf.pdu, cmd->service, cmd->opcode);
  send_pdu(wbuf);

  return ERROR_NONE;

err_poll_device_activate:
  destroy_pdu_wbuf(wbuf);
  return ERROR_FAIL;
}

static int
disable_sensor(const struct pdu* cmd)
{
  int32_t id;
  struct pdu_wbuf* wbuf;
  int res;

  assert(g_poll_device);
  assert(g_poll_device->activate);

  if (read_pdu_at(cmd, 0, "i", &id) < 0) {
    return ERROR_PARM_INVALID;
  }

  wbuf = create_pdu_wbuf(0, 0, NULL);

  if (!wbuf) {
    return ERROR_NOMEM;
  }

  res = g_poll_device->activate(g_poll_device, id, 0);

  if (res < 0) {
    ALOGE_ERRNO_NUM("sensors_poll_device_t::activate", -res);
    goto err_poll_device_activate;
  }

  init_pdu(&wbuf->buf.pdu, cmd->service, cmd->opcode);
  send_pdu(wbuf);

  return ERROR_NONE;

err_poll_device_activate:
  destroy_pdu_wbuf(wbuf);
  return ERROR_FAIL;
}

static int
set_period(const struct pdu* cmd)
{
  int32_t id;
  uint64_t period;
  struct pdu_wbuf* wbuf;
  int res;

  assert(g_poll_device);
  assert(g_poll_device->setDelay);

  if (read_pdu_at(cmd, 0, "iL", &id, &period) < 0) {
    return ERROR_PARM_INVALID;
  }

  if (period > INT64_MAX) {
    return ERROR_PARM_INVALID;
  }

  wbuf = create_pdu_wbuf(0, 0, NULL);

  if (!wbuf) {
    return ERROR_NOMEM;
  }

  res = g_poll_device->setDelay(g_poll_device, id, period);

  if (res < 0) {
    ALOGE_ERRNO_NUM("sensors_poll_device_t::setDelay", -res);
    goto err_poll_device_setDelay;
  }

  init_pdu(&wbuf->buf.pdu, cmd->service, cmd->opcode);
  send_pdu(wbuf);

  return ERROR_NONE;

err_poll_device_setDelay:
  destroy_pdu_wbuf(wbuf);
  return ERROR_FAIL;
}

/*
 * Poll module framework
 *
 * The functions below implement the overall framework for the Poll
 * module.
 *
 * Please see the header file for a description of |register_poll| and
 * |unregister_poll|. We cannot easily cleanup Poll completely, so we
 * just set a flag in |unregister_poll| and switch off all sensors. The
 * function's implementation contains related details.
 *
 * In |register_poll| we start Android's Sensors driver and set up our
 * internal resources. Polling for sensor events blocks, so we poll on
 * a separate thread. The code is implemented in |poll_devices|.
 *
 * |poll_devices| polls the Android driver and calls |event_ntf| for each
 * event. It might block forever if there are no sensors events from the
 * driver. It's not supposed to return, except when polling itself fails.
 * Please see the implemenation of |poll_devices| for the workarounds for
 * specific systems.
 *
 * After starting the poll thread in |register_poll|, we need to inform
 * the connected client about available sensors. This is implemented by
 * |detect_sensors|. The function sends out a sensor-detected notification
 * for each available sensor.
 *
 * |detect_sensors| is called from a dispatched task. So |register_poll|
 * will return before it runs. The effect is that clients will *first* get
 * the response message to registering the Poll service, and *then* get
 * the notifications about available sensors.
 *
 * As final step of the registration, |register_poll| returns a pointer to
 * |poll_handler|, the service-handler function for the Poll service.
 */

static void
poll_devices(struct sensor_t* sensors)
{
  /* THE ONLY SENSORS FUNCTION SAFE TO RUN ON THE POLL THREAD IS
   * |sensors_poll_device_t::poll|. All other functions must run
   * concurrently on the I/O thread. We hand-over the list of
   * sensors from the I/O thread, so we don't have to detect them
   * here.
   */

  assert(g_poll_device);
  assert(g_poll_device->poll);

  assert(sensors);

  while (1) {
    sensors_event_t ev[16];
    int res, count, i;

    res = g_poll_device->poll(g_poll_device, ev, ARRAY_LENGTH(ev));

    if (res < 0) {
      if (res == -ENOMEM) {
        /* If we ran out of memory, we try to send a notification and give
         * the client some time to free up some memory. If this fails, the
         * daemon will abort immediately.
         */
        error_ntf(ERROR_NOMEM);
        sleep(30);
        continue;
      } else {
        ALOGE_ERRNO_NUM("sensors_poll_device_t::poll", -res);
        break;
      }
    }
    count = res;

    for (i = 0; i < count; ++i) {
      const struct sensor_t* sensor;
      uint32_t flags;
      uint8_t delivery;

      sensor = find_sensor_by_id(sensors, ev[i].sensor);

#ifdef SENSORS_DEVICE_API_VERSION_1_3
      flags = sensor->flags;
#else
      if (sensor->type >= (ssize_t)ARRAY_LENGTH(g_default_sensor_flags)) {
        continue; /* Unknown sensors type; ignore */
      }
      flags = g_default_sensor_flags[sensor->type];
#endif

      /* Workaround for emulator */
      ev[i].type = sensor->type;

      /* The delivery mode will help clients to manage their wake locks. */
      delivery = !!(flags & SENSOR_FLAG_WAKE_UP);

      event_ntf(ev + i, delivery);
    }
  }
}

/* Pthread callback for |poll_devices| */
static void*
poll_devices_cb(void* arg)
{
  poll_devices(arg);
  free_sensors_list(arg);
  return NULL;
}

static void
detect_sensors(void)
{
  int len, i;
  const struct sensor_t* sensors;

  assert(g_sensors_module);

  len = g_sensors_module->get_sensors_list(g_sensors_module, &sensors);

  for (i = 0; i < len; ++i) {
    sensor_detected_ntf(sensors + i);
  }
}

/* Task callback for |detect_sensors| */
static enum ioresult
detect_sensors_cb(void* arg ATTRIBS(UNUSED))
{
  detect_sensors();
  return IO_OK;
}

static int
poll_handler(const struct pdu* cmd)
{
  static int (* const handler[PDU_MAX_NUM_OPCODES])(const struct pdu*) = {
    [OPCODE_ENABLE_SENSOR] = enable_sensor,
    [OPCODE_DISABLE_SENSOR] = disable_sensor,
    [OPCODE_SET_PERIOD] = set_period
  };

  return handle_pdu_by_opcode(cmd, handler);
}

int
(*register_poll(void (*send_pdu_cb)(struct pdu_wbuf*)))(const struct pdu*)
{
  int err;
  hw_module_t* module;
  struct sensor_t* sensors;

  assert(send_pdu_cb);

  if (g_is_registered) {
    ALOGE("Poll service is already registered");
    return NULL;
  }

  if (!(g_sensors_module && g_poll_device && g_poll_thread)) {

    if (g_sensors_module) {
      ALOGE("Sensors module already loaded");
      return NULL;
    }

    if (g_poll_device) {
      ALOGE("Sensors already opened");
      return NULL;
    }

    err = hw_get_module(SENSORS_HARDWARE_MODULE_ID,
                        (hw_module_t const**)&module);
    if (err) {
      ALOGE_ERRNO_NUM("hw_get_module", err);
      return NULL;
    }

    err = sensors_open(module, &g_poll_device);

    if (err) {
      ALOGE_ERRNO_NUM("sensors_open", err);
      return NULL;
    }

    g_sensors_module = CONTAINER(struct sensors_module_t, module, common);
  }

  g_send_pdu = send_pdu_cb;

  /* On flame-kk, some sensors appear to not be cleared correctly during
   * startup. Sometimes we see events during startup *before* the sensor
   * has been activated. So we disable all sensors explicitly.
   */

  disable_all_sensors();

  /* Sensors should be in a clean state now. Let's start the poll thread.
   * We hand over a list of available sensors: the emulator is broken and
   * returns events without types. We keep a list of sensors on the poll
   * thread and assign the event type from the sensor.
   */

  if (!g_poll_thread) {

    sensors = create_sensors_list();

    if (!sensors) {
      goto err_create_sensors_list;
    }

    err = pthread_create(&g_poll_thread, NULL, poll_devices_cb, sensors);

    if (err) {
      ALOGE_ERRNO_NUM("pthread_create", err);
      goto err_pthread_create;
    }
  }

  if (run_task(detect_sensors_cb, NULL) < 0) {
    return NULL;
  }

  g_is_registered = 1;

  return poll_handler;

err_pthread_create:
  free_sensors_list(sensors);
err_create_sensors_list:
  g_send_pdu = NULL;
  err = sensors_close(g_poll_device);
  if (err) {
    ALOGW_ERRNO_NUM("sensors_close", err);
  }
  g_poll_device = NULL;
  g_sensors_module = NULL;
  return NULL;
}

int
unregister_poll()
{
  disable_all_sensors();

  /* |sensors_poll_device_t::poll| blocks and Android doesn't have
   * |pthread_cancel|, so there's no reliable way for terminating the
   * poll thread. Consequently there's no reliable way for terminating
   * the sensors module. Thus we just set |is_registered| to 0 here.
   * After unregistering the poll service, the client will likely stop
   * the daemon anyway, which will clean up all the resources.
   */
  g_is_registered = 0;

  return 0;
}
