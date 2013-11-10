/*
 * Copyright © 2010 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/input.h>
#include <unistd.h>
#include <fcntl.h>

#include "compositor.h"

struct evdev_input {
	struct wlsc_input_device base;
	struct wl_list devices_list;
	struct udev_monitor *udev_monitor;
	char *seat_id;
};

struct evdev_input_device {
	struct evdev_input *master;
	struct wl_list link;
	struct wl_event_source *source;
	struct wlsc_output *output;
	char *devnode;
	int fd;
	struct {
		int min_x, max_x, min_y, max_y;
		int old_x, old_y, reset_x, reset_y;
	} abs;
	int is_touchpad;
};

/* event type flags */
#define EVDEV_ABSOLUTE_MOTION	(1 << 0)
#define EVDEV_RELATIVE_MOTION	(1 << 1)

struct evdev_motion_accumulator {
	int x, y;
	int dx, dy;
	int type; /* event type flags */
};

static inline void
evdev_process_key(struct evdev_input_device *device,
                        struct input_event *e, int time)
{
	if (e->value == 2)
		return;

	switch (e->code) {
	case BTN_TOOL_PEN:
	case BTN_TOOL_RUBBER:
	case BTN_TOOL_BRUSH:
	case BTN_TOOL_PENCIL:
	case BTN_TOOL_AIRBRUSH:
	case BTN_TOOL_FINGER:
	case BTN_TOOL_MOUSE:
	case BTN_TOOL_LENS:
		if (device->is_touchpad)
		{
			device->abs.reset_x = 1;
			device->abs.reset_y = 1;
		}
		break;

	case BTN_TOUCH:
		/* Treat BTN_TOUCH from devices that only have BTN_TOUCH as
		 * BTN_LEFT */
		e->code = BTN_LEFT;
		/* Intentional fallthrough! */
	case BTN_LEFT:
	case BTN_RIGHT:
	case BTN_MIDDLE:
	case BTN_SIDE:
	case BTN_EXTRA:
	case BTN_FORWARD:
	case BTN_BACK:
	case BTN_TASK:
		notify_button(&device->master->base.input_device,
			      time, e->code, e->value);
		break;

	default:
		notify_key(&device->master->base.input_device,
			   time, e->code, e->value);
		break;
	}
}

static inline void
evdev_process_absolute_motion(struct evdev_input_device *device,
		struct input_event *e, struct evdev_motion_accumulator *accum)
{
	const int screen_width = device->output->current->width;
	const int screen_height = device->output->current->height;

	switch (e->code) {
	case ABS_X:
		accum->x = (e->value - device->abs.min_x) * screen_width /
			(device->abs.max_x - device->abs.min_x) +
			device->output->x;
		accum->type = EVDEV_ABSOLUTE_MOTION;
		break;
	case ABS_Y:
		accum->y = (e->value - device->abs.min_y) * screen_height /
			(device->abs.max_y - device->abs.min_y) +
			device->output->y;
		accum->type = EVDEV_ABSOLUTE_MOTION;
		break;
	}
}

static inline void
evdev_process_absolute_motion_touchpad(struct evdev_input_device *device,
		struct input_event *e, struct evdev_motion_accumulator *accum)
{
	/* FIXME: Make this configurable somehow. */
	const int touchpad_speed = 700;

	switch (e->code) {
	case ABS_X:
		e->value -= device->abs.min_x;
		if (device->abs.reset_x)
			device->abs.reset_x = 0;
		else {
			accum->dx = (e->value - device->abs.old_x) *
				touchpad_speed /
				(device->abs.max_x - device->abs.min_x);
		}
		device->abs.old_x = e->value;
		accum->type = EVDEV_RELATIVE_MOTION;
		break;
	case ABS_Y:
		e->value -= device->abs.min_y;
		if (device->abs.reset_y)
			device->abs.reset_y = 0;
		else {
			accum->dy = (e->value - device->abs.old_y) *
				touchpad_speed /
				/* maybe use x size here to have the same scale? */
				(device->abs.max_y - device->abs.min_y);
		}
		device->abs.old_y = e->value;
		accum->type = EVDEV_RELATIVE_MOTION;
		break;
	}
}

static inline void
evdev_process_relative_motion(struct input_event *e,
			      struct evdev_motion_accumulator *accum)
{
	switch (e->code) {
	case REL_X:
		accum->dx += e->value;
		accum->type = EVDEV_RELATIVE_MOTION;
		break;
	case REL_Y:
		accum->dy += e->value;
		accum->type = EVDEV_RELATIVE_MOTION;
		break;
	}
}

