/*
 * dm2.c  -  Mixman DM2 stateful MIDI driver
 *
 *
 * Copyright (C) 2007-2008 Jan Jockusch (jan@jockusch.de)
 * Copyright (C) 2006-2008 Andre Roth <lynx@netlabs.org>
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * $Id: dm2.c,v 1.53 2008/03/04 10:55:10 jan Exp $
 *
 */

/*
 * TODO
 *
 * Note on / note off on primary and secondary beat: reset LED position,
 * then advance one position on every secondary beat.
 * CC on a channel: light the LEDs in a VU meter pattern.
 *
 * MIDI configuration: Use a system exclusive (SysEx) block to program
 * all keys and buttons. Use reset (0xff) to get the default mode.
 *
 * Emulate BCD 3000 feature of calling all controller settings with one
 * SysEx call.
 * 
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <asm/uaccess.h>
#include <linux/usb.h>
#include <linux/spinlock.h>
#include <linux/version.h>

#include <sound/core.h>
#include <sound/rawmidi.h>
#include <sound/initval.h>

#include "dm2.h"

static int index = SNDRV_DEFAULT_IDX1;	/* Index 0-MAX */
static char *id = SNDRV_DEFAULT_STR1;	/* ID for this card */

module_param(index, int, 0444);
MODULE_PARM_DESC(index, "Index value for DM2 MIDI controller.");
module_param(id, charp, 0444);
MODULE_PARM_DESC(id, "ID string for DM2 MIDI controller.");

static struct usb_driver dm2_driver;

// Make kernel version check
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,22)
#  warning This driver will not compile for kernels older than 2.6.22
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
#  warning Please make sure your kernel is patched with linux-lowspeedbulk.patch
#  define USE_BULK_SNDPIPE 1
#endif
// Kernel API compatibility
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,30)
static inline
int snd_card_create(int idx, const char *id,
		    struct module *module, int extra_size,
		    struct snd_card **card_ret)
{
	*card_ret = snd_card_new(idx, id, module, extra_size);
	if (!(*card_ret)) return -1;
	return 0;
}
#endif

#define err(format, arg...) printk(KERN_ERR KBUILD_MODNAME ": " format "\n" , ## arg)
#define info(format, arg...) printk(KERN_INFO KBUILD_MODNAME ": " format "\n" , ## arg)


static void dm2_slider_reset(struct dm2slider *slider, u8 value)
{
	slider->pos = value;
	slider->mid = value;
	slider->min = value - slider->dead - 1;
	slider->max = (slider->max) ? value + slider->dead + 1 : 0;
	slider->midival = 64;
}

static void dm2_slider_init(struct dm2slider *slider, u8 param, u8 dead, u8 usemax)
{
	slider->param = param;
	slider->max = usemax;
	slider->dead = dead;
	dm2_slider_reset(slider, slider->mid ? slider->mid : 80);	/* Dummy value */
}

static void dm2_slider_set(struct dm2slider *slider, u8 value)
{
	if (value < slider->min) slider->min = value;
	if (slider->max && (value > slider->max)) slider->max = value;
	slider->pos = value;
}

static int dm2_slider_get(struct dm2slider *slider)
{
	int value;
	u8 max = slider->max;

	if (!max) max = (slider->mid<<1) - slider->min;
	if (slider->pos < slider->mid) {
		value = ((slider->pos - slider->min)*64 /
			 (slider->mid - slider->dead - slider->min));
		if (value > 64) value = 64;
	} else {
		value = (127 - (max - slider->pos)*63 /
			 (max - slider->dead - slider->mid));
		if (value < 64) value = 64;
	}
	if (value < 0) value = 0;
	if (value > 127) value = 127;
	return value;
}

static void dm2_slider_update(struct usb_dm2 *dev, struct dm2slider *slider, u8 prev, u8 curr)
{
	int value;
	
	dm2_slider_set(slider, curr);
	value = dm2_slider_get(slider);
	if (value == slider->midival) return;
	dm2_midi_send(dev, 0xb0, slider->param, value);
	slider->midival = value;
	return;
}

