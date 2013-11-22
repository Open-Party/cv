/*
   Copyright (C) 2013 Xfennec, CQFD Corp.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <libgen.h>

#include "cv.h"
#include "sizes.h"

char *proc_names[]={"cp", "mv", "dd", "cat", NULL};
//~ char *proc_names[]={"chrome", "firefox", NULL};

signed char is_numeric(char *str)
{
while(*str) {
    if(!isdigit(*str))
        return 0;
    str++;
}
return 1;
}

int find_pids_by_binary_name(char *bin_name, pidinfo_t *pid_list, int max_pids)
{
DIR *proc;
struct dirent *direntp;
struct stat stat_buf;
char fullpath_dir[MAXPATHLEN + 1];
char fullpath_exe[MAXPATHLEN + 1];
char exe[MAXPATHLEN + 1];
ssize_t len;
int pid_count=0;

proc=opendir(PROC_PATH);
if(!proc) {
    perror("opendir");
    fprintf(stderr,"Can't open %s\n",PROC_PATH);
    exit(EXIT_FAILURE);
}

while((direntp = readdir(proc)) != NULL) {
    snprintf(fullpath_dir, MAXPATHLEN, "%s/%s", PROC_PATH, direntp->d_name);

    if(stat(fullpath_dir, &stat_buf) == -1) {
        perror("stat (find_pids_by_binary_name)");
        continue;
    }

    if((S_ISDIR(stat_buf.st_mode) && is_numeric(direntp->d_name))) {
        snprintf(fullpath_exe, MAXPATHLEN, "%s/exe", fullpath_dir);
        len=readlink(fullpath_exe, exe, MAXPATHLEN);
        if(len != -1)
            exe[len] = 0;
        else {
            // Will be mostly "Permission denied"
            //~ perror("readlink");
            continue;
        }

        if(!strcmp(basename(exe), bin_name)) {
            pid_list[pid_count].pid=atol(direntp->d_name);
            strcpy(pid_list[pid_count].name, bin_name);
            pid_count++;
            if(pid_count==max_pids)
                break;
        }
    }
}

closedir(proc);
return pid_count;
}

int find_fd_for_pid(pid_t pid, int *fd_list, int max_fd)
{
DIR *proc;
struct dirent *direntp;
char path_dir[MAXPATHLEN + 1];
char fullpath[MAXPATHLEN + 1];
char link_dest[MAXPATHLEN + 1];
struct stat stat_buf;
int count = 0;
ssize_t len;

snprintf(path_dir, MAXPATHLEN, "%s/%d/fd", PROC_PATH, pid);

proc=opendir(path_dir);
if(!proc) {
    perror("opendir");
    fprintf(stderr,"Can't open %s\n",path_dir);
    return 0;
}

while((direntp = readdir(proc)) != NULL) {
    snprintf(fullpath, MAXPATHLEN, "%s/%s", path_dir, direntp->d_name);
    if(stat(fullpath, &stat_buf) == -1) {
        perror("stat (find_fd_for_pid)");
        continue;
    }

    // if not a regular file ...
    if(!S_ISREG(stat_buf.st_mode))
        continue;

    // try to read link ...
    len=readlink(fullpath, link_dest, MAXPATHLEN);
    if(len != -1)
        link_dest[len] = 0;
    else
        continue;

    // try to stat link target (invalid link ?)
    if(stat(link_dest, &stat_buf) == -1)
        continue;

    // OK, we've found a potential interesting file.

    fd_list[count++] = atoi(direntp->d_name);
    //~ printf("[debug] %s\n",fullpath);
    if(count == max_fd)
        break;
}

closedir(proc);
return count;
}


signed char get_fdinfo(pid_t pid, int fdnum, fdinfo_t *fd_info)
{
struct stat stat_buf;
char fdpath[MAXPATHLEN + 1];
char line[LINE_LEN];
ssize_t len;
FILE *fp;

snprintf(fdpath, MAXPATHLEN, "%s/%d/fd/%d", PROC_PATH, pid, fdnum);

len=readlink(fdpath, fd_info->name, MAXPATHLEN);
if(len != -1)
    fd_info->name[len] = 0;
else {
    //~ perror("readlink");
    return 0;
}

if(stat(fd_info->name, &stat_buf) == -1) {
    //~ printf("[debug] %i - %s\n",pid,fd_info->name);
    perror("stat (get_fdinfo)");
    return 0;
}

fd_info->size = stat_buf.st_size;
fd_info->pos = 0;

snprintf(fdpath, MAXPATHLEN, "%s/%d/fdinfo/%d", PROC_PATH, pid, fdnum);
fp = fopen(fdpath, "rt");

while(fgets(line, LINE_LEN - 1, fp) != NULL) {
    line[4]=0;
    if(!strcmp(line, "pos:")) {
        fd_info->pos = atol(line + 5);
        break;
    }
}

return 1;
}

// TODO: deal with --help

int main(int argc, char *argv[])
{
int pid_count, fd_count;
int i,j;
pidinfo_t pidinfo_list[MAX_PIDS];
fdinfo_t fdinfo;
int fdnum_list[MAX_FD_PER_PID];
off_t max_size;
int biggest_fd;
char fsize[64];
char fpos[64];

pid_count = 0;

for(i = 0 ; proc_names[i] ; i++) {
    pid_count += find_pids_by_binary_name(proc_names[i],
                                          pidinfo_list + pid_count,
                                          MAX_PIDS - pid_count);
    if(pid_count >= MAX_PIDS) {
        fprintf(stderr,"Found too much procs (max = %d)\n",MAX_PIDS);
        break;
    }
}

if(!pid_count) {
    fprintf(stderr,"No interesting command currently running.\n");
    return 0;
}

for(i = 0 ; i < pid_count ; i++) {
    fd_count = find_fd_for_pid(pidinfo_list[i].pid, fdnum_list, MAX_FD_PER_PID);

    max_size = 0;
    biggest_fd = -1;

    // let's find the biggest opened file
    for(j = 0 ; j < fd_count ; j++) {
        get_fdinfo(pidinfo_list[i].pid, fdnum_list[j], &fdinfo);
        if(fdinfo.size > max_size) {
            biggest_fd = j;
            max_size = fdinfo.size;
        }
    }

    if(biggest_fd < 0) { // nothing found
        printf("[%5d] %s inactive or flushing\n",
                pidinfo_list[i].pid,
                pidinfo_list[i].name);
        continue;
    }

    // should use a cache ... (shame)
    get_fdinfo(pidinfo_list[i].pid, fdnum_list[biggest_fd], &fdinfo);

    format_size(fdinfo.pos, fpos);
    format_size(fdinfo.size, fsize);

    printf("[%5d] %s %s %.1f%% (%s / %s)\n",
        pidinfo_list[i].pid,
        pidinfo_list[i].name,
        fdinfo.name,
        ((double)100 / (double)fdinfo.size) * (double)fdinfo.pos,
        fpos,
        fsize);
}

return 0;
}