static int
is_motion_event(struct input_event *e)
{
	switch (e->type) {
	case EV_REL:
		switch (e->code) {
		case REL_X:
		case REL_Y:
			return 1;
		}
	case EV_ABS:
		switch (e->code) {
		case ABS_X:
		case ABS_Y:
			return 1;
		}
	}

	return 0;
}

static void
evdev_flush_motion(struct wl_input_device *device, uint32_t time,
		   struct evdev_motion_accumulator *accum)
{
	if (accum->type == EVDEV_RELATIVE_MOTION)
		notify_motion(device, time,
			      device->x + accum->dx, device->y + accum->dy);
	if (accum->type == EVDEV_ABSOLUTE_MOTION)
		notify_motion(device, time, accum->x, accum->y);

	memset(accum, 0, sizeof *accum);
}

static int
evdev_input_device_data(int fd, uint32_t mask, void *data)
{
	struct wlsc_compositor *ec;
	struct evdev_input_device *device = data;
	struct input_event ev[8], *e, *end;
	int len;
	struct evdev_motion_accumulator accumulator;
	uint32_t time;

	ec = device->master->base.compositor;
	if (!ec->focus)
		return 1;

	len = read(fd, &ev, sizeof ev);
	if (len < 0 || len % sizeof e[0] != 0) {
		/* FIXME: call device_removed when errno is ENODEV. */;
		return 1;
	}

	memset(&accumulator, 0, sizeof accumulator);

	e = ev;
	end = (void *) ev + len;
	for (e = ev; e < end; e++) {
		time = e->time.tv_sec * 1000 + e->time.tv_usec / 1000;

		/* we try to minimize the amount of notifications to be
		 * forwarded to the compositor, so we accumulate motion
		 * events and send as a bunch */
		if (!is_motion_event(e))
			evdev_flush_motion(&device->master->base.input_device,
					   time, &accumulator);
		switch (e->type) {
		case EV_REL:
			evdev_process_relative_motion(e, &accumulator);
			break;
		case EV_ABS:
			if (device->is_touchpad)
				evdev_process_absolute_motion_touchpad(device,
					e, &accumulator);
			else
				evdev_process_absolute_motion(device, e,
					&accumulator);
			break;
		case EV_KEY:
			evdev_process_key(device, e, time);
			break;
		}
	}

	evdev_flush_motion(&device->master->base.input_device, time,
			   &accumulator);

	return 1;
}


/* copied from udev/extras/input_id/input_id.c */
/* we must use this kernel-compatible implementation */
#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define NBITS(x) ((((x)-1)/BITS_PER_LONG)+1)
#define OFF(x)  ((x)%BITS_PER_LONG)
#define BIT(x)  (1UL<<OFF(x))
#define LONG(x) ((x)/BITS_PER_LONG)
#define TEST_BIT(array, bit)    ((array[LONG(bit)] >> OFF(bit)) & 1)
/* end copied */

static int
evdev_configure_device(struct evdev_input_device *device)
{
	struct input_absinfo absinfo;
	unsigned long ev_bits[NBITS(EV_MAX)];
	unsigned long abs_bits[NBITS(ABS_MAX)];
	unsigned long key_bits[NBITS(KEY_MAX)];
	int has_key, has_abs;

	has_key = 0;
	has_abs = 0;

	ioctl(device->fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits);
	if (TEST_BIT(ev_bits, EV_ABS)) {
		has_abs = 1;

		ioctl(device->fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)),
		      abs_bits);
		if (TEST_BIT(abs_bits, ABS_X)) {
			ioctl(device->fd, EVIOCGABS(ABS_X), &absinfo);
			device->abs.min_x = absinfo.minimum;
			device->abs.max_x = absinfo.maximum;
		}
		if (TEST_BIT(abs_bits, ABS_Y)) {
			ioctl(device->fd, EVIOCGABS(ABS_Y), &absinfo);
			device->abs.min_y = absinfo.minimum;
			device->abs.max_y = absinfo.maximum;
		}
	}
	if (TEST_BIT(ev_bits, EV_KEY)) {
		has_key = 1;
		ioctl(device->fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)),
		      key_bits);
		if (TEST_BIT(key_bits, BTN_TOOL_FINGER) &&
		             !TEST_BIT(key_bits, BTN_TOOL_PEN))
			device->is_touchpad = 1;
	}

	/* This rule tries to catch accelerometer devices and opt out. We may
	 * want to adjust the protocol later adding a proper event for dealing
	 * with accelerometers and implement here accordingly */
	if (has_abs && !has_key)
		return -1;

	return 0;
}

static struct evdev_input_device *
evdev_input_device_create(struct evdev_input *master,
			  struct wl_display *display, const char *path)
{
	struct evdev_input_device *device;
	struct wl_event_loop *loop;
	struct wlsc_compositor *ec;