static void dm2_wheel_init(struct dm2wheel *wheel, const u8 notes[8], const u8 params[8],
			   u8 jogparam, u8 midup, u8 middown, u8 midrel, u8 exclusive,
			   u8 relparams, u8 notoggle, u8 paramthresh, u8 cursorthresh)
{
	int i;
	u8 mask;

	wheel->turnacc = wheel->showlight = 0;
	wheel->pressed = wheel->light = wheel->whenreleased = 0;
	wheel->midpressed = 0;
	wheel->jogparam = jogparam;
	wheel->jogmidival = 64;
	for (i=0, mask=1; i<8; i++, mask<<=1) {
		wheel->notes[i] = notes[i];
		wheel->params[i] = params[i];
		wheel->midivals[i] = 64;
	}
	wheel->relparams = ((relparams<<1)&0xf0) | (relparams&0x07);
	wheel->notoggle = ((notoggle<<1)&0xf0) | (notoggle&0x07);
	wheel->wheelused = 0;
	wheel->midup = midup;
	wheel->middown = middown;
	wheel->midrel = midrel;
	wheel->exclusive = exclusive;
	wheel->paramthresh = paramthresh;
	wheel->cursorthresh = cursorthresh;
}

static void dm2_wheel_update(struct usb_dm2 *dev, struct dm2wheel *wheel, u8 curr, u8 currmid)
{
	u8 presses, releases, newlight, reset, mask, flagson, flagsoff;
	int i;

	currmid &= DM2_MIDMASK;
	if ((wheel->pressed == curr) && (wheel->midpressed == currmid))
		return;
	wheel->turnacc = 0;

	// Calculate note on/off
	presses = ~wheel->pressed & curr;
	releases = wheel->pressed & ~curr;

	flagson  = presses  & (wheel->notoggle | ~wheel->light);
	flagsoff = releases & (wheel->notoggle | ~wheel->whenreleased);
	for (i=0, mask=1; i<8; i++, mask<<=1) {
		if (!wheel->notes[i]) continue;
		if (!wheel->params[i]) {
			if (mask & flagson)
				dm2_midi_send(dev, 0x90, wheel->notes[i], 0x7f);
			if (mask & flagsoff)
				dm2_midi_send(dev, 0x90, wheel->notes[i], 0x00);
			continue;
		}
		if ((  wheel->wheelused  && (mask & releases & ~wheel->notoggle & ~wheel->whenreleased)) || 
		    ((!wheel->wheelused) && (mask & releases &  wheel->notoggle)))
			dm2_midi_send(dev, 0x90, wheel->notes[i], 0x7f);
	}

	// Mid key
	if ((wheel->midpressed & ~currmid) && wheel->midrel && wheel->wheelused) {
		dm2_midi_send(dev, 0x90, wheel->midrel, 0x7f);
	}

	// Releases
	releases &= ~DM2_CLR;
	newlight = wheel->whenreleased & releases;
	if (!(wheel->exclusive && newlight)) newlight |= wheel->light & ~releases;
	newlight = (newlight & ~DM2_CLR) | DM2_MID(currmid);
	wheel->whenreleased &= ~releases;

	// Keys which are masked out as toggles
	newlight = ((newlight & ~wheel->notoggle) |
		    (curr & wheel->notoggle));

	// Bottom keypress: reset values
	reset = (presses & DM2_CLR);
	if (flagson || (currmid & ~wheel->midpressed)) wheel->wheelused = 0;
	if ((wheel->pressed ^ curr) & DM2_CLR) wheel->wheelused = 1;

	// Other presses
	presses = (presses & ~DM2_CLR) | DM2_MID(~wheel->midpressed & currmid);
	wheel->whenreleased = ((wheel->whenreleased & ~presses) |
			       (~newlight & presses));
	newlight |= presses;
	wheel->light = newlight;
	wheel->pressed = curr;
	wheel->midpressed = currmid;

	// Reset values
	if (!reset) return;
	for (i=0, mask=1; i<8; i++, mask<<=1) {
		if (!(mask & newlight)) continue;
		if (!(wheel->params[i])) continue;
		if (wheel->midivals[i] == 64) continue;
		wheel->midivals[i] = 64;
		dm2_midi_send(dev, 0xb0, wheel->params[i], wheel->midivals[i]);
	}
}

