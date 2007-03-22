/*
 *  Copyright (c) 2007 XenSource Inc.
 *  All rights reserved.
 *
 */

/*
 * This module implements a "dot locking" style advisory file locking algorithm.
 */

/* TODO: 
  o deal w/ errors better
  o return error codes(?) through errno(?)
  o decide if we want random/fixed sleep on retry?
 */

#if !defined(EXCLUSIVE_LOCK)
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include "lock.h"

#define unlikely(x) __builtin_expect(!!(x), 0)

/* format: xenlk.hostname.uuid.<xf><rw>*/
#define LF_POSTFIX ".xenlk"
#define LFXL_FORMAT LF_POSTFIX ".%s.%s.x%s"
#define LFFL_FORMAT LF_POSTFIX ".%s.%s.f%s"
#define RETRY_MAX 16

#if defined(LOGS)
#define LOG(format, args...) printf("%d: ", __LINE__); printf(format, ## args)
#else
#define LOG(format, args...)
#endif

/* random wait - up to .5 seconds */
#define XSLEEP usleep(random() & 0x7ffff)

typedef int (*eval_func)(char *name, int readonly);

static char *create_lockfn(char *fn_to_lock)
{
        char *lockfn;
    
        /* allocate string to hold constructed lock file */
        lockfn = malloc(strlen(fn_to_lock) + strlen(LF_POSTFIX) + 1);
        if (unlikely(!lockfn)) {
                return 0;
        }

        /* append postfix to file to lock */
        strcpy(lockfn, fn_to_lock);
        strcat(lockfn, LF_POSTFIX);

        return lockfn;
}

static char *create_lockfn_link(char *fn_to_lock, char *format, char *uuid, int readonly)
{
        char hostname[128];
        char *lockfn_link;
        char *ptr;

        /* get hostname */
        if (unlikely(gethostname(hostname, sizeof(hostname)) == -1)) {
                return 0;
        }

        /* allocate string to hold constructed lock file link */
        lockfn_link = malloc(strlen(fn_to_lock) + strlen(LF_POSTFIX) +
                             strlen(hostname) + strlen(uuid) + 8);
        if (unlikely(!lockfn_link)) {
                return 0;
        }

        /* construct lock file link with specific format */
        strcpy(lockfn_link, fn_to_lock);
        ptr = lockfn_link + strlen(lockfn_link);
        sprintf(ptr, format, hostname, uuid, readonly ? "r" : "w");

        return lockfn_link;
}

static int writer_eval(char *name, int readonly) 
{
        return name[strlen(name)-1] == 'w';
}

static int reader_eval(char *name, int readonly) 
{
        return name[strlen(name)-1] == 'r' && !readonly;
}

static int lock_holder(char *fn, char *lockfn, char *lockfn_link, 
                       int force, int readonly, int *stole, eval_func eval)
{
        int status = 0;
        int ustat;
        DIR *pd = 0;
        struct dirent *dptr;
        char *ptr;
        char *dirname = malloc(strlen(lockfn));
        char *uname = malloc(strlen(lockfn_link) + 8);

        *stole = 0;

        if (!dirname) goto finish;
        if (!uname) goto finish;
        
        /* get directory */
        ptr = strrchr(lockfn, '/');
        if (!ptr) {
                strcpy(dirname, ".");
        } else {
                int numbytes = ptr - lockfn;
                strncpy(dirname, lockfn, numbytes);
        }
        pd = opendir(dirname); 
        if (!pd) goto finish;

        /* 
         * scan through directory entries and use eval function 
         * if we have a match (i.e. reader or writer lock) but
         * note that if we are forcing, we will remove any and
         * all locks that appear for target of our lock, regardless
         * if it a reader/writer owns the lock.
         */
        dptr = readdir(pd);
        while (dptr) {
                char *p1 = strrchr(fn, '/');
                char *p2 = strrchr(lockfn, '/');
                char *p3 = strrchr(lockfn_link, '/');
                if (p1) p1+=1;
                if (p2) p2+=1;
                if (p3) p3+=1;
                if (strcmp(dptr->d_name, p1 ? p1 : fn) &&
                    strcmp(dptr->d_name, p2 ? p2 : lockfn) &&
                    strcmp(dptr->d_name, p3 ? p3 : lockfn_link) &&
                    !strncmp(dptr->d_name, p1 ? p1 : fn, strlen(p1 ? p1 : fn))) {
                        if (force) {
                                strcpy(uname, dirname);
                                strcat(uname, "/");
                                strcat(uname, dptr->d_name);
                                ustat = unlink(uname);
                                if (ustat == -1) {
                                        LOG("failed to unlink %s\n", uname);
                                }
                                *stole = 1;
                        } else {
                                if ((*eval)(dptr->d_name, readonly)) {
                                        closedir(pd);
                                        status = 1;
                                        goto finish;
                                }
                        }
                }
                dptr = readdir(pd);
        }

        closedir(pd);

finish:
        free(dirname);
        free(uname);

        return status;
}