	device = malloc(sizeof *device);
	if (device == NULL)
		return NULL;

	ec = master->base.compositor;
	device->output =
		container_of(ec->output_list.next, struct wlsc_output, link);

	device->master = master;
	device->is_touchpad = 0;
	device->devnode = strdup(path);

	device->fd = open(path, O_RDONLY);
	if (device->fd < 0)
		goto err0;

	if (evdev_configure_device(device) == -1)
		goto err1;

	loop = wl_display_get_event_loop(display);
	device->source = wl_event_loop_add_fd(loop, device->fd,
					      WL_EVENT_READABLE,
					      evdev_input_device_data, device);
	if (device->source == NULL)
		goto err1;

	wl_list_insert(master->devices_list.prev, &device->link);

	return device;

err1:
	close(device->fd);
err0:
	free(device->devnode);
	free(device);
	return NULL;
}

static const char default_seat[] = "seat0";

static void
device_added(struct udev_device *udev_device, struct evdev_input *master)
{
	struct wlsc_compositor *c;
	const char *devnode;
	const char *device_seat;

	device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");
	if (!device_seat)
		device_seat = default_seat;

	if (strcmp(device_seat, master->seat_id))
		return;

	c = master->base.compositor;
	devnode = udev_device_get_devnode(udev_device);
	if (evdev_input_device_create(master, c->wl_display, devnode))
		fprintf(stderr, "evdev input device: added: %s\n", devnode);
}

static void
device_removed(struct udev_device *udev_device, struct evdev_input *master)
{
	const char *devnode = udev_device_get_devnode(udev_device);
	struct evdev_input_device *device, *next;

	wl_list_for_each_safe(device, next, &master->devices_list, link) {
		if (!strcmp(device->devnode, devnode)) {
			wl_event_source_remove(device->source);
			wl_list_remove(&device->link);
			close(device->fd);
			free(device->devnode);
			free(device);
			break;
		}
	}
	fprintf(stderr, "evdev input device: removed: %s\n", devnode);
}

static int
evdev_udev_handler(int fd, uint32_t mask, void *data)
{
	struct evdev_input *master = data;
	struct udev_device *udev_device;
	const char *action;

	udev_device = udev_monitor_receive_device(master->udev_monitor);
	if (!udev_device)
		return 1;

	action = udev_device_get_action(udev_device);
	if (action) {
		if (strncmp("event", udev_device_get_sysname(udev_device), 5) != 0)
			return 0;

		if (!strcmp(action, "add")) {
			device_added(udev_device, master);
		}
		else if (!strcmp(action, "remove"))
			device_removed(udev_device, master);
	}
	udev_device_unref(udev_device);

	return 0;
}

static int
evdev_config_udev_monitor(struct udev *udev, struct evdev_input *master)
{
	struct wl_event_loop *loop;
	struct wlsc_compositor *c = master->base.compositor;

	master->udev_monitor = udev_monitor_new_from_netlink(udev, "udev");
	if (!master->udev_monitor)
		return 0;

	udev_monitor_filter_add_match_subsystem_devtype(master->udev_monitor,
			"input", NULL);

	if (udev_monitor_enable_receiving(master->udev_monitor)) {
		fprintf(stderr, "udev: failed to bind the udev monitor\n");
		return 0;
	}

	loop = wl_display_get_event_loop(c->wl_display);
	wl_event_loop_add_fd(loop, udev_monitor_get_fd(master->udev_monitor),
			     WL_EVENT_READABLE, evdev_udev_handler, master);

	return 1;
}

void
evdev_input_add_devices(struct wlsc_compositor *c,
			struct udev *udev, const char *seat)
{
	struct evdev_input *input;
	struct udev_enumerate *e;
	struct udev_list_entry *entry;
	struct udev_device *device;
	const char *path;

	input = malloc(sizeof *input);
	if (input == NULL)
		return;

	memset(input, 0, sizeof *input);
	wlsc_input_device_init(&input->base, c);

	wl_list_init(&input->devices_list);
	input->seat_id = strdup(seat);
	if (!evdev_config_udev_monitor(udev, input)) {
		free(input->seat_id);
		free(input);
		return;
	}

	e = udev_enumerate_new(udev);
	udev_enumerate_add_match_subsystem(e, "input");
	udev_enumerate_scan_devices(e);
	udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(e)) {
		path = udev_list_entry_get_name(entry);
		device = udev_device_new_from_syspath(udev, path);

		if (strncmp("event", udev_device_get_sysname(device), 5) != 0)
			continue;

		device_added(device, input);

		udev_device_unref(device);
	}
	udev_enumerate_unref(e);

	c->input_device = &input->base.input_device;
}