static void dm2_wheel_turn(struct usb_dm2 *dev, struct dm2wheel *wheel, u8 step)
{
	int acc, midiadd, value, i, diff, thresh, reldiff;
	u8 params, mask;

	diff = step;
	if (step & 0x80) diff-=256;
	diff = -diff;

	// Calculate step for relative mode
	// reldiff = diff += 64;
	// reldiff = (reldiff < 0) ? 0 : (reldiff > 127) ? 127: reldiff;

	// Jog wheel mode
	if (!(wheel->pressed || wheel->light || wheel->midpressed)) {
		reldiff = diff;
		if (reldiff != 0) {
			do {
				int trnc = (reldiff < -64) ? -64 : (reldiff > 63) ? 63 : reldiff;
				dm2_midi_send(dev, 0xb0, wheel->jogparam, trnc+64);
				wheel->jogmidival = trnc+64;
				reldiff -= trnc;
			} while (reldiff);
		} else {
			if (wheel->jogmidival != 64)
				dm2_midi_send(dev, 0xb0, wheel->jogparam, 64);
			wheel->jogmidival = 64;
		}
		return;
	}

	// Adjust stepping accumulator (for absolute CCs and cursor motion)
	thresh = wheel->paramthresh;
	if (wheel->midpressed && (wheel->midup || wheel->middown))
		thresh = wheel->cursorthresh;
	acc = wheel->turnacc;
	acc += diff;
	midiadd = acc / thresh;
	wheel->turnacc = acc % thresh;
	// if (!midiadd && !wheel->relparams) return;

	wheel->showlight = 1;
	wheel->wheelused = 1;

	// Mid key pressed: only mid parameter / cursor
	if (wheel->midpressed) {
		if (wheel->midup || wheel->middown) {
			if ((midiadd < 0) && wheel->middown) {
				for (i=0; i<-midiadd; i++)
					dm2_midi_send(dev, 0x90, wheel->middown, 0x7f);
			}
			if ((midiadd > 0) && wheel->midup) {
				for (i=0; i<midiadd; i++)
					dm2_midi_send(dev, 0x90, wheel->midup, 0x7f);
			}
			return;
		}
		if (wheel->params[DM2_MIDINDEX]) {
			value = wheel->midivals[DM2_MIDINDEX] + midiadd;
			value = (value < 0) ? 0 : (value > 127) ? 127: value;
			if (value != wheel->midivals[DM2_MIDINDEX]) {
				dm2_midi_send(dev, 0xb0, wheel->params[DM2_MIDINDEX], value);
				wheel->midivals[DM2_MIDINDEX] = value;
			}
			return;
		}
	}

	// Use presses, then lights
	params = wheel->pressed;
	if (params) {
		params = wheel->pressed;
		wheel->whenreleased = wheel->light & ~params;
	} else {
		params = wheel->light;		
	}

	// Transmit params
	if (!params) return;
	for (i=0, mask=1; i<8; i++, mask<<=1) {
		if (!(params & mask)) continue;
		if (!(wheel->params[i])) continue;
		if (wheel->relparams & mask) {
			reldiff = diff;
			if (reldiff != 0) {
				do {
					int trnc = (reldiff < -64) ? -64 : (reldiff > 63) ? 63 : reldiff;
					dm2_midi_send(dev, 0xb0, wheel->params[i], trnc+64);
					wheel->midivals[i] = trnc+64;
					reldiff -= trnc;
				} while (reldiff);
			} else {
				if (wheel->midivals[i] != 64)
					dm2_midi_send(dev, 0xb0, wheel->params[i], 64);
				wheel->midivals[i] = 64;
			}
		} else {
			value = wheel->midivals[i] + midiadd;
			value = (value < 0) ? 0 : (value > 127) ? 127: value;
			if ((value != wheel->midivals[i]) &&
			    wheel->params[i]) {
				dm2_midi_send(dev, 0xb0, wheel->params[i], value);
				wheel->midivals[i] = value;
			}
		}
	}
	return;
}

static void dm2_buttons_init(struct dm2buttons *buttons, const u8 notes[8])
{
	buttons->pressed = 0;
	memcpy(buttons->notes, notes, 8*sizeof(u8));
	return;
}

static void dm2_buttons_update(struct usb_dm2 *dev, struct dm2buttons *buttons, u8 curr)
{
	u8 presses, releases, mask;
	int i;

	if (buttons->pressed == curr) return;
	presses = ~buttons->pressed & curr;
	releases = buttons->pressed & ~curr;
	for (i=0, mask=1; i<8; i++, mask<<=1) {
		if (!buttons->notes[i]) continue;
		if (mask & presses)
			dm2_midi_send(dev, 0x90, buttons->notes[i], 0x7f);
		if (mask & releases)
			dm2_midi_send(dev, 0x90, buttons->notes[i], 0x00);
	}
	buttons->pressed = curr;
	return;
}

static void dm2_leds_init(struct dm2leds *leds, const u8 notes[8], u8 idlenote)
{
	leds->timeout = 0;
	leds->idletimeout = leds->wheeltimeout = 0;
	leds->curr = leds->mask = leds->light = 0;
	memcpy(leds->notes, notes, 8*sizeof(u8));
	leds->idlelight = leds->wheel = 0;
	leds->idlenote = idlenote;
}

