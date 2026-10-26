/* Thor port: proc_pidinfo(PROC_PIDTBSDINFO) for "is this process alive" (game_slot.c), from /proc/<pid>/stat. */
#pragma once
#include <stdint.h>
#define PROC_PIDTBSDINFO 3
#define PROC_PIDPATHINFO_MAXSIZE 4096
#define SZOMB 5
struct proc_bsdinfo { uint32_t pbi_flags, pbi_status, pbi_xstatus, pbi_pid, pbi_ppid; char pbi_comm[16]; uint64_t pbi_start_tvsec, pbi_start_tvusec; };
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_pidpath(int pid, void *buffer, uint32_t buffersize);   /* readlink /proc/<pid>/exe */
int proc_listallpids(void *buffer, int buffersize);   /* the numeric entries of /proc; with no buffer, how many there are */
