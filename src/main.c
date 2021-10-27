/* Copyright © 2019 Raheman Vaiya.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef __FreeBSD__
#include <dev/evdev/input.h>
#include <dev/evdev/uinput.h>
#else
#include <linux/input.h>
#include <linux/uinput.h>
#endif

#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/file.h>
#include <dirent.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <libudev.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <fcntl.h>
#include "keys.h"
#include "config.h"

#define VIRTUAL_KEYBOARD_NAME "keyd virtual keyboard"
#define VIRTUAL_POINTER_NAME "keyd virtual pointer"
#define IS_MOUSE_BTN(code) (((code) >= BTN_LEFT && (code) <= BTN_TASK) ||\
			    ((code) >= BTN_0 && (code) <= BTN_9))
#define MAX_KEYBOARDS 256
#define TAPPING_TERM 200
#define TAPPING_TERM_ENABLED

#define dbg(fmt, ...) { if(debug) warn("%s:%d: "fmt, __FILE__, __LINE__, ## __VA_ARGS__); }

static int debug = 0;
static int vkbd = -1;
static int vptr = -1;

static struct udev *udev;
static struct udev_monitor *udevmon;
static uint8_t keystate[KEY_CNT] = {0};

//Active keyboard state.
struct keyboard {
	int fd;
	char devnode[256];

	struct layer **layers;
	size_t nlayers;

	//The layer to which modifiers are applied,
	//this may be distinct from the main layout
	struct layer *modlayout;
	struct layer *layout;

	struct keyboard *next;
};

static struct keyboard *keyboards = NULL;

static void warn(char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
}

static void _die(char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);

	vfprintf(stderr, fmt, args);
	va_end(args);
	fprintf(stderr, "\n");
	exit(-1);
}

#define die(fmt, ...) _die("%s:%d: "fmt, __FILE__, __LINE__, ## __VA_ARGS__)

static uint64_t get_time()
{
	struct timespec tv;
	clock_gettime(CLOCK_MONOTONIC, &tv);
	return (tv.tv_sec*1E9)+tv.tv_nsec;
}

static uint64_t get_taping_term(){
	return TAPPING_TERM*1E6;
}

int in_tapping_term(uint64_t from){
	#ifndef TAPPING_TERM_ENABLED
		return 1;
	#else
		return (abs(get_time() - from) <= get_taping_term());
	#endif
}

static int is_keyboard(struct udev_device *dev)
{
	const char *path = udev_device_get_devnode(dev);
	if(!path || !strstr(path, "event")) //Filter out non evdev devices.
		return 0;

	struct udev_list_entry *prop;
	udev_list_entry_foreach(prop, udev_device_get_properties_list_entry(dev)) {
		if(!strcmp(udev_list_entry_get_name(prop), "ID_INPUT_KEYBOARD") &&
		   !strcmp(udev_list_entry_get_value(prop), "1")) {
			return 1;
		}
	}

	return 0;
}

static const char *evdev_device_name(const char *devnode)
{
	static char name[256];

	int fd = open(devnode, O_RDONLY);
	if(fd < 0) {
		perror("open");
		exit(-1);
	}

	if(ioctl(fd, EVIOCGNAME(sizeof(name)), &name) == -1)
		return NULL;

	close(fd);
	return name;
}

static void get_keyboard_nodes(char *nodes[MAX_KEYBOARDS], int *sz)
{
	struct udev *udev;
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *ent;

	udev = udev_new();
	if (!udev)
		die("Cannot create udev context.");

	enumerate = udev_enumerate_new(udev);
	if (!enumerate)
		die("Cannot create enumerate context.");

	udev_enumerate_add_match_subsystem(enumerate, "input");
	udev_enumerate_scan_devices(enumerate);

	devices = udev_enumerate_get_list_entry(enumerate);
	if (!devices)
		die("Failed to get device list.");

	*sz = 0;
	udev_list_entry_foreach(ent, devices) {
		const char *name = udev_list_entry_get_name(ent);;
		struct udev_device *dev = udev_device_new_from_syspath(udev, name);
		const char *path = udev_device_get_devnode(dev);

		if(is_keyboard(dev)) {
			dbg("Detected keyboard node %s (%s)", name, evdev_device_name(path));
			nodes[*sz] = malloc(strlen(path)+1);
			strcpy(nodes[*sz], path);
			(*sz)++;
			assert(*sz <= MAX_KEYBOARDS);
		} else if(path) {
			dbg("Ignoring %s (%s)", evdev_device_name(path), path);
		}

		udev_device_unref(dev);
	}

	udev_enumerate_unref(enumerate);
	udev_unref(udev);
}

static int create_virtual_pointer() 
{
	size_t i;
	uint16_t code;
	struct uinput_setup usetup;

	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if(fd < 0) {
		perror("open");
		exit(-1);
	}

	ioctl(fd, UI_SET_EVBIT, EV_REL);
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);

	ioctl(fd, UI_SET_RELBIT, REL_X);
	ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
	ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
	ioctl(fd, UI_SET_RELBIT, REL_Y);
	ioctl(fd, UI_SET_RELBIT, REL_Z);

	for(code = BTN_LEFT;code <= BTN_TASK;code++)
		ioctl(fd, UI_SET_KEYBIT, code);

	for(code = BTN_0;code <= BTN_9;code++)
		ioctl(fd, UI_SET_KEYBIT, code);

	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x1234;
	usetup.id.product = 0x567a;
	strcpy(usetup.name, VIRTUAL_POINTER_NAME);

	ioctl(fd, UI_DEV_SETUP, &usetup);
	ioctl(fd, UI_DEV_CREATE);

	return fd;
}

static int create_virtual_keyboard()
{
	size_t i;
	struct uinput_setup usetup;

	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if(fd < 0) {
		perror("open");
		exit(-1);
	}

	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_SYN);

	for(i = 0;i < KEY_MAX;i++) {
		if(keycode_table[i].name && !IS_MOUSE_BTN(i) )
			ioctl(fd, UI_SET_KEYBIT, i);
	}

	memset(&usetup, 0, sizeof(usetup));
	usetup.id.bustype = BUS_USB;
	usetup.id.vendor = 0x1234;
	usetup.id.product = 0x567a;
	strcpy(usetup.name, VIRTUAL_KEYBOARD_NAME);

	ioctl(fd, UI_DEV_SETUP, &usetup);
	ioctl(fd, UI_DEV_CREATE);

	return fd;
}

static void syn(int fd)
{
	static struct input_event ev = {
		.type = EV_SYN,
		.code = 0,
		.value = 0,
	};

	write(fd, &ev, sizeof(ev));
}

static void send_repetitions()
{
	size_t i;
	struct input_event ev = {
		.type = EV_KEY,
		.value = 2,
		.time.tv_sec = 0,
		.time.tv_usec = 0
	};

	//Inefficient, but still reasonably fast (<100us)
	for(i = 0; i < sizeof keystate / sizeof keystate[0];i++) {
		if(keystate[i]) {
			ev.code = i;
			write(vkbd, &ev, sizeof(ev));
			syn(vkbd);
		}
	}
}

static void send_key(uint16_t code, int is_pressed)
{
	if(code == KEY_NOOP)
		return;

	keystate[code] = is_pressed;
	struct input_event ev;

	ev.type = EV_KEY;
	ev.code = code;
	ev.value = is_pressed;
	ev.time.tv_sec = 0;
	ev.time.tv_usec = 0;

	write(vkbd, &ev, sizeof(ev));

	syn(vkbd);
}

static void setmods(uint16_t mods)
{
	if(!!(mods & MOD_CTRL) != keystate[KEY_LEFTCTRL])
		send_key(KEY_LEFTCTRL, !keystate[KEY_LEFTCTRL]);
	if(!!(mods & MOD_SHIFT) != keystate[KEY_LEFTSHIFT])
		send_key(KEY_LEFTSHIFT, !keystate[KEY_LEFTSHIFT]);
	if(!!(mods & MOD_SUPER) != keystate[KEY_LEFTMETA])
		send_key(KEY_LEFTMETA, !keystate[KEY_LEFTMETA]);
	if(!!(mods & MOD_ALT) != keystate[KEY_LEFTALT])
		send_key(KEY_LEFTALT, !keystate[KEY_LEFTALT]);
	if(!!(mods & MOD_ALT_GR) != keystate[KEY_RIGHTALT])
		send_key(KEY_RIGHTALT, !keystate[KEY_RIGHTALT]);
}

static void reify_layer_mods(struct keyboard *kbd)
{
	uint16_t mods = 0;
	size_t i;

	for(i = 0;i < kbd->nlayers;i++) {
		struct layer *layer = kbd->layers[i];

		if(layer->active)
			mods |= layer->mods;
	}

	setmods(mods);
}

static struct key_descriptor *kbd_lookup_descriptor(struct keyboard *kbd, uint16_t code, uint16_t *modsp)
{
	size_t i;
	struct key_descriptor *desc = NULL;
	struct layer *layer = NULL;
	uint16_t mods = 0;
	size_t nactive = 0;

	//Pick the most recently activated layer in which a mapping is defined.

	for(i = 0;i < kbd->nlayers;i++) {
		struct layer *l = kbd->layers[i];
		struct key_descriptor *d = &l->keymap[code];

		if(l->active) {
			if(d->action && (!desc || (l->timestamp > layer->timestamp))) {
				desc = d;
				layer = l;
			}

			nactive++;
		}
	}

	//Calculate the modifier union of active layers, excluding the mods for
	//the layer in which the mapping is defined.

	mods = 0;
	for(i = 0;i < kbd->nlayers;i++) {
		struct layer *l = kbd->layers[i];

		if(l->active && l != layer)
			mods |= l->mods;
	}

	if(!desc) {
		//If any modifier layers are active and do not explicitly
		//define a mapping, obtain the target from modlayout.

		if(mods) {
			if(mods == MOD_SHIFT || mods == MOD_ALT_GR)
				desc = &kbd->layout->keymap[code];
			else
				desc = &kbd->modlayout->keymap[code];
		} else if(!nactive) //If no layers are active use the default layout
			desc = &kbd->layout->keymap[code];
		else
			return NULL;
	}

	*modsp = mods;
	return desc;
}

//Where the magic happens
static void process_event(struct keyboard *kbd, struct input_event *ev)
{
	size_t i;
	uint16_t code = ev->code;
	uint8_t pressed = ev->value;

	struct key_descriptor *d;

	static struct key_descriptor *lastd = NULL;
	static uint8_t oneshot_layers[MAX_LAYERS] = {0};
	static uint64_t pressed_timestamps[KEY_CNT] = {0};
	static uint64_t last_keyseq_timestamp = 0;
	uint16_t mods = 0;

	//Cache the descriptor to ensure consistency upon up/down event pairs since layers can change midkey.

	static struct key_descriptor *dcache[KEY_CNT] ={0};
	static uint16_t mcache[KEY_CNT] ={0};

	if(ev->type != EV_KEY || IS_MOUSE_BTN(ev->code)) {
		if(ev->type == EV_REL || ev->type == EV_KEY) {
			write(vptr, ev, sizeof(*ev));
			syn(vptr);
		}

		return;
	}

	//Wayland and X both ignore repeat events but VTs seem to require them.
	if(pressed == 2) {
		send_repetitions();
		return;
	}

	if(!pressed) {
		d = dcache[code];
		mods = mcache[code];

		dcache[code] = NULL;
		mcache[code] = 0;
	} else {
		pressed_timestamps[code] = get_time();
		d = kbd_lookup_descriptor(kbd, code, &mods);

		dcache[code] = d;
		mcache[code] = mods;
	}

	if(!d)
		goto keyseq_cleanup;

	switch(d->action) {
		struct layer *layer;
		uint32_t keyseq;
		uint16_t keycode;

	case ACTION_OVERLOAD:
		keyseq = d->arg.keyseq;
		layer = kbd->layers[d->arg2.layer];

		if(pressed) {
			layer->active = !layer->active;
			layer->timestamp = get_time();
		} else {
			layer->active = !layer->active;

			if(lastd == d) { //If tapped
				uint16_t key = keyseq & 0xFFFF;
				mods |= keyseq >> 16;

				setmods(mods);
				send_key(key, 1);
				send_key(key, 0);

				last_keyseq_timestamp = get_time();
				goto keyseq_cleanup;
			}
		}
		reify_layer_mods(kbd);
		break;
	case ACTION_TAP_HOLD:
		keyseq = d->arg.keyseq;
		layer = kbd->layers[d->arg2.layer];

		if(pressed) {
			layer->active = !layer->active;
			layer->timestamp = get_time();
		} else {
			layer->active = !layer->active;

			if(lastd == d && in_tapping_term(layer->timestamp)) { //If tapped
				uint16_t key = keyseq & 0xFFFF;
				mods |= keyseq >> 16;

				setmods(mods);
				send_key(key, 1);
				send_key(key, 0);

				last_keyseq_timestamp = get_time();
				goto keyseq_cleanup;
			}
		}
		reify_layer_mods(kbd);
		break;
	case ACTION_LAYOUT:
		kbd->layout = kbd->layers[d->arg.layer];
		kbd->modlayout = kbd->layers[d->arg2.layer];

		dbg("layer: %d, modlayout: %d", kbd->layout, kbd->modlayout);
		break;
	case ACTION_ONESHOT:
		layer = kbd->layers[d->arg.layer];

		if(pressed) {
			layer->active = 1;
			layer->timestamp = get_time();
		} else if(pressed_timestamps[code] < last_keyseq_timestamp) {
			layer->active = !layer->active;
		} else //Tapped
			oneshot_layers[d->arg.layer] = 1;

		reify_layer_mods(kbd);
		break;
	case ACTION_LAYER_TOGGLE:
		if(!pressed) {
			layer = kbd->layers[d->arg.layer];

			if(oneshot_layers[d->arg.layer]) {
				oneshot_layers[d->arg.layer] = 0;
			} else {
				layer->active = !layer->active;
			}
			reify_layer_mods(kbd);
			goto keyseq_cleanup;
		}
		break;
	case ACTION_LAYER:
		layer = kbd->layers[d->arg.layer];

		if(pressed) {
			layer->active = 1;
			layer->timestamp = get_time();
		} else {
			//Toggle rather than clear to account for
			//the possibility of interposed layert()
			layer->active = !layer->active;
		}

		reify_layer_mods(kbd);
		break;
	case ACTION_KEYSEQ:
		mods |= d->arg.keyseq >> 16;
		keycode = d->arg.keyseq & 0xFFFF;
		if(pressed) {
			setmods(mods);

			//Account for the possibility that a version of the key
			//with a different modifier set is already depressed (e.g [/{)
			if(keystate[keycode])
				send_key(keycode, 0);

			send_key(keycode, 1);
		} else {
			reify_layer_mods(kbd);
			send_key(keycode, 0);
		}

		goto keyseq_cleanup;
		break;
	case ACTION_MACRO:
		if(pressed) {
			uint32_t *macro = d->arg.macro;
			size_t sz = d->arg2.sz;

			for(i = 0; i < sz;i++) {
				uint32_t seq = macro[i];
				uint16_t mods = macro[i] >> 16;
				uint16_t key = macro[i] & 0xFFFF;

				if(mods & MOD_TIMEOUT) {
					usleep(GET_TIMEOUT(seq)*1000);
				} else {
					setmods(mods);
					send_key(key, 1);
					send_key(key, 0);
				}
			}

			reify_layer_mods(kbd);
			goto keyseq_cleanup;

		}
		break;
	case ACTION_UNDEFINED:
		goto keyseq_cleanup;
		break;
	}

	lastd = d;
	return;

keyseq_cleanup:
	lastd = d;

	if(pressed)
		last_keyseq_timestamp = get_time();

	//Clear active oneshot layers.
	for(i = 0;i < kbd->nlayers;i++) {
		if(oneshot_layers[i]) {
			kbd->layers[i]->active = 0;
			oneshot_layers[i] = 0;
		}
	}
}

//Block on the given keyboard nodes until no keys are depressed.
static void await_keyboard_neutrality(char **devs, int n)
{
	int fds[MAX_KEYBOARDS];
	int maxfd = 0;
	int i;

	dbg("Awaiting keyboard neutrality.");
	for(i = 0;i < n;i++) {
		if((fds[i] = open(devs[i], O_RDONLY | O_NONBLOCK)) < 0)
			die("open");

		if(fds[i] > maxfd)
			maxfd = fds[i];
	}

	//There is a race condition here since it is possible for a key down
	//event to be generated before keyd is launched, in that case we hope a
	//repeat event is generated within the first 300ms. If we miss the
	//keydown event and the repeat event is not generated within the first
	//300ms it is possible for this to yield a false positive. In practice
	//this seems to work fine. Given the stateless nature of evdev I am not
	//aware of a better way to achieve this.

	while(1) {
		struct timeval tv = {
			.tv_usec = 300000
		};

		struct input_event ev;
		int i;
		fd_set fdset;

		FD_ZERO(&fdset);
		for(i = 0;i < n;i++)
			FD_SET(fds[i], &fdset);

		select(maxfd+1, &fdset, NULL, NULL, &tv);

		for(i = 0;i < n;i++) {
			if(FD_ISSET(fds[i], &fdset)) {
				while(read(fds[i], &ev, sizeof ev) > 0) {
					if(ev.type == EV_KEY) {
						keystate[ev.code] = ev.value;
						dbg("keystate[%d]: %d", ev.code, ev.value);
					}
				}
			}
		}

		for(i = 0;i < KEY_CNT;i++)
			if(keystate[i])
				break;

		if(i == KEY_CNT)
			break;
	}

	for(i = 0;i < n;i++)
		close(fds[i]);

	dbg("Keyboard neutrality achieved");
}

static int manage_keyboard(const char *devnode)
{
	int fd;
	const char *name = evdev_device_name(devnode);
	struct keyboard *kbd;
	struct keyboard_config *cfg = NULL;
	struct keyboard_config *default_cfg = NULL;

	if(!strcmp(name, VIRTUAL_KEYBOARD_NAME) || 
	   !strcmp(name, VIRTUAL_POINTER_NAME)) //Don't manage virtual devices.
		return 0;

	for(kbd = keyboards;kbd;kbd = kbd->next) {
		if(!strcmp(kbd->devnode, devnode)) {
			dbg("Already managing %s.", devnode);
			return 0;
		}
	}

	for(cfg = configs;cfg;cfg = cfg->next) {
		if(!strcmp(cfg->name, "default"))
			default_cfg = cfg;

		if(!strcmp(cfg->name, name))
			break;
	}

	if(!cfg) {
		if(default_cfg) {
			warn("No config found for %s (%s), falling back to default.cfg", name, devnode);
			cfg = default_cfg;
		} else {
			//Don't manage keyboards for which there is no configuration.
			warn("No config found for %s (%s), ignoring", name, devnode);
			return 0;
		}
	}

	if((fd = open(devnode, O_RDONLY | O_NONBLOCK)) < 0) {
		perror("open");
		exit(1);
	}

	kbd = calloc(1, sizeof(struct keyboard));
	kbd->fd = fd;
	kbd->layers = cfg->layers;
	kbd->nlayers = cfg->nlayers;

	kbd->modlayout = cfg->layers[cfg->default_modlayout];
	kbd->layout = cfg->layers[cfg->default_layout];

	strcpy(kbd->devnode, devnode);

	//Grab the keyboard.
	if(ioctl(fd, EVIOCGRAB, (void *)1) < 0) {
		perror("EVIOCGRAB");
		exit(-1);
	}

	kbd->next = keyboards;
	keyboards = kbd;

	warn("Managing %s", evdev_device_name(devnode));
	return 1;
}

static int destroy_keyboard(const char *devnode)
{
	struct keyboard **ent = &keyboards;

	while(*ent) {
		if(!strcmp((*ent)->devnode, devnode)) {
			dbg("Destroying %s", devnode);
			struct keyboard *kbd = *ent;
			*ent = kbd->next;

			//Attempt to ungrab the the keyboard (assuming it still exists)
			if(ioctl(kbd->fd, EVIOCGRAB, (void *)1) < 0) {
				perror("EVIOCGRAB");
			}

			close(kbd->fd);
			free(kbd);

			return 1;
		}

		ent = &(*ent)->next;
	}

	return 0;
}

static void evdev_monitor_loop(int *fds, int sz)
{
	struct input_event ev;
	fd_set fdset;
	int i;
	char names[256][256];

	for(i = 0;i < sz;i++) {
		int fd = fds[i];
		if(ioctl(fd, EVIOCGNAME(sizeof(names[fd])), names[fd]) == -1) {
			perror("ioctl");
			exit(-1);
		}
	}

	while(1) {
		int i;
		int maxfd = fds[0];

		FD_ZERO(&fdset);
		for(i = 0;i < sz;i++) {
			if(maxfd < fds[i]) maxfd = fds[i];
			FD_SET(fds[i], &fdset);
		}

		select(maxfd+1, &fdset, NULL, NULL, NULL);

		for(i = 0;i < sz;i++) {
			int fd = fds[i];
			if(FD_ISSET(fd, &fdset)) {
				while(read(fd, &ev, sizeof(ev)) > 0) {
					if(ev.type == EV_KEY && ev.value != 2) {
						const char *name = keycode_table[ev.code].name;
						if(name)
							fprintf(stderr, "%s: %s %s\n",
								names[fd],
								name,
								ev.value == 0 ? "up" : "down");
						else
							fprintf(stderr, "Unrecognized keycode: %d\n", ev.code);
					}
				}
			}
		}
	}
}

static int monitor_loop()
{
	char *devnodes[256];
	int sz, i;
	int fd = -1;
	int fds[256];
	int nfds = 0;

	get_keyboard_nodes(devnodes, &sz);

	for(i = 0;i < sz;i++) {
		fd = open(devnodes[i], O_RDONLY | O_NONBLOCK);
		if(fd < 0) {
			perror("open");
			exit(-1);
		}
		fds[nfds++] = fd;
	}

	evdev_monitor_loop(fds, nfds);

	return 0;
}

static void main_loop()
{
	struct keyboard *kbd;
	int monfd;

	int i, n;
	char *devs[MAX_KEYBOARDS];

	get_keyboard_nodes(devs, &n);
	await_keyboard_neutrality(devs, n);

	for(i = 0;i < n;i++) {
		manage_keyboard(devs[i]);
		free(devs[i]);
	}

	udev = udev_new();
	udevmon = udev_monitor_new_from_netlink(udev, "udev");

	if (!udev)
		die("Can't create udev.");

	udev_monitor_filter_add_match_subsystem_devtype(udevmon, "input", NULL);
	udev_monitor_enable_receiving(udevmon);

	monfd = udev_monitor_get_fd(udevmon);

	while(1) {
		int maxfd;
		fd_set fds;
		struct udev_device *dev;

		FD_ZERO(&fds);
		FD_SET(monfd, &fds);

		maxfd = monfd;

		for(kbd = keyboards;kbd;kbd=kbd->next) {
			int fd = kbd->fd;

			maxfd = maxfd > fd ? maxfd : fd;
			FD_SET(fd, &fds);
		}

		if(select(maxfd+1, &fds, NULL, NULL, NULL) > 0) {
			if(FD_ISSET(monfd, &fds)) {
				dev = udev_monitor_receive_device(udevmon);
				const char *devnode = udev_device_get_devnode(dev);

				if(devnode && is_keyboard(dev)) {
					const char *action = udev_device_get_action(dev);

					if(!strcmp(action, "add"))
						manage_keyboard(devnode);
					else if(!strcmp(action, "remove"))
						destroy_keyboard(devnode);
					else
						dbg("udev: action %s %s", action, devnode);
				}
				udev_device_unref(dev);
			}


			for(kbd = keyboards;kbd;kbd=kbd->next) {
				int fd = kbd->fd;

				if(FD_ISSET(fd, &fds)) {
					struct input_event ev;

					while(read(fd, &ev, sizeof(ev)) > 0) {
						process_event(kbd, &ev);
					}
				}
			}
		}
	}
}


static void cleanup()
{
	struct keyboard *kbd = keyboards;
	config_free();

	while(kbd) {
		struct keyboard *tmp = kbd;
		kbd = kbd->next;
		free(tmp);
	}

	udev_unref(udev);
	udev_monitor_unref(udevmon);
}

static void lock()
{
	int fd;

	if((fd=open(LOCK_FILE, O_CREAT | O_RDWR, 0600)) == -1) {
		perror("flock open");
		exit(1);
	}

	if(flock(fd, LOCK_EX | LOCK_NB) == -1) {
		warn("Another instance of keyd is already running.");
		exit(-1);
	}
}


static void exit_signal_handler(int sig)
{
	warn("%s received, cleaning up and terminating...", sig == SIGINT ? "SIGINT" : "SIGTERM");

	cleanup();
	exit(0);
}

static void daemonize()
{
	int fd = open(LOG_FILE, O_APPEND|O_WRONLY);

	warn("Daemonizing.");
	warn("Log output will be stored in %s", LOG_FILE);

	if(fork()) exit(0);
	if(fork()) exit(0);

	close(0);
	close(1);
	close(2);

	dup2(fd, 1);
	dup2(fd, 2);
}

int main(int argc, char *argv[])
{
	if(getenv("KEYD_DEBUG"))
		debug = 1;

	dbg("Debug mode enabled.");

	if(argc > 1) {
		if(!strcmp(argv[1], "-v")) {
			fprintf(stderr, "keyd version: %s (%s)\n", VERSION, GIT_COMMIT_HASH);
			return 0;
		} else if(!strcmp(argv[1], "-m")) {
			return monitor_loop();
		} else if(!strcmp(argv[1], "-l")) {
			size_t i;

			for(i = 0; i < KEY_MAX;i++)
				if(keycode_table[i].name) {
					const struct keycode_table_ent *ent = &keycode_table[i];
					printf("%s\n", ent->name);
					if(ent->alt_name)
						printf("%s\n", ent->alt_name);
					if(ent->shifted_name)
						printf("%s\n", ent->shifted_name);
				}
			return 0;
		}
	}

	lock();

	signal(SIGINT, exit_signal_handler);
	signal(SIGTERM, exit_signal_handler);

	if(argc > 1 && !strcmp(argv[1], "-d"))
		daemonize();

	warn("Starting keyd v%s (%s).", VERSION, GIT_COMMIT_HASH);
	config_generate();
	vkbd = create_virtual_keyboard();
	vptr = create_virtual_pointer();

	main_loop();
}