static void dm2_leds_timer(struct dm2leds *leds)
{
	// Handle idle loop
	if (leds->idlelight) {
		if (!leds->idletimeout) {
			leds->idletimeout = DM2_LEDIDLEINT;
			leds->idlelight >>= 1;
			if (!leds->idlelight) leds->idlelight = 0x80;
		}
		leds->idletimeout--;
	}

	// Handle mask timeout
	if (leds->wheeltimeout) leds->wheeltimeout--;
	if (!(leds->timeout)) return;
	if (--(leds->timeout)) return;
	leds->mask = leds->light = 0;
}

#ifdef UNUSED_FUNCTIONS
static void dm2_leds_overlay(struct dm2leds *leds, u8 mask, u8 light)
{
	leds->timeout = DM2_LEDTIMEOUT;
	leds->mask |= mask;
	leds->light = (leds->light & ~mask) | (light & mask);
}
#endif

static void dm2_leds_update(struct dm2leds *leds, u8 note, u8 vel)
{
	int i;
	u8 mask;

	leds->timeout = DM2_LEDTIMEOUT;
	for (i=0, mask=1; i<8; i++, mask<<=1) {
		if (leds->notes[i] != note) continue;
		if (vel) leds->light |= mask;
		else     leds->light &= ~mask;
		leds->mask |= mask;
	}
	if (note == leds->idlenote)
		leds->idlelight = vel ? 0x80 : 0;
}

static void dm2_leds_send(struct usb_dm2 *dev)
{
	int i, send = 0;
	u8 new[2];
	struct dm2leds *leds;

	for (i=0; i<2; i++) {
		// Handle timing of LED layers
		leds = &(dev->dm2.leds[i]);
		if ((dev->dm2.wheels[i].light != leds->wheel) ||
		    dev->dm2.wheels[i].showlight || dev->dm2.wheels[i].light) {
			leds->wheeltimeout = DM2_LEDTIMEOUT;
			leds->wheel = dev->dm2.wheels[i].light;
			dev->dm2.wheels[i].showlight = 0;
		}
		// Merge layers
		new[i] = leds->wheeltimeout ? leds->wheel : leds->idlelight;
		new[i] = ((new[i] & ~leds->mask) | (leds->light & leds->mask));
		if (leds->curr == new[i]) continue;
		leds->curr = new[i];
		send = 1;
	}

	if (send) dm2_set_leds(dev, new[0], new[1]);
}


/* Main event handler */

static void dm2_tasklet(unsigned long arg)
{
	struct usb_dm2 *dev;
	u8 curr[10], prev[10];
	unsigned long flags;

	dev = (struct usb_dm2 *)arg;

	spin_lock_irqsave(&dev->lock, flags);
	memcpy(curr, dev->dm2.curr_state, 10*sizeof(u8));
	spin_unlock_irqrestore(&dev->lock, flags);

	memcpy(prev, dev->dm2.prev_state, 10*sizeof(u8));


	// byte 0, 1: handle right and left shift buttons.
	if ((curr[1] != prev[1]) || (curr[3] != prev[3]))
		dm2_wheel_update(dev, &(dev->dm2.wheels[0]), curr[1], curr[3]);
	if ((curr[0] != prev[0]) || (curr[3] != prev[3]))
		dm2_wheel_update(dev, &(dev->dm2.wheels[1]), curr[0], curr[3]);

	// byte 2, 3: handle top and bottom normal buttons.
	if (curr[2] != prev[2]) dm2_buttons_update(dev, &(dev->dm2.buttons[0]), curr[2]);
	if (curr[3] != prev[3]) dm2_buttons_update(dev, &(dev->dm2.buttons[1]), curr[3]);

	// bytes 5, 6, 7: handle sliders.
	if (curr[5] != prev[5]) dm2_slider_update(dev, &(dev->dm2.sliders[0]), prev[5], curr[5]);
	if (curr[6] != prev[6]) dm2_slider_update(dev, &(dev->dm2.sliders[1]), prev[6], curr[6]);
	if (curr[7] != prev[7]) dm2_slider_update(dev, &(dev->dm2.sliders[2]), prev[7], curr[7]);

	// bytes 8, 9: handle wheels.
	if (curr[8] || prev[8]) dm2_wheel_turn(dev, &(dev->dm2.wheels[0]), curr[8]);
	if (curr[9] || prev[9]) dm2_wheel_turn(dev, &(dev->dm2.wheels[1]), curr[9]);

	// Update LEDs
	dm2_leds_timer(&(dev->dm2.leds[0]));
	dm2_leds_timer(&(dev->dm2.leds[1]));
	dm2_leds_send(dev);

	memcpy(dev->dm2.prev_state, curr, 10*sizeof(u8));

#if 0
	// Print current status in hex.
	{
		int i;
		printk("received: ");
		for (i=0; i<10; i++) printk( "%02x ", curr[i] );
		printk( "\n" );
	}
#endif
}