int lock(char *fn_to_lock, char *uuid, int force, int readonly)
{
        char *lockfn = 0;
        char *lockfn_xlink = 0;
        char *lockfn_flink = 0;
        char *buf = 0;
        int fd;
        int status = 0;
        struct stat stat1, stat2;
        int retry_attempts = 0;
        int clstat;
        int tmpstat;
        int stealx = 0;
        int stealw = 0;
        int stealr = 0;
    
        if (!fn_to_lock || !uuid)
                return LOCK_EBADPARM;

        /* seed random with time/pid combo */
        srandom((int)time(0) ^ getpid());

        /* build lock file strings */
        lockfn = create_lockfn(fn_to_lock);
        if (unlikely(!lockfn)) { status = LOCK_ENOMEM; goto finish; }

        lockfn_xlink = create_lockfn_link(fn_to_lock, LFXL_FORMAT, uuid, readonly);
        if (unlikely(!lockfn_xlink)) { status = LOCK_ENOMEM; goto finish; }

        lockfn_flink = create_lockfn_link(fn_to_lock, LFFL_FORMAT, uuid, readonly);
        if (unlikely(!lockfn_flink)) { status = LOCK_ENOMEM; goto finish; }

try_again:
        if (retry_attempts++ > RETRY_MAX) goto finish;

        /* try to open exlusive lockfile */
        fd = open(lockfn, O_WRONLY | O_CREAT | O_EXCL, 0644); 
        if (fd == -1) {

                LOG("Initial lockfile creation failed %s force=%d, errno=%d\n",
                     lockfn, force, errno);
                /* already owned? (hostname & uuid match, skip time bits) */
                fd = open(lockfn, O_RDWR, 0644);
                if (fd != -1) {
                        buf = malloc(strlen(lockfn_xlink)+1);
                        if (!buf) {
                                clstat = close(fd);
                                if (unlikely(clstat == -1)) {
                                        LOG("fail on close\n");
                                }
                                status = LOCK_ENOMEM;
                                goto finish;
                        }
                        if (read(fd, buf, strlen(lockfn_xlink)) !=
                           (strlen(lockfn_xlink))) {
                                clstat = close(fd);
                                if (unlikely(clstat == -1)) {
                                        LOG("fail on close\n");
                                }
                                free(buf);
                                goto force_lock;
                        }
                        if (!strncmp(buf, lockfn_xlink, strlen(lockfn_xlink)-1)) {
                                LOG("lock owned by us, reasserting\n");
                                /* our lock, reassert by rewriting below */
                                if (lseek(fd, 0, SEEK_SET) == -1) {
                                        clstat = close(fd);
                                        if (unlikely(clstat == -1)) {
                                                LOG("fail on close\n");
                                        }
                                        goto force_lock;
                                }
                                free(buf);
                                goto skip;
                        }
                        free(buf);
                        clstat = close(fd);
                        if (unlikely(clstat == -1)) {
                                LOG("fail on close\n");
                        }
                }
force_lock:
                if (force) {
                        /* remove lock file, we are forcing lock, try again */
                        status = unlink(lockfn);
                        if (unlikely(status == -1)) {
                                LOG("force removal of %s lockfile failed, "
                                    "errno=%d, trying again\n", lockfn, errno);
                        }
                        stealx = 1;
                }
                XSLEEP;
                status = LOCK_EXLOCK_OPEN;
                goto try_again;
        }

        LOG("lockfile created %s\n", lockfn);

skip:
        /* 
         * write into the temporary xlock
         */
        if (write(fd, lockfn_xlink, strlen(lockfn_xlink)) != 
                strlen(lockfn_xlink)) {
                clstat = close(fd);
                if (unlikely(clstat == -1)) {
                        LOG("fail on close\n");
                }
                XSLEEP;
                status = LOCK_EXLOCK_WRITE;
                if (unlink(lockfn) == -1)  {
                        LOG("removal of %s lockfile failed, "
                            "errno=%d, trying again\n", lockfn, errno);
                }
                goto try_again;
        }
        clstat = close(fd);
        if (unlikely(clstat == -1)) {
                LOG("fail on close\n");
        }

        while (retry_attempts++ < RETRY_MAX) {
                tmpstat = link(lockfn, lockfn_xlink);
                LOG("linking %s and %s\n", lockfn, lockfn_xlink);
                if ((tmpstat == -1) && (errno != EEXIST)) { 
                        LOG("link status is %d, errno=%d\n", tmpstat, errno); 
                }

                if ((lstat(lockfn, &stat1) == -1) || 
                    (lstat(lockfn_xlink, &stat2) == -1)) {
                        /* try again, cleanup first */
                        tmpstat = unlink(lockfn);
                        if (unlikely(tmpstat == -1)) {
                                LOG("error removing lock file %s", lockfn);
                        }
                        tmpstat = unlink(lockfn_xlink);
                        if (unlikely(tmpstat == -1)) {
                                LOG("error removing linked lock file %s", 
                                    lockfn_xlink);
                        }
                        XSLEEP;
                        status = LOCK_ESTAT;
                        goto finish;
                }

                /* compare inodes */
                if (stat1.st_ino == stat2.st_ino) {
                        /* success, inodes are the same */
                        /* should we check that st_nlink's are also 2?? */
                        status = LOCK_OK;
                        tmpstat = unlink(lockfn_xlink);
                        if (unlikely(tmpstat == -1)) {
                                LOG("error removing linked lock file %s", 
                                    lockfn_xlink);
                        }
                        goto finish;
                } else {
                        /* try again, cleanup first */
                        tmpstat = unlink(lockfn);
                        if (unlikely(tmpstat == -1)) {
                                LOG("error removing lock file %s", lockfn);
                        }
                        tmpstat = unlink(lockfn_xlink);
                        if (unlikely(tmpstat == -1)) {
                                LOG("error removing linked lock file %s", 
                                    lockfn_xlink);
                        }
                        XSLEEP;
                        status = LOCK_EINODE;
                        goto try_again;
                }
        }

finish:
        if (!status) {

                /* we have exclusive lock */

                /* fast check, see if we own a final lock and are reasserting */
                if (!lstat(lockfn_flink, &stat1)) 
                        goto skip_scan;

                /* we allow exclusive writer, or multiple readers */
                if (lock_holder(fn_to_lock, lockfn, lockfn_flink, force,
                                     readonly, &stealw, writer_eval)) {
                        status = LOCK_EHELD_WR;
                } else if (lock_holder(fn_to_lock, lockfn, lockfn_flink, force,
                                     readonly, &stealr, reader_eval)) {
                        status = LOCK_EHELD_RD;
                } 
        }

skip_scan:
        if (!status) {
                /* update file, changes last modify time */
                fd = open(lockfn_flink, O_WRONLY | O_CREAT, 0644); 
                if (fd == -1) {
                        status = LOCK_EOPEN;
                } else {
                        if (write(fd, lockfn_flink, strlen(lockfn_flink)) != 
                                  strlen(lockfn_flink)) {
                                clstat = close(fd);
                                if (unlikely(clstat == -1)) {
                                        LOG("fail on close\n");
                                }
                                XSLEEP;
                                status = LOCK_EUPDATE;
                                goto try_again;
                        }
                }
                clstat = close(fd);
                if (unlikely(clstat == -1)) {
                        LOG("fail on close\n");
                }
        }

        if (!status && force && (stealx || stealw || stealr)) {
                struct timeval timeout;

                /* enforce quiet time on steal */
                timeout.tv_sec = LEASE_TIME_SECS;
                timeout.tv_usec = 0;
                select(0, 0, 0, 0, &timeout);
        }

        /* remove exclusive lock, final read/write locks will hold */
        tmpstat = unlink(lockfn);
        if (unlikely(tmpstat == -1)) {
                LOG("error removing exclusive lock file %s", 
                    lockfn);
        }

        free(lockfn);
        free(lockfn_xlink);
        free(lockfn_flink);

        LOG("returning status %d, errno=%d\n", status, errno);
        return status;
}


