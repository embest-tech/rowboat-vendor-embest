/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <cutils/log.h>

#include "AccelSensor.h"

#define FETCH_FULL_EVENT_BEFORE_RETURN 1
/* default poll interval is 50 ms */
#define DEFAULT_POLL_INTERVAL 50

int prev_delay_ms = 0;
/* To handle am335x accelerometer which is an input_polled_device in kernel.
 *
 * Whenever disabled, the last known poll-delay is saved. This is done so as
 * to be able to restore the poll-delay when enabled. This is required as we
 * are using "poll" sys-fs entry for setting the rate as well as
 * enabling/disabling the sensor.
 *
 * For detailed explanation refer the AccelSensor::enable() comments below.
 */
#define ACCEL_PATH "/sys/bus/i2c/drivers/lis3lv02d_i2c/2-0018/"
#define ACCEL_NAME "ST LIS3LV02DL Accelerometer"
#define ACCEL_POLL "poll"

/*****************************************************************************/

AccelSensor::AccelSensor()
    : SensorBase(NULL, ACCEL_NAME),
      mEnabled(0),
      mInputReader(4),
      mHasPendingEvent(false)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_A;
    mPendingEvent.type = SENSOR_TYPE_ACCELEROMETER;
    memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));

    if (data_fd) {
        strcpy(input_sysfs_path, "/sys/class/input/");
        strcat(input_sysfs_path, input_name);
        strcat(input_sysfs_path, "/device/");
        input_sysfs_path_len = strlen(input_sysfs_path);
        enable(0, 1);
    }
}

AccelSensor::~AccelSensor() {
    if (mEnabled) {
        enable(0, 0);
    }
}

int AccelSensor::setInitialState() {
    struct input_absinfo absinfo_x;
    struct input_absinfo absinfo_y;
    struct input_absinfo absinfo_z;
    float value;
    if (!ioctl(data_fd, EVIOCGABS(EVENT_TYPE_ACCEL_X), &absinfo_x) &&
        !ioctl(data_fd, EVIOCGABS(EVENT_TYPE_ACCEL_Y), &absinfo_y) &&
        !ioctl(data_fd, EVIOCGABS(EVENT_TYPE_ACCEL_Z), &absinfo_z)) {
        value = absinfo_y.value;
        mPendingEvent.data[0] = value * CONVERT_A_X;
        value = absinfo_x.value;
        mPendingEvent.data[1] = value * CONVERT_A_Y;
        value = absinfo_z.value;
        mPendingEvent.data[2] = value * CONVERT_A_Z;
        mHasPendingEvent = true;
    }
    return 0;
}

int AccelSensor::enable(int32_t, int en) {

/* am335x accel sensor is lis3lv02d which is a input_ polled_device
 *
 * The input_polled_device starts pumping out input events to userspace as
 * soon as anyone opens a fd on the input device. i.e. no waiting for enable=1.
 *
 * 1. Support for "poll_delay" (using existing "poll") in sys-fs.
 * 2. "Disable" is done by poll-delay=0. "Enable" is a non-zero poll-delay.
 *    i.e. when "en"=0, set "poll"=0 disables reporting sensor data.
 * */

    int flags = en ? 1 : 0;
    if (flags != mEnabled) {
        int fd;
        strcpy(&input_sysfs_path[input_sysfs_path_len], ACCEL_POLL);
        fd = open(input_sysfs_path, O_RDWR);
        if (fd >= 0) {
            char buf[80];
            if (flags) {
				/* if delay is zero and device is getting enable then set default to 50 ms*/
				prev_delay_ms = prev_delay_ms !=0 ? prev_delay_ms:DEFAULT_POLL_INTERVAL;
                sprintf(buf, "%d", prev_delay_ms);
                setInitialState();
            } else {
                sprintf(buf, "%d", 0);
            }
            write(fd, buf, strlen(buf)+1);
            close(fd);
			mEnabled = flags;
            return 0;
        }
        return -1;
    }

    return 0;
}

bool AccelSensor::hasPendingEvents() const {
    return mHasPendingEvent;
}

int AccelSensor::setDelay(int32_t handle, int64_t delay_ns)
{
    int fd;
	if(!mEnabled){
		int32_t delay_ms = (int32_t)delay_ns/1000000;
		prev_delay_ms = delay_ms;
		return 0;
	}
    strcpy(&input_sysfs_path[input_sysfs_path_len], ACCEL_POLL);
    fd = open(input_sysfs_path, O_RDWR);
    if (fd >= 0) {
        char buf[80];
        int32_t delay_ms = (int32_t)delay_ns/1000000;
        sprintf(buf, "%d", delay_ms);
        write(fd, buf, strlen(buf)+1);
        prev_delay_ms = delay_ms;
        close(fd);
        return 0;
    }
    return -1;
}

int AccelSensor::readEvents(sensors_event_t* data, int count)
{
    if (count < 1)
        return -EINVAL;

    if (mHasPendingEvent) {
        mHasPendingEvent = false;
        mPendingEvent.timestamp = getTimestamp();
        *data = mPendingEvent;
        return mEnabled ? 1 : 0;
    }

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;

    int numEventReceived = 0;
    input_event const* event;

#if FETCH_FULL_EVENT_BEFORE_RETURN
again:
#endif
    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_ABS) {
            float value = event->value;
            if (event->code == EVENT_TYPE_ACCEL_Y) {
                mPendingEvent.data[0] =  (value * CONVERT_A_Y);
            } else if (event->code == EVENT_TYPE_ACCEL_X) {
                mPendingEvent.data[1] = -(value * CONVERT_A_X);
            } else if (event->code == EVENT_TYPE_ACCEL_Z) {
                mPendingEvent.data[2] = (value * CONVERT_A_Z);
            }
        } else if (type == EV_SYN) {
            mPendingEvent.timestamp = timevalToNano(event->time);
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }

        } else {
	    LOGE("AccelSensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }

#if FETCH_FULL_EVENT_BEFORE_RETURN
    /* if we didn't read a complete event, see if we can fill and
       try again instead of returning with nothing and redoing poll. */
    if (numEventReceived == 0 && mEnabled == 1) {
        n = mInputReader.fill(data_fd);
        if (n)
            goto again;
    }
#endif

    return numEventReceived;
}