/* URB writing interface */

static ssize_t dm2_write(struct usb_dm2 *dev, const char *data, size_t count);
static void dm2_set_leds(struct usb_dm2 *dev, u8 left, u8 right)
{
	char data[4] = { 0xff, 0xff, 0xff, 0xff };
	data[0] ^= right; data[1] ^= left;
	dm2_write(dev, data, 4);
}

/* Basic interpretation of received URBs */

static void dm2_update_status(struct usb_dm2 *dev, u8 *buf, int length)
{
	// ATTENTION: Called in interrupt context!
	int i;
	unsigned long flags;

	if (length != 10) {
		err("Unexpected URB length!");
		return;
	}

	// Invert X joystick axis.
	buf[5] = ~buf[5];

	// Slider initialization with fancy LED blinking.
	if (dev->dm2.initialize==38) dm2_set_leds(dev, 0xaa, 0x55);
	if (dev->dm2.initialize==25) dm2_set_leds(dev, 0x55, 0xaa);
	if (dev->dm2.initialize==12) dm2_set_leds(dev, 0xff, 0xff);
	if (dev->dm2.initialize==1)  dm2_set_leds(dev, 0x00, 0x00);
	if (dev->dm2.initialize && (!--dev->dm2.initialize)) {
		for (i=0; i<3; i++) dm2_slider_reset(&(dev->dm2.sliders[i]), buf[i+5]);
		dm2_set_leds(dev, 0, 0);
	}

	// Nothing works until initialization is complete!
	if (dev->dm2.initialize) return;	

	// Transfer latest transmission into dm2 structure.
	spin_lock_irqsave(&dev->lock, flags);
	memcpy(dev->dm2.curr_state, buf, 10*sizeof(u8));
	spin_unlock_irqrestore(&dev->lock, flags);

	// Trigger further processing.
	tasklet_schedule(&dev->dm2midi.tasklet);

	return;
}


/* Initialize DM2 structure */

static void dm2_internal_init(struct dm2 *dm2, struct dm2_params *params)
{
	int i;

	memset(dm2->prev_state, 0, 10*sizeof(u8));
	dm2->initialize = 50;
	for (i=0; i<3; i++)
		dm2_slider_init(&(dm2->sliders[i]), params->sliderparam[i],
				params->sliderdeadzone,	(i==2) ? 0 : 1);

	dm2_wheel_init(&(dm2->wheels[0]), params->wheel0notes, params->wheel0params,
		       params->wheel0jogparam, params->midup0, params->middown0,
		       params->midrel0, params->excl0, params->relparams0,
		       params->notoggle0, params->paramthresh, params->cursorthresh);
	dm2_wheel_init(&(dm2->wheels[1]), params->wheel1notes, params->wheel1params,
		       params->wheel1jogparam, params->midup1, params->middown1,
		       params->midrel1, params->excl1, params->relparams1,
		       params->notoggle1, params->paramthresh, params->cursorthresh);

	dm2_buttons_init(&(dm2->buttons[0]), params->buttons0);
	dm2_buttons_init(&(dm2->buttons[1]), params->buttons1);

	dm2_leds_init(&(dm2->leds[0]), params->led0notes, params->led0idle);
	dm2_leds_init(&(dm2->leds[1]), params->led1notes, params->led1idle);
	return;
}


/* MIDI processing */
static void dm2_midi_process(struct usb_dm2 *dev, unsigned char byte)
{
	int args = 0, args_required = 2;
	unsigned char cmd, arg1, arg2 = 0;
	struct dm2midi *dm2midi = &(dev->dm2midi);

	if (byte == 0xff) {
		// perform reset
		dm2midi->in_rstatus = dm2midi->in_arg1 = 0;
		dm2midi->out_rstatus = 0;
		dm2_internal_init(&(dev->dm2), &(dm2_params[0]));
		return;
	}	

	// Handle SysEx (0xf0..0xf7) here!

	if (byte & 0x80) {
		if (dm2midi->chan && ((byte & 0x0f) != dm2midi->chan)) {
			dm2midi->in_rstatus = dm2midi->in_arg1 = 0;
			return;
		}
		cmd = byte & 0xf0;
		if ((cmd < 0x80) || (cmd > 0xc0) || (cmd == 0xa0)) {
			dm2midi->in_rstatus = dm2midi->in_arg1 = 0;
			return;
		}
		dm2midi->in_rstatus = byte;
		dm2midi->in_arg1 = 0;
		return;
	}

	cmd = dm2midi->in_rstatus & 0xf0;
	if (!cmd) return;
	if (cmd == 0xc0) args_required = 1;

	// Fill argument in and check for completion
	if (!dm2midi->in_arg1) {
		dm2midi->in_arg1 = byte; args = 1;
	} else {
		arg2 = byte; args = 2;
	}

	arg1 = dm2midi->in_arg1;
	if (args < args_required) return;
	dm2midi->in_arg1 = 0;

	switch (cmd) {
	case 0x80:
		arg2 = 0;
	case 0x90:
	case 0xb0:
		dm2_leds_update(&(dev->dm2.leds[0]), arg1, arg2);
		dm2_leds_update(&(dev->dm2.leds[1]), arg1, arg2);
		return;
	case 0xc0:
		if (arg1 < DM2_NUMPRESETS)
			dm2_internal_init(&(dev->dm2), &(dm2_params[arg1]));
		return;
	}
}



