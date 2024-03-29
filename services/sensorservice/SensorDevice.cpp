/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <sys/types.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/Singleton.h>

#include <binder/BinderService.h>
#include <binder/Parcel.h>
#include <binder/IServiceManager.h>

#include <hardware/sensors.h>

#include "SensorDevice.h"
#include "SensorService.h"

#ifdef SYSFS_LIGHT_SENSOR
#include <fcntl.h>
#endif

namespace android {
// ---------------------------------------------------------------------------

ANDROID_SINGLETON_STATIC_INSTANCE(SensorDevice)

#ifdef SYSFS_LIGHT_SENSOR
#define DUMMY_ALS_HANDLE 0xdeadbeef
static ssize_t addDummyLightSensor(sensor_t const **list, ssize_t count) {
    struct sensor_t dummy_light =     {
                  name            : "CyanogenMod dummy light sensor",
                  vendor          : "CyanogenMod",
                  version         : 1,
                  handle          : DUMMY_ALS_HANDLE,
                  type            : SENSOR_TYPE_LIGHT,
                  maxRange        : 20,
                  resolution      : 0.1,
                  power           : 20,
    };
    void * new_list = malloc((count+1)*sizeof(sensor_t));
    new_list = memcpy(new_list, *list, count*sizeof(sensor_t));
    ((sensor_t *)new_list)[count] = dummy_light;
    *list = (sensor_t const *)new_list;
    count++;
    return count;
}
#endif

SensorDevice::SensorDevice()
    :  mSensorDevice(0),
       mSensorModule(0)
{
    status_t err = hw_get_module(SENSORS_HARDWARE_MODULE_ID,
            (hw_module_t const**)&mSensorModule);

    ALOGE_IF(err, "couldn't load %s module (%s)",
            SENSORS_HARDWARE_MODULE_ID, strerror(-err));

    if (mSensorModule) {
        err = sensors_open_1(&mSensorModule->common, &mSensorDevice);

        ALOGE_IF(err, "couldn't open device for module %s (%s)",
                SENSORS_HARDWARE_MODULE_ID, strerror(-err));

        if (mSensorDevice) {
            if (mSensorDevice->common.version == SENSORS_DEVICE_API_VERSION_1_1 ||
                mSensorDevice->common.version == SENSORS_DEVICE_API_VERSION_1_2) {
                ALOGE(">>>> WARNING <<< Upgrade sensor HAL to version 1_3");
            }

            sensor_t const* list;
            ssize_t count = mSensorModule->get_sensors_list(mSensorModule, &list);
#ifdef SYSFS_LIGHT_SENSOR
            count = addDummyLightSensor(&list, count);
#endif
            mActivationCount.setCapacity(count);
            Info model;
            for (size_t i=0 ; i<size_t(count) ; i++) {
                mActivationCount.add(list[i].handle, model);
                mSensorDevice->activate(
                        reinterpret_cast<struct sensors_poll_device_t *>(mSensorDevice),
                        list[i].handle, 0);
            }
        }
    }
}

void SensorDevice::dump(String8& result)
{
    if (!mSensorModule) return;
    sensor_t const* list;
    ssize_t count = mSensorModule->get_sensors_list(mSensorModule, &list);

    result.appendFormat("halVersion %d\n", getHalDeviceVersion());
    result.appendFormat("%d h/w sensors:\n", int(count));

    Mutex::Autolock _l(mLock);
    for (size_t i=0 ; i<size_t(count) ; i++) {
        const Info& info = mActivationCount.valueFor(list[i].handle);
        result.appendFormat("handle=0x%08x, active-count=%zu, batch_period(ms)={ ", list[i].handle,
                            info.batchParams.size());
        for (size_t j = 0; j < info.batchParams.size(); j++) {
            BatchParams params = info.batchParams.valueAt(j);
            result.appendFormat("%4.1f%s", params.batchDelay / 1e6f,
                                j < info.batchParams.size() - 1 ? ", " : "");
        }
        result.appendFormat(" }, selected=%4.1f ms\n", info.bestBatchParams.batchDelay / 1e6f);

        result.appendFormat("handle=0x%08x, active-count=%zu, batch_timeout(ms)={ ", list[i].handle,
                            info.batchParams.size());
        for (size_t j = 0; j < info.batchParams.size(); j++) {
            BatchParams params = info.batchParams.valueAt(j);
            result.appendFormat("%4.1f%s", params.batchTimeout / 1e6f,
                                j < info.batchParams.size() - 1 ? ", " : "");
        }
        result.appendFormat(" }, selected=%4.1f ms\n", info.bestBatchParams.batchTimeout / 1e6f);
    }
}

ssize_t SensorDevice::getSensorList(sensor_t const** list) {
    if (!mSensorModule) return NO_INIT;
    ssize_t count = mSensorModule->get_sensors_list(mSensorModule, list);
#ifdef SYSFS_LIGHT_SENSOR
    return addDummyLightSensor(list, count);
#else
    return count;
#endif
}

status_t SensorDevice::initCheck() const {
    return mSensorDevice && mSensorModule ? NO_ERROR : NO_INIT;
}

ssize_t SensorDevice::poll(sensors_event_t* buffer, size_t count) {
    if (!mSensorDevice) return NO_INIT;
    ssize_t c;
    do {
        c = mSensorDevice->poll(reinterpret_cast<struct sensors_poll_device_t *> (mSensorDevice),
                                buffer, count);
    } while (c == -EINTR);
    return c;
}

void SensorDevice::autoDisable(void *ident, int handle) {
    Info& info( mActivationCount.editValueFor(handle) );
    Mutex::Autolock _l(mLock);
    info.removeBatchParamsForIdent(ident);
}

status_t SensorDevice::activate(void* ident, int handle, int enabled)
{
    if (!mSensorDevice) return NO_INIT;
    status_t err(NO_ERROR);
    bool actuateHardware = false;

    Mutex::Autolock _l(mLock);

#ifdef SYSFS_LIGHT_SENSOR
    if (handle == DUMMY_ALS_HANDLE) {
        int nwr, ret, fd;
        char value[2];

        fd = open(SYSFS_LIGHT_SENSOR, O_RDWR);
        if(fd < 0)
            return -ENODEV;

        nwr = snprintf(value, 2, "%d\n", enabled ? 1 : 0);
        write(fd, value, nwr);
        close(fd);
        return 0;
    }
#endif

    Info& info( mActivationCount.editValueFor(handle) );

    ALOGD_IF(DEBUG_CONNECTIONS,
             "SensorDevice::activate: ident=%p, handle=0x%08x, enabled=%d, count=%zu",
             ident, handle, enabled, info.batchParams.size());

    if (enabled) {
        ALOGD_IF(DEBUG_CONNECTIONS, "enable index=%zd", info.batchParams.indexOfKey(ident));

        if (info.batchParams.indexOfKey(ident) >= 0) {
          if (info.batchParams.size() == 1) {
              // This is the first connection, we need to activate the underlying h/w sensor.
              actuateHardware = true;
          }
        } else {
            // Log error. Every activate call should be preceded by a batch() call.
            ALOGE("\t >>>ERROR: activate called without batch");
        }
    } else {
        ALOGD_IF(DEBUG_CONNECTIONS, "disable index=%zd", info.batchParams.indexOfKey(ident));

        if (info.removeBatchParamsForIdent(ident) >= 0) {
            if (info.batchParams.size() == 0) {
                // This is the last connection, we need to de-activate the underlying h/w sensor.
                actuateHardware = true;
            } else {
                const int halVersion = getHalDeviceVersion();
                if (halVersion >= SENSORS_DEVICE_API_VERSION_1_1) {
                    // Call batch for this sensor with the previously calculated best effort
                    // batch_rate and timeout. One of the apps has unregistered for sensor
                    // events, and the best effort batch parameters might have changed.
                    ALOGD_IF(DEBUG_CONNECTIONS,
                             "\t>>> actuating h/w batch %d %d %" PRId64 " %" PRId64, handle,
                             info.bestBatchParams.flags, info.bestBatchParams.batchDelay,
                             info.bestBatchParams.batchTimeout);
                    mSensorDevice->batch(mSensorDevice, handle,info.bestBatchParams.flags,
                                         info.bestBatchParams.batchDelay,
                                         info.bestBatchParams.batchTimeout);
                }
            }
        } else {
            // sensor wasn't enabled for this ident
        }
    }

    if (actuateHardware) {
        ALOGD_IF(DEBUG_CONNECTIONS, "\t>>> actuating h/w activate handle=%d enabled=%d", handle, enabled);
        err = mSensorDevice->activate(
                reinterpret_cast<struct sensors_poll_device_t *> (mSensorDevice), handle, enabled);
        ALOGE_IF(err, "Error %s sensor %d (%s)", enabled ? "activating" : "disabling", handle,
                 strerror(-err));

        if (err != NO_ERROR && enabled) {
            // Failure when enabling the sensor. Clean up on failure.
            info.removeBatchParamsForIdent(ident);
        }
    }

    // On older devices which do not support batch, call setDelay().
    if (getHalDeviceVersion() < SENSORS_DEVICE_API_VERSION_1_1 && info.batchParams.size() > 0) {
        ALOGD_IF(DEBUG_CONNECTIONS, "\t>>> actuating h/w setDelay %d %" PRId64, handle,
                 info.bestBatchParams.batchDelay);
        mSensorDevice->setDelay(
                reinterpret_cast<struct sensors_poll_device_t *>(mSensorDevice),
                handle, info.bestBatchParams.batchDelay);
    }
    return err;
}

status_t SensorDevice::batch(void* ident, int handle, int flags, int64_t samplingPeriodNs,
                             int64_t maxBatchReportLatencyNs) {
    if (!mSensorDevice) return NO_INIT;

    if (samplingPeriodNs < MINIMUM_EVENTS_PERIOD) {
        samplingPeriodNs = MINIMUM_EVENTS_PERIOD;
    }

    const int halVersion = getHalDeviceVersion();
    if (halVersion < SENSORS_DEVICE_API_VERSION_1_1 && maxBatchReportLatencyNs != 0) {
        // Batch is not supported on older devices return invalid operation.
        return INVALID_OPERATION;
    }

    ALOGD_IF(DEBUG_CONNECTIONS,
             "SensorDevice::batch: ident=%p, handle=0x%08x, flags=%d, period_ns=%" PRId64 " timeout=%" PRId64,
             ident, handle, flags, samplingPeriodNs, maxBatchReportLatencyNs);

    Mutex::Autolock _l(mLock);
    Info& info(mActivationCount.editValueFor(handle));

    if (info.batchParams.indexOfKey(ident) < 0) {
        BatchParams params(flags, samplingPeriodNs, maxBatchReportLatencyNs);
        info.batchParams.add(ident, params);
    } else {
        // A batch has already been called with this ident. Update the batch parameters.
        info.setBatchParamsForIdent(ident, flags, samplingPeriodNs, maxBatchReportLatencyNs);
    }

    BatchParams prevBestBatchParams = info.bestBatchParams;
    // Find the minimum of all timeouts and batch_rates for this sensor.
    info.selectBatchParams();

    ALOGD_IF(DEBUG_CONNECTIONS,
             "\t>>> curr_period=%" PRId64 " min_period=%" PRId64
             " curr_timeout=%" PRId64 " min_timeout=%" PRId64,
             prevBestBatchParams.batchDelay, info.bestBatchParams.batchDelay,
             prevBestBatchParams.batchTimeout, info.bestBatchParams.batchTimeout);

    status_t err(NO_ERROR);
    // If the min period or min timeout has changed since the last batch call, call batch.
    if (prevBestBatchParams != info.bestBatchParams) {
        if (halVersion >= SENSORS_DEVICE_API_VERSION_1_1) {
            ALOGD_IF(DEBUG_CONNECTIONS, "\t>>> actuating h/w BATCH %d %d %" PRId64 " %" PRId64, handle,
                     info.bestBatchParams.flags, info.bestBatchParams.batchDelay,
                     info.bestBatchParams.batchTimeout);
            err = mSensorDevice->batch(mSensorDevice, handle, info.bestBatchParams.flags,
                                       info.bestBatchParams.batchDelay,
                                       info.bestBatchParams.batchTimeout);
        } else {
            // For older devices which do not support batch, call setDelay() after activate() is
            // called. Some older devices may not support calling setDelay before activate(), so
            // call setDelay in SensorDevice::activate() method.
        }
        if (err != NO_ERROR) {
            ALOGE("sensor batch failed %p %d %d %" PRId64 " %" PRId64 " err=%s",
                  mSensorDevice, handle,
                  info.bestBatchParams.flags, info.bestBatchParams.batchDelay,
                  info.bestBatchParams.batchTimeout, strerror(-err));
            info.removeBatchParamsForIdent(ident);
        }
    }
    return err;
}

status_t SensorDevice::setDelay(void* ident, int handle, int64_t samplingPeriodNs)
{
    if (!mSensorDevice) return NO_INIT;
    if (samplingPeriodNs < MINIMUM_EVENTS_PERIOD) {
        samplingPeriodNs = MINIMUM_EVENTS_PERIOD;
    }
    Mutex::Autolock _l(mLock);
    Info& info( mActivationCount.editValueFor(handle) );
    // If the underlying sensor is NOT in continuous mode, setDelay() should return an error.
    // Calling setDelay() in batch mode is an invalid operation.
    if (info.bestBatchParams.batchTimeout != 0) {
      return INVALID_OPERATION;
    }
    ssize_t index = info.batchParams.indexOfKey(ident);
    if (index < 0) {
        return BAD_INDEX;
    }
    BatchParams& params = info.batchParams.editValueAt(index);
    params.batchDelay = samplingPeriodNs;
    info.selectBatchParams();
    return mSensorDevice->setDelay(reinterpret_cast<struct sensors_poll_device_t *>(mSensorDevice),
                                   handle, info.bestBatchParams.batchDelay);
}

int SensorDevice::getHalDeviceVersion() const {
    if (!mSensorDevice) return -1;

    return mSensorDevice->common.version;
}

status_t SensorDevice::flush(void* ident, int handle) {
    if (getHalDeviceVersion() < SENSORS_DEVICE_API_VERSION_1_1) {
        return INVALID_OPERATION;
    }
    ALOGD_IF(DEBUG_CONNECTIONS, "\t>>> actuating h/w flush %d", handle);
    return mSensorDevice->flush(mSensorDevice, handle);
}

// ---------------------------------------------------------------------------

status_t SensorDevice::Info::setBatchParamsForIdent(void* ident, int flags,
                                                    int64_t samplingPeriodNs,
                                                    int64_t maxBatchReportLatencyNs) {
    ssize_t index = batchParams.indexOfKey(ident);
    if (index < 0) {
        ALOGE("Info::setBatchParamsForIdent(ident=%p, period_ns=%" PRId64 " timeout=%" PRId64 ") failed (%s)",
              ident, samplingPeriodNs, maxBatchReportLatencyNs, strerror(-index));
        return BAD_INDEX;
    }
    BatchParams& params = batchParams.editValueAt(index);
    params.flags = flags;
    params.batchDelay = samplingPeriodNs;
    params.batchTimeout = maxBatchReportLatencyNs;
    return NO_ERROR;
}

void SensorDevice::Info::selectBatchParams() {
    BatchParams bestParams(-1, -1, -1);

    if (batchParams.size() > 0) {
        BatchParams params = batchParams.valueAt(0);
        bestParams = params;
    }

    for (size_t i = 1; i < batchParams.size(); ++i) {
        BatchParams params = batchParams.valueAt(i);
        if (params.batchDelay < bestParams.batchDelay) {
            bestParams.batchDelay = params.batchDelay;
        }
        if (params.batchTimeout < bestParams.batchTimeout) {
            bestParams.batchTimeout = params.batchTimeout;
        }
    }
    bestBatchParams = bestParams;
}

ssize_t SensorDevice::Info::removeBatchParamsForIdent(void* ident) {
    ssize_t idx = batchParams.removeItem(ident);
    if (idx >= 0) {
        selectBatchParams();
    }
    return idx;
}

// ---------------------------------------------------------------------------
}; // namespace android

