/* The Bluetooth app, for other parts of the system.
 *
 * bluetooth_app_send_file() opens the Bluetooth window (or brings the open one to the front) with `path` (a file in the virtual file system) in its
 * "File" box. When a device that takes files is connected, the file is sent at once; else the window says to connect one first and the user presses
 * Send. Returns NULL when it worked, else a short sentence saying why not. */
#ifndef HARIS_BLUETOOTHAPP_H
#define HARIS_BLUETOOTHAPP_H

const char *bluetooth_app_send_file(const char *path);

#endif