/* Midi functions */

static int dm2_midi_input_open(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
 	dev->dm2midi.input = substream;
	/* Reset the current status */
	dev->dm2midi.out_rstatus = 0;
	/* increment our usage count for the device */
	kref_get(&dev->kref);
	return 0;
}

static int dm2_midi_input_close(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	dev->dm2midi.input = NULL;
	/* decrement the count on our device */
	kref_put(&dev->kref, dm2_delete);
	return 0;
}

static int dm2_midi_output_open(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	dev->dm2midi.output = substream;
	/* increment our usage count for the device */
	kref_get(&dev->kref);
	return 0;
}

static int dm2_midi_output_close(struct snd_rawmidi_substream *substream)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	/* decrement the count on our device */
	kref_put(&dev->kref, dm2_delete);
	return 0;
}

static void dm2_midi_input_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	if (up)
		dev->dm2midi.input_triggered = 1;
	else
		dev->dm2midi.input_triggered = 0;
	// Should reschedule a tasklet which does snd_rawmidi_receive(substream, data, len) ?
}

static void dm2_midi_output_trigger(struct snd_rawmidi_substream *substream, int up)
{
	struct usb_dm2 *dev = substream->rmidi->private_data;
	unsigned char byte;

	while (snd_rawmidi_transmit_peek(substream, &byte, 1)) {
		dm2_midi_process(dev, byte);
		snd_rawmidi_transmit_ack(substream, 1);
	}
}

static struct snd_rawmidi_ops dm2_midi_output = {
	.open =		dm2_midi_output_open,
	.close =	dm2_midi_output_close,
	.trigger =	dm2_midi_output_trigger,
};

static struct snd_rawmidi_ops dm2_midi_input = {
	.open = 	dm2_midi_input_open,
	.close =	dm2_midi_input_close,
	.trigger =	dm2_midi_input_trigger,
};


static void dm2_midi_send(struct usb_dm2 *dev, u8 cmd, u8 param, u8 value)
{
	unsigned char midimsg[3] = { cmd, param, value };
	if (!dev->dm2midi.input) return;
	midimsg[0] += dev->dm2midi.chan;
	// Use running status
	if (midimsg[0] == dev->dm2midi.out_rstatus)
		snd_rawmidi_receive(dev->dm2midi.input, midimsg+1, 2);
	else
		snd_rawmidi_receive(dev->dm2midi.input, midimsg, 3);
	dev->dm2midi.out_rstatus = midimsg[0];
}


static int __devinit dm2_midi_init(struct usb_dm2 *dev)
{
	struct snd_rawmidi *rmidi;
	struct snd_card *card;
	int err;

	tasklet_init(&dev->dm2midi.tasklet, dm2_tasklet, (unsigned long)dev );

	if (snd_card_create(index, id, THIS_MODULE, 0, &card) < 0) {
		printk("%s snd_card_create failed\n", __FUNCTION__);
		return -ENOMEM;
	}
	dev->dm2midi.card = card;
	if ((err = snd_rawmidi_new(dev->dm2midi.card, "Mixman DM2", 1, 1, 1, &rmidi)) < 0) {
		printk("%s snd_rawmidi_new failed\n", __FUNCTION__);
		return err;
	}
	strcpy(rmidi->name, "Mixman DM2");
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_OUTPUT, &dm2_midi_output);
	snd_rawmidi_set_ops(rmidi, SNDRV_RAWMIDI_STREAM_INPUT, &dm2_midi_input);
	rmidi->info_flags |= SNDRV_RAWMIDI_INFO_OUTPUT | SNDRV_RAWMIDI_INFO_INPUT | SNDRV_RAWMIDI_INFO_DUPLEX;
	rmidi->private_data = dev;
	dev->dm2midi.rmidi = rmidi;

	if ((err = snd_card_register(dev->dm2midi.card)) < 0) {
		printk( "%s snd_card_register failed\n", __FUNCTION__);
		snd_card_free(dev->dm2midi.card);
		return err;
	}

	// Variables
	dev->dm2midi.chan = 0;
	dev->dm2midi.in_arg1 = dev->dm2midi.in_rstatus = dev->dm2midi.out_rstatus = 0;

	return 0;
}


