/* Thor port: getsectiondata over the running executable's ELF section headers, and proc_pidinfo over /proc.
   The section headers are not loaded at run time, so /proc/self/exe is read once; a section's address is its sh_addr
   plus the executable's load bias (AT_PHDR minus the program headers' file offset). */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include "mach-o/getsect.h"
#include "libproc.h"

const struct mach_header_64 _mh_execute_header = { 0xfeedfacf };

typedef struct { char name[64]; uint64_t addr, size; } section_t;
static section_t *g_sections; static int g_nsections;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static int read_at(int fd, void *p, size_t n, off_t off) { return pread(fd, p, n, off) == (ssize_t)n; }
static void load_sections(void) {
    int fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC); if (fd < 0) return;
    Elf64_Ehdr eh;
    if (!read_at(fd, &eh, sizeof eh, 0) || memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_shentsize != sizeof(Elf64_Shdr) || eh.e_shstrndx >= eh.e_shnum) { close(fd); return; }
    Elf64_Shdr *sh = calloc(eh.e_shnum, sizeof *sh);
    if (!sh || !read_at(fd, sh, (size_t)eh.e_shnum * sizeof *sh, (off_t)eh.e_shoff)) { free(sh); close(fd); return; }
    char *names = malloc(sh[eh.e_shstrndx].sh_size + 1);
    if (!names || !read_at(fd, names, sh[eh.e_shstrndx].sh_size, (off_t)sh[eh.e_shstrndx].sh_offset)) { free(names); free(sh); close(fd); return; }
    names[sh[eh.e_shstrndx].sh_size] = 0;
    uintptr_t bias = (uintptr_t)getauxval(AT_PHDR) - (uintptr_t)eh.e_phoff;   /* PIE: the first PT_LOAD maps file offset 0 at vaddr 0 + bias */
    g_sections = calloc(eh.e_shnum, sizeof *g_sections);
    for (int i = 0; g_sections && i < eh.e_shnum; i++) {
        if (!sh[i].sh_addr || sh[i].sh_name >= sh[eh.e_shstrndx].sh_size) continue;
        section_t *s = &g_sections[g_nsections++];
        snprintf(s->name, sizeof s->name, "%s", names + sh[i].sh_name); s->addr = bias + sh[i].sh_addr; s->size = sh[i].sh_size;
    }
    free(names); free(sh); close(fd);
}
uint8_t *getsectiondata(const struct mach_header_64 *mh, const char *segname, const char *sectname, unsigned long *size) {
    (void)mh; (void)segname; pthread_once(&g_once, load_sections);
    for (int i = 0; i < g_nsections; i++)
        if (!strcmp(g_sections[i].name, sectname)) { if (size) *size = (unsigned long)g_sections[i].size; return (uint8_t *)(uintptr_t)g_sections[i].addr; }
    if (size) *size = 0;
    return NULL;
}

int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize) {
    (void)arg;
    if (flavor != PROC_PIDTBSDINFO || !buffer || buffersize < (int)sizeof(struct proc_bsdinfo)) { errno = EINVAL; return 0; }
    char path[64], line[512]; snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "re"); if (!f) return 0;
    int ok = fgets(line, sizeof line, f) != NULL; fclose(f); if (!ok) return 0;
    char *rp = strrchr(line, ')'); if (!rp || rp[1] != ' ') return 0;
    struct proc_bsdinfo *b = buffer; memset(b, 0, sizeof *b); b->pbi_pid = (uint32_t)pid;
    b->pbi_status = rp[2] == 'Z' || rp[2] == 'X' ? SZOMB : 2;   /* 2: SRUN */
    /* field 22 (starttime, clock ticks after boot), turned into seconds since the epoch */
    unsigned long long start = 0; const char *q = rp + 2; for (int field = 3; field < 22 && q; field++) { q = strchr(q, ' '); if (q) q++; }
    if (q && sscanf(q, "%llu", &start) == 1) {
        struct timespec now, up; clock_gettime(CLOCK_REALTIME, &now); clock_gettime(CLOCK_BOOTTIME, &up);
        long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
        b->pbi_start_tvsec = (uint64_t)(now.tv_sec - up.tv_sec) + start / (unsigned long long)hz;
    }
    return (int)sizeof *b;
}
int proc_pidpath(int pid, void *buffer, uint32_t buffersize) {
    char path[64]; snprintf(path, sizeof path, "/proc/%d/exe", pid);
    ssize_t n = buffersize ? readlink(path, buffer, buffersize - 1) : -1;
    if (n <= 0) return 0;
    ((char *)buffer)[n] = 0; return (int)n;
}
int proc_listallpids(void *buffer, int buffersize) {
    DIR *d = opendir("/proc"); if (!d) return 0;
    int n = 0, cap = buffer ? buffersize / (int)sizeof(int) : 0; struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        if (buffer && n < cap) ((int *)buffer)[n] = atoi(e->d_name);
        n++;
    }
    closedir(d);
    return buffer && n > cap ? cap : n;
}
