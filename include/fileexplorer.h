/* File Explorer, for other parts of the system: show a folder in it. */
#ifndef HARIS_FILEEXPLORER_H
#define HARIS_FILEEXPLORER_H

/* Open the File Explorer (or bring the open one to the front) and go to the folder `node` (a vfs node; nothing if it is not a folder). */
void fileexplorer_open_folder(int node);

#endif