static void dm2_midi_destroy(struct usb_dm2 *dev)
{
	if (dev->dm2midi.card) {
                snd_card_free(dev->dm2midi.card);
		dev->dm2midi.card = NULL;
	}
}


/* End of MIDI functions */



/* Generic USB driver section below. Only hook new functions in, do not edit a lot! */


static void dm2_write_int_callback(struct urb *urb)
{
	struct usb_dm2 *dev;

	dev = (struct usb_dm2 *)urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status &&
	    !(urb->status == -ENOENT ||
	      urb->status == -ECONNRESET ||
	      urb->status == -ESHUTDOWN)) {
		err("%s - nonzero write status received: %d",
		    __FUNCTION__, urb->status);
	}
	/* Unlock collision detector */
	dev->output_failed = 0;
	up(&dev->limit_sem);
}


static ssize_t dm2_write(struct usb_dm2 *dev, const char *data, size_t count)
{
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	size_t writesize = min(count, (size_t)MAX_TRANSFER);
	unsigned long flags;

	/* If there's trouble with output (on <=2.6.22 without patch),
	 * we bail out immediately. */

	/* This doubles as a collision preventer... */
	if (dev->output_failed) goto exit;

	/* verify that we actually have some data to write */
	if (count == 0)
		goto exit;

	/* limit the number of URBs in flight to stop a user from using up all RAM */
	if (down_interruptible(&dev->limit_sem)) {
		retval = -ERESTARTSYS;
		goto exit;
	}

	urb = dev->int_out_urb;
	buf = dev->int_out_buffer;

	memcpy(buf, data, writesize);

	/* this lock makes sure we don't submit URBs to gone devices */
	spin_lock_irqsave(&dev->lock, flags);
	if (!dev->interface) {		/* disconnect() was called */
		spin_unlock_irqrestore(&dev->lock, flags);
		retval = -ENODEV;
		goto error;
	}

	/* send the data out the int port */
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	spin_unlock_irqrestore(&dev->lock, flags);

	if (retval) {
		err("%s - failed submitting write urb, error %d", __FUNCTION__, retval);
		if (retval == -EINVAL) {
			dev->output_failed = 1;
			info("Your kernel cannot transmit data to the DM2.");
			info("The driver will still work, but there will be no LED output.");
			info("To make the LEDs work on 2.6.22, please apply the kernel patch that came with this driver!");
		}
		goto error;
	}

	/* Collision prevention */
	dev->output_failed = 1;

	return writesize;

error:
	up(&dev->limit_sem);

exit:
	if (retval) printk("%s - failed to write urb, error %d\n", __FUNCTION__, retval);
	return retval;
}


static void dm2_read_int_callback(struct urb *urb)
{
	// ATTENTION: Called in interrupt context!
	struct usb_dm2 *dev = urb->context;
  
	if (urb->status == 0) {
		dm2_update_status(dev, urb->transfer_buffer, urb->actual_length);
	}
	if (urb->status != -ENOENT && urb->status != -ECONNRESET) {
		urb->dev = dev->udev;
		usb_submit_urb(urb, GFP_ATOMIC);
	}
}

static int dm2_setup_writer(struct usb_dm2 *dev) {
	int bufsize = 4;
	void *buf = NULL;
	struct urb *urb = NULL;

	buf = kmalloc(bufsize, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
  
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		kfree(buf);
		return -ENOMEM;
	}
#ifdef USE_BULK_SNDPIPE
	// Compatibility code for older kernels:
	usb_fill_bulk_urb(urb, dev->udev,
			  usb_sndbulkpipe(dev->udev, dev->int_out_endpointAddr),
			  buf, bufsize, dm2_write_int_callback, dev);
#else
	usb_fill_int_urb(urb, dev->udev,
			 usb_sndintpipe(dev->udev, dev->int_out_endpointAddr),
			 buf, bufsize, dm2_write_int_callback, dev, 10);
#endif
	// urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP || URB_ZERO_PACKET;

	dev->int_out_urb = urb;
	dev->int_out_buffer = buf;

	return 0;
}