int unlock(char *fn_to_unlock, char *uuid, int readonly)
{
        char *lockfn_link = 0;
        int status;

        if (!fn_to_unlock || !uuid)
                return LOCK_EBADPARM;

        lockfn_link = create_lockfn_link(fn_to_unlock, LFFL_FORMAT, uuid, readonly);
        if (unlikely(!lockfn_link)) { status = LOCK_ENOMEM; goto finish; }

        if (unlikely(unlink(lockfn_link) == -1)) {
                /* if no lock file than fold into success case */
                LOG("error removing linked lock file %s", lockfn_link);
        }

        status = LOCK_OK;

finish:
        free(lockfn_link);
        return status;
}

int lock_delta(char *fn)
{
        DIR *pd = 0;
        struct dirent *dptr;
        char *ptr;
        int result = INT_MAX;
        char *buf = 0;
        int fd;
        int clstat;
        struct stat statbuf, statnow;
        char *dirname = malloc(strlen(fn));
        char *uname = malloc(strlen(fn) + 8);
        int pid = (int)getpid();
        int uniq;

        if (!fn || !dirname || !uname)
                return LOCK_EBADPARM;
        
        /* create file to normalize time */
        srandom((int)time(0) ^ pid);
        uniq = random() % 0xffffff; 
        buf = malloc(strlen(fn) + 24);
        if (unlikely(!buf)) { result = LOCK_ENOMEM; goto finish; }

        strcpy(buf, fn);
        sprintf(buf + strlen(buf), ".xen%08d.tmp", uniq);

        fd = open(buf, O_WRONLY | O_CREAT, 0644);
        if (fd == -1) { result = LOCK_EOPEN; goto finish; }
        clstat = close(fd);
        if (unlikely(clstat == -1)) {
                LOG("fail on close\n");
        }
        if (lstat(buf, &statnow) == -1) {
                unlink(buf);
                result = LOCK_ESTAT;
                goto finish;
        }
        unlink(buf);

        /* get directory */
        ptr = strrchr(fn, '/');
        if (!ptr) {
                strcpy(dirname, ".");
                ptr = fn;
        } else {
                int numbytes = ptr - fn;
                strncpy(dirname, fn, numbytes);
                ptr += 1;
        }
        pd = opendir(dirname); 
        if (!pd) goto finish;

        dptr = readdir(pd);
        while (dptr) {
                if (strcmp(dptr->d_name, ptr) &&
                    !strncmp(dptr->d_name, ptr,  strlen(ptr))) {
                        char *fpath = malloc(strlen(dptr->d_name) + 
                                             strlen(dirname) + 2);
                        if (!fpath) {
                            closedir(pd);
                            result = LOCK_ENOMEM;
                            goto finish;
                        }
                        strcpy(fpath, dirname);
                        strcat(fpath, "/");
                        strcat(fpath, dptr->d_name);
                        if (lstat(fpath, &statbuf) != -1) {
                                int diff = (int)statnow.st_mtime - 
                                           (int)statbuf.st_mtime;
                                /* adjust diff if someone updated the lock
                                   between now and when we created the "now"
                                   file 
                                 */
                                diff = (diff < 0) ? 0 : diff;
                                result = diff < result ? diff : result;
                        }
                        free(fpath);
                }
                dptr = readdir(pd);
        }

        closedir(pd);

finish:
        free(dirname);
        free(uname);

        /* returns smallest lock time, or error */
        if (result == INT_MAX) result = LOCK_ENOLOCK;
        return result;
}

