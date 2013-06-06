/*
 * dm2.h  -  Mixman DM2 stateful MIDI driver header file
 *
 *
 * Copyright (C) 2007-2008 Jan Jockusch (jan@jockusch.de)
 * Copyright (C) 2006-2007 Andre Roth <lynx@netlabs.org>
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * $Id: dm2.h,v 1.15 2008/01/19 18:48:28 jan Exp $
 *
 */


/* Structure with complete parameter set for a DM2. */
/* Use this to encode program sets and to set the */
/* SysEx message. All values are thus 7 bit values! */

struct dm2_params {
	// Slider parameters:  X  Y  Fader
	u8 sliderparam[3];

	u8 sliderdeadzone;
	u8 paramthresh;
	u8 cursorthresh;

	u8 wheel0jogparam;
	u8 wheel1jogparam;
	// Wheel button Notes/Params:  NW   W  SW   S  SE   E  NE   N
	u8 wheel0notes[8];
	u8 wheel0params[8];
	u8 wheel1notes[8];
	u8 wheel1params[8];
	// Use parameters in relative mode: nn NW  W  SW  SE  E  NE  N
	u8 relparams0, relparams1;
	// Disable toggle mode on which keys: nn NW  W  SW  SE  E  NE  N
	u8 notoggle0, notoggle1;
	// First button set: Stop  Play  Rec  T3  T2  T1   R   L
	u8 buttons0[8];
	// Second button set: nn Mid  B  A  B4  B3  B2  B1
	u8 buttons1[8];
	// Mid button up/down keys, on-release keys
	u8 midup0, middown0, midup1, middown1, midrel0, midrel1;
	// Exclusive mode? (only one param at a time)
	u8 excl0, excl1;

	// Notes to activate LEDs: NW  W  SW  S  SE  E  NE  N
	u8 led0notes[8];
	u8 led1notes[8];
	// Activate/deactivate idle loop
	u8 led0idle, led1idle;
};

/* How to parameterize LED keys:
 *
 * allowed combination       meaning
 * notoggle note  param
 * off      set   unset      press: note on; release: note off.
 * off      unset set        press: wheel into param mode, lock. 2nd release: unlock
 * on       unset set        press: wheel into param mode. release: nothing
 * off      set   set        press: wheel into param mode, lock. 2nd release: note on if wheel turned, unlock
 * on       set   set        press: wheel into param mode. release: note on if no wheel turn.
 */