static int dm2_setup_reader(struct usb_dm2 *dev) {
	int bufsize = 32;
	int retval;
	void *buf = NULL;
	struct urb *urb = NULL;

	buf = kmalloc(bufsize, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
  
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		kfree(buf);
		return -ENOMEM;
	}
	usb_fill_int_urb(urb, dev->udev,
			 usb_rcvintpipe(dev->udev, dev->int_in_endpointAddr ),
			 buf, bufsize,
			 dm2_read_int_callback, dev, dev->int_in_interval);
	dev->int_in_urb = urb;
	retval = usb_submit_urb(urb, GFP_KERNEL);
	if (retval) {
		kfree(buf);
		return retval;
	}
	return 0;
}

static void dm2_delete(struct kref *kref)
{
	struct usb_dm2 *dev = to_dm2_dev(kref);
	// struct urb *urb;

	usb_put_dev(dev->udev);
	kfree(dev->int_in_buffer);

	/* XXX  Handling correct? */
	kfree(dev->int_out_buffer);
	// urb = dev->int_out_urb;
	// usb_free_urb(urb);

	kfree(dev);
}

static int dm2_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_dm2 *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	size_t buffer_size;
	int i;
	int retval = -ENOMEM;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		err("Out of memory");
		goto error;
	}
	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	dev->lock = __SPIN_LOCK_UNLOCKED();

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;

	/* set up the endpoint information */
	/* use only the first int-in and int-out endpoints */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;
		if (!dev->int_in_endpointAddr &&
		    usb_endpoint_is_int_in(endpoint)) {
			/* we found a int in endpoint */
			buffer_size = le16_to_cpu(endpoint->wMaxPacketSize);
			dev->int_in_size = buffer_size;
			dev->int_in_endpointAddr = endpoint->bEndpointAddress;
			dev->int_in_interval = endpoint->bInterval;
			dev->int_in_buffer = kmalloc(buffer_size, GFP_KERNEL);
			if (!dev->int_in_buffer) {
				err("Could not allocate int_in_buffer");
				goto error;
			}
		}
#ifdef USE_BULK_SNDPIPE
		// Compatibility code for older kernels:
		if (!dev->int_out_endpointAddr &&
		    usb_endpoint_is_bulk_out(endpoint)) {
			/* we found a bulk out endpoint */
			dev->int_out_endpointAddr = endpoint->bEndpointAddress;
		}
#else
		if (!dev->int_out_endpointAddr &&
		    usb_endpoint_is_int_out(endpoint)) {
			/* we found an int out endpoint */
			dev->int_out_endpointAddr = endpoint->bEndpointAddress;
		}
#endif
	}
	if (!(dev->int_in_endpointAddr && dev->int_out_endpointAddr)) {
		err("Could not find both int-in and int-out endpoints");
		goto error;
	}

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	retval = dm2_setup_writer(dev);
	if (retval) {
		err("Problem setting up the writer.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	retval = dm2_setup_reader(dev);
	if (retval) {
		err("Problem setting up the reader.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	retval = dm2_midi_init(dev);
	if (retval) {
		err("Problem setting up MIDI.");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	dm2_internal_init(&(dev->dm2), &(dm2_params[0]));


	info("Mixman DM2 device now attached.");
	return 0;

error:
	if (dev)
		/* this frees allocated memory */
		kref_put(&dev->kref, dm2_delete);
	return retval;
}

static void dm2_disconnect(struct usb_interface *interface)
{
	struct usb_dm2 *dev;
	unsigned long flags;

	dev = usb_get_intfdata(interface);

	/* prevent dm2_open() from racing dm2_disconnect() */
	spin_lock_irqsave(&dev->lock, flags);

	usb_set_intfdata(interface, NULL);
	/* prevent more I/O from starting */
	dev->interface = NULL;

	spin_unlock_irqrestore(&dev->lock, flags);

	/* decrement our usage count */
	kref_put(&dev->kref, dm2_delete);

	dm2_midi_destroy(dev);

	info("Mixman DM2 now disconnected");
}

static struct usb_driver dm2_driver = {
	.name =		"Mixman DM2",
	.probe =	dm2_probe,
	.disconnect =	dm2_disconnect,
	.id_table =	dm2_table,
	.supports_autosuspend = 0,
};

static int __init usb_dm2_init(void)
{
	int result;

	/* register this driver with the USB subsystem */
	result = usb_register(&dm2_driver);
	if (result)
		err("usb_register failed. Error number %d", result);

	return result;
}

static void __exit usb_dm2_exit(void)
{
	/* deregister this driver with the USB subsystem */
	usb_deregister(&dm2_driver);
}

module_init(usb_dm2_init);
module_exit(usb_dm2_exit);

MODULE_LICENSE("GPL");
