/* ============================================================
 * HarisOS sound: Intel High Definition Audio (HDA) controller + codec driver
 *
 *   hda_init();                  once, after pci_scan() and serial_init(); finds every HDA controller
 *   hda_ready();                 1 = a controller with a usable output (speaker / headphone / line out) was found
 *   hda_play_tone(440, 500);     a sine tone, blocks until it has played (polled, no interrupts)
 *   hda_status(i, loc, buf, n);  one line per controller for the Device Manager
 *
 * How HDA works: the controller on PCI (class 04, subclass 03) is only a DMA engine and a serial link. The sound
 * chips ("codecs", e.g. a Realtek ALC236, or the HDMI audio of a GPU) hang on that link at addresses 0..14.
 * Software talks to a codec with 32-bit commands ("verbs") put in a ring in RAM (CORB); the answers come back in
 * a second ring (RIRB). Inside a codec is a graph of widgets: DACs (digital to analog), mixers, selectors and pins
 * (the jacks and the speaker). To play, we find a path pin <- ... <- DAC, unmute it, give the DAC a stream number,
 * and let one of the controller's output stream engines read samples from a buffer list (BDL) in RAM. Every
 * stream frame sent on the link carries the stream number, and the DAC takes the samples of its own number.
 *
 * Tested in QEMU (-device intel-hda -device hda-duplex). Real hardware may need controller quirks (snoop,
 * position fix) that are not done yet: the Device Manager line says how far it got.
 * ============================================================ */
#ifndef HARIS_HDA_H
#define HARIS_HDA_H

#define HDA_MAX_CTRL 4

void hda_init(void);
int hda_ready(void);

/* Plays a sine tone of `hz` for `ms` milliseconds at 48 kHz, 16-bit stereo, on every output the codec has.
 * 0 = played, <0 = no output / the stream did not move. Blocks the caller. */
int hda_play_tone(unsigned hz, unsigned ms);

/* Controllers found (0..HDA_MAX_CTRL). For each: PCI location (bus, dev, fn), whether it works, and one line. */
int hda_count(void);
int hda_status(int index, unsigned char loc[3], int *working, char *buf, int cap);

/* ---- pure helpers (host-tested) ---- */
/* sin(2*pi * phase / 2^32) in Q15 (-32767..32767), error below 8 / 32767 */
int hda_sine_q15(unsigned phase);
/* stream format word for PCM (spec 3.7.1): 48000 or 44100 Hz (or 8000..192000 through mult / div), 8/16/20/24/32
 * bits, 1..16 channels. 0 = not representable. */
unsigned short hda_stream_format(unsigned rate, unsigned bits, unsigned channels);

#endif