#define DM2_NUMPRESETS 3
static struct dm2_params dm2_params[DM2_NUMPRESETS] = {
	{ // Program 0: Default program (for Mixxx)
		.sliderparam = {4, 5, 2},
		.sliderdeadzone = 5,
		.paramthresh = 4,
		.cursorthresh = 12,
		.wheel0jogparam = 1,
		.wheel1jogparam = 3,
		//                NW   W  SW   S  SE   E  NE   N
		.wheel0notes =  { 16, 17, 18,  0, 20, 21, 22,  0 },
		.wheel0params = { 16, 17, 18,  0, 20, 21, 22, 23 },
		.wheel1notes =  { 32, 33, 34,  0, 36, 37, 38,  0 },
		.wheel1params = { 32, 33, 34,  0, 36, 37, 38, 39 },
		// All params in absolute mode.
		.relparams0 = 0,
		.relparams1 = 0,
		// Disable toggle mode on which keys: nn NW  W  SW  SE  E  NE  N
		.notoggle0 = 0x3f,
		.notoggle1 = 0x3f,
		// First button set: Stop  Play  Rec  T3  T2  T1   R   L
		.buttons0 =     { 48, 49, 50, 51, 52, 53, 54, 55 },
		//                nn Mid   B   A  B4  B3  B2  B1
		.buttons1 =     {  0,  0, 58, 59, 60, 61, 62, 63 },
		// Mid button up/down keys, on-release keys
		.midup0 = 65,
		.midup1 = 65,
		.middown0 = 66,
		.middown1 = 66,
		.midrel0 = 67,
		.midrel1 = 68,
		// Exclusive mode? (only one param at a time)
		.excl0 = 1, .excl1 = 1,
		// LED buttons activated by these notes:
		.led0notes =  { 64, 65, 66, 67, 68, 69, 70, 71 },
		.led1notes =  { 80, 81, 82, 83, 84, 85, 86, 87 },
		.led0idle = 88, .led1idle = 89
	},
	{ // Program 1: Simple program (only CC multiplexing with toggle switches)
		.sliderparam = {4, 5, 2},
		.sliderdeadzone = 5,
		.paramthresh = 4,
		.cursorthresh = 12,
		.wheel0jogparam = 1,
		.wheel1jogparam = 3,
		//            NW   W  SW   S  SE   E  NE   N
		.wheel0notes =  {  0,  0,  0,  0,  0,  0,  0,  0 },
		.wheel0params = { 16, 17, 18,  0, 20, 21, 22, 23 },
		.wheel1notes =  {  0,  0,  0,  0,  0,  0,  0,  0 },
		.wheel1params = { 32, 33, 34,  0, 36, 37, 38, 39 },
		// All params in absolute mode.
		.relparams0 = 0,
		.relparams1 = 0,
		// Disable toggle mode on which keys: nn NW  W  SW  SE  E  NE  N
		.notoggle0 = 0x00,
		.notoggle1 = 0x00,
		// First button set: Stop  Play  Rec  T3  T2  T1   R   L
		.buttons0 =     { 48, 49, 50, 51, 52, 53, 54, 55 },
		//                     nn Mid   B   A  B4  B3  B2  B1
		.buttons1 =     {  0,  0, 58, 59, 60, 61, 62, 63 },
		// Mid button up/down keys, on-release keys
		.midup0 = 65,
		.midup1 = 65,
		.middown0 = 66,
		.middown1 = 66,
		.midrel0 = 67,
		.midrel1 = 68,
		// Exclusive mode? (only one param at a time)
		.excl0 = 0, .excl1 = 0,
		// LED buttons activated by these notes:
		.led0notes =  { 64, 65, 66, 67, 68, 69, 70, 71 },
		.led1notes =  { 80, 81, 82, 83, 84, 85, 86, 87 },
		.led0idle = 88, .led1idle = 89
	},
	{ // Program 2: Cinelerra, only relative controls
		.sliderparam = {4, 5, 2},
		.sliderdeadzone = 5,
		.paramthresh = 6,
		.cursorthresh = 20,
		.wheel0jogparam = 1,
		.wheel1jogparam = 3,
		//            NW   W  SW   S  SE   E  NE   N
		.wheel0notes =  { 16, 17, 18,  0, 20, 21, 22, 23 },
		.wheel0params = { 16, 17, 18,  0, 20, 21, 22, 23 },
		.wheel1notes =  { 32, 33, 34,  0, 36, 37, 38, 39 },
		.wheel1params = { 32, 33, 34,  0, 36, 37, 38, 39 },
		// All params in relative mode:
		.relparams0 = 0x7f,
		.relparams1 = 0x7f,
		// Disable toggle mode on which keys: nn NW  W  SW  SE  E  NE  N
		.notoggle0 = 0x7f,
		.notoggle1 = 0x7f,
		// First button set: Stop  Play  Rec  T3  T2  T1   R   L
		.buttons0 =     { 48, 49, 50, 51, 52, 53, 54, 55 },
		//                     nn Mid   B   A  B4  B3  B2  B1
		.buttons1 =     {  0,  0, 58, 59, 60, 61, 62, 63 },
		// Mid button up/down keys, on-release keys
		.midup0 = 65,
		.midup1 = 66,
		.middown0 = 67,
		.middown1 = 68,
		.midrel0 = 69,
		.midrel1 = 70,
		// Exclusive mode? (only one param at a time)
		.excl0 = 0, .excl1 = 0,
		// LED buttons activated by these notes:
		.led0notes =  { 64, 65, 66, 67, 68, 69, 70, 71 },
		.led1notes =  { 80, 81, 82, 83, 84, 85, 86, 87 },
		.led0idle = 88, .led1idle = 89
	}
};


struct dm2midi {
	struct snd_card			*card;
	struct snd_rawmidi		*rmidi;
	struct snd_rawmidi_substream	*input;
	struct snd_rawmidi_substream	*output;

	struct tasklet_struct		tasklet;
	int				input_triggered;

	u8		   	chan;		/* MIDI channel */
	u8			out_rstatus;	/* MIDI Running status reminder */
	u8			in_rstatus;	/* same for input */
	u8			in_arg1;	/* 1st argument for input */
};