#if defined(TEST)
/*
 * the following is for sanity testing.
 */

static void usage(char *prg)
{
        printf("usage %s\n"
               "    [dwtr <filename>]\n"
               "    [p <filename> [num iterations]\n"
               "    [u <filename> 0|1] [<uniqid>]\n"
               "    [l <filename> [0|1] [0|1] ([q <uniqid>] ]\n", prg);
        printf("        p : perf test lock take and reassert\n");
        printf("        d : delta lock time\n");
        printf("        t : test the file (after random locks)\n");
        printf("        r : random lock tests (must ^C)\n");
        printf("        u : unlock, readonly? optional uniqID (default is PID)\n");
        printf("        l : lock, readonly? force?, optional uniqID (default is PID)\n");
}

static void test_file(char *fn)
{
        FILE *fptr;
        int prev_count = 0;
        int count, pid, time;

        fptr = fopen(fn, "r");
        if (!fptr) {
                LOG("ERROR on file %s open, errno=%d\n", fn, errno);
                return;
        } 

        while (!feof(fptr)) {
                fscanf(fptr, "%d %d %d\n", &count, &pid, &time);
                if (prev_count != count) {
                        LOG("ERROR: prev_count=%d, count=%d, pid=%d, time=%d\n",
                                    prev_count, count, pid, time);
                }
                prev_count = count + 1;
        }
}

