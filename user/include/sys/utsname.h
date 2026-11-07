#ifndef HARIS_H_SYS_UTSNAME_H
#define HARIS_H_SYS_UTSNAME_H 1
#define HARIS_STD_HEADERS 1
struct utsname
{
    char sysname[65], nodename[65], release[65], version[65], machine[65];
};
int uname(struct utsname *u);
#endif