struct dm2slider {
	u8			pos;		/* Current position */
	u8			min, max, mid;	/* Values for auto-calibration */
	u8			dead;		/* Dead zone width in slider units */
	u8			param;
	u8			midival;
};


#define DM2_MIDINDEX 3
#define DM2_MIDMASK 0x02
#define DM2_CLR 0x08
#define DM2_MID(v) (((v)&DM2_MIDMASK)<<2)


struct dm2wheel {
	u8			pressed;	/* Map of pressed keys */
	u8			light;		/* Which are locked now */
	u8			whenreleased;	/* Which state to assume when released */
	u8			notes[8];	/* Note to be used for each button. 0 disables. */
	u8			params[8];	/* Param for controller. 0 disables. */
	u8			midivals[8];
	u8			relparams;	/* Params which send relative values. */
	u8			notoggle;      	/* Buttons which do not toggle. */
	u8			exclusive;	/* Only one param active at a time */

	u8			paramthresh;	/* Wheel turn threshold for adjusting parameters */
	u8			cursorthresh;	/* Wheel turn threshold for adjusting the cursor */

	u8			jogparam;
	u8			jogmidival;
	u8			midpressed;

	u8			midup;		/* If set: "up" key while mid is pressed */
	u8			middown;	/* If set: "down" key while mid id pressed */
	u8			midrel;		/* If set: key pressed when mid is released */
	u8			wheelused;	/* Set if wheel has turned while holding a key */

	int			showlight;	/* Make sure lights are shown */
	int			turnacc;	/* Turn accumulator before increment is done. */
};


struct dm2buttons {
	u8			pressed;
	u8			notes[8];
};

#define DM2_LEDIDLEINT 20
#define DM2_LEDTIMEOUT 100

struct dm2leds {
	int			timeout;		/* remaining duration of overlay */
	int			wheeltimeout;		/* Wheel should show through */
	int			idletimeout;		/* Delay between idle loop advances */
	u8			curr;			/* current setting */
	u8			wheel;			/* Setting from the wheel buttons */
	u8			mask;			/* LEDs masked by foreign input */
	u8			light;			/* Light setting not from wheels */
	u8			idlelight;		/* State of the idle loop */
	u8			notes[8];		/* Note on/off that we interpret */
	u8			idlenote;		/* Note that switches the idle loop */
};


struct dm2 {
	u8			prev_state[10];
	u8			curr_state[10];
	struct dm2slider	sliders[3];
	int			initialize;	/* Signals that the pots have to be initalized */

	struct dm2wheel		wheels[2];
	struct dm2buttons	buttons[2];
	struct dm2leds		leds[2];
};



/* Vendor and Product ID of the Mixman DM2 */
#define USB_DM2_VENDOR_ID	0x0665
#define USB_DM2_PRODUCT_ID	0x0301

/* table of devices that work with this driver */
static struct usb_device_id dm2_table [] = {
	{ USB_DEVICE(USB_DM2_VENDOR_ID, USB_DM2_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, dm2_table);


#define MAX_TRANSFER		(PAGE_SIZE - 512)
/* MAX_TRANSFER is chosen so that the VM is not stressed by
   allocations > PAGE_SIZE and the number of packets in a page
   is an integer 512 is the largest possible packet on EHCI */
#define WRITES_IN_FLIGHT	8


/* Structure to hold all of our device specific stuff */
struct usb_dm2 {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	unsigned char           *int_in_buffer;		/* the buffer to receive data */
	size_t			int_in_size;		/* the size of the receive buffer */
	__u8			int_in_endpointAddr;	/* the address of the int in endpoint */
	__u8			int_out_endpointAddr;	/* the address of the int/bulk out endpoint */
	int			output_failed;		/* flag which indicates an unpatched kernel */
	struct kref		kref;
	struct urb		*int_in_urb;
	int			int_in_interval;

	struct urb		*int_out_urb;		/* output URB */
	unsigned char           *int_out_buffer;	/* the buffer to send data */

	struct dm2		dm2;
	struct dm2midi          dm2midi;
	spinlock_t		lock;			/* To protect tasklet from irq handler */
};
#define to_dm2_dev(d) container_of(d, struct usb_dm2, kref)


static void dm2_midi_send(struct usb_dm2 *, u8, u8, u8);
static void dm2_set_leds(struct usb_dm2 *, u8, u8);

static void dm2_delete(struct kref *);