static void random_locks(char *fn)
{
        int pid = getpid();
        int status;
        char *filebuf = malloc(256);
        int count = 0;
        int dummy;
        int clstat;
        char uuid[12];
        int readonly;

        /* this will never return, kill to exit */

        srandom((int)time(0) ^ pid);

        LOG("pid: %d using file %s\n", pid, fn);
        sprintf(uuid, "%08d", pid);

        while (1) {
                XSLEEP;
                readonly = random()  & 1;
                status = lock(fn, uuid, 0, readonly);
                if (status == LOCK_OK) {
                        /* got lock, open, read, modify write close file */
                        int fd = open(fn, O_RDWR, 0644);
                        if (fd == -1) {
                                LOG("pid: %d ERROR on file %s open, errno=%d\n", 
                                    pid, fn, errno);
                        } else {
                            if (!readonly) {
                                /* ugly code to read data in test format */
                                /* format is "%d %d %d" 'count pid time' */
                                struct stat statbuf;
                                int bytes;
                                status = stat(fn, &statbuf);
                                if (status != -1) {
                                        if (statbuf.st_size > 256) {
                                                lseek(fd, -256, SEEK_END);
                                        } 
                                        memset(filebuf, 0, 256);
                                        bytes = read(fd, filebuf, 256);
                                        if (bytes) {
                                                int bw = bytes-2;
                                                while (bw && filebuf[bw]!='\n') 
                                                        bw--;
                                                if (!bw) bw = -1;
                                                sscanf(&filebuf[bw+1], 
                                                       "%d %d %d", 
                                                       &count, &dummy, &dummy);
                                                count += 1;
                                        }
                                        lseek(fd, 0, SEEK_END);
                                        sprintf(filebuf, "%d %d %d\n", 
                                                count, pid, (int)time(0));
                                        write(fd, filebuf, strlen(filebuf));
                                } else {
                                        LOG("pid: %d ERROR on file %s stat, "
                                            "errno=%d\n", pid, fn, errno);
                                }
                            }
                            clstat = close(fd);
                            if (unlikely(clstat == -1)) {
                                    LOG("fail on close\n");
                            }
                        }
                        XSLEEP;
                        status = unlock(fn, uuid, readonly);
                        LOG("unlock status is %d\n", status);
                }
        }
}

static void perf_lock(char *fn, int loops)
{
    int status;
    char buf[9];
    int start = loops;

    sprintf(buf, "%08d", getpid());

    while (loops--) {
        status = lock(fn, buf, 0, 0);
        if (status < 0) {
            printf("failed to get lock at iteration %d errno=%d\n", start - loops, errno);
            return;
        }
    }
    unlock(fn, buf, 0);
}

int main(int argc, char *argv[])
{
        int status;
        char *ptr;
        char uuid[12];
        int force;
        int readonly;

        if (argc < 3) {
                usage(argv[0]);
                return 0;
        }

        sprintf(uuid, "%08d", getpid());
        ptr = uuid;

        if (!strcmp(argv[1],"d")) {
                status = lock_delta(argv[2]);
                printf("lock delta for %s is %d seconds\n", argv[2], status);
        } else if (!strcmp(argv[1],"t")) {
                test_file(argv[2]);
        } else if (!strcmp(argv[1],"r")) {
                random_locks(argv[2]);
        } else if (!strcmp(argv[1],"p")) {
                perf_lock(argv[2], argc < 3 ? 100000 : atoi(argv[3]));
        } else if (!strcmp(argv[1],"l")) {
                if (argc < 4) force = 0; else force = atoi(argv[3]);
                if (argc < 5) readonly = 0; else readonly = atoi(argv[4]);
                if (argc == 6) ptr = argv[5];
                status = lock(argv[2], ptr, readonly, force);
                printf("lock status = %d\n", status);
        } else if (!strcmp(argv[1],"u") ) {
                if (argc < 5) readonly = 0; else readonly = atoi(argv[3]);
                if (argc == 5) ptr = argv[4];
                status = unlock(argv[2], ptr, readonly);
                printf("unlock status = %d\n", status);
        } else {
                usage(argv[0]);
        }

        return 0;
}
#endif
#endif
