/*
 * QEMU live migration via generic fd
 *
 * Copyright Red Hat, Inc. 2009
 *
 * Authors:
 *  Chris Lalancette <clalance@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu-common.h"
#include "qemu_socket.h"
#include "migration.h"
#include "monitor.h"
#include "qemu-char.h"
#include "buffered_file.h"
#include "block.h"
#include "qemu_socket.h"
#include "cloudlet/qemu-cloudlet.h"

#define DEBUG_MIGRATION_RAW

#ifdef DEBUG_MIGRATION_RAW
#define DPRINTF(fmt, ...) \
    do { printf("migration-raw: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

static int raw_errno(MigrationState *s)
{
    return errno;
}

static int raw_write(MigrationState *s, const void * buf, size_t size)
{
    return write(s->fd, buf, size);
}

static int raw_close(MigrationState *s)
{
    struct stat st;
    int ret;

    DPRINTF("raw_close\n");
    if (s->fd != -1) {
        ret = fstat(s->fd, &st);
        if (ret == 0 && S_ISREG(st.st_mode)) {
            /*
             * If the file handle is a regular file make sure the
             * data is flushed to disk before signaling success.
             */
            ret = fsync(s->fd);
            if (ret != 0) {
                ret = -errno;
                perror("migration-fd: fsync");
                return ret;
            }
        }
        ret = close(s->fd);
        s->fd = -1;
        if (ret != 0) {
            ret = -errno;
            perror("migration-raw: close");
            return ret;
        }
    }
    return 0;
}

int raw_start_outgoing_migration(MigrationState *s, const char *fdname, raw_type type)
{
    DPRINTF("raw_migration: start migration at %s\n", fdname);
    // for already created file
    s->fd = monitor_get_fd(cur_mon, fdname);
    if (s->fd == -1) {
		s->fd = open(fdname, O_CREAT | O_WRONLY | O_TRUNC, 00644);
		if (s->fd == -1) {
			DPRINTF("raw_migration: failed to open file\n");
			goto err_after_get_fd;
		}

	}

    s->get_error = raw_errno;
    s->write = raw_write;
    s->close = raw_close;

    migrate_fd_connect_raw(s, type);
    return 0;

    close(s->fd);
err_after_get_fd:
    return -1;
}

static void raw_accept_incoming_migration(void *opaque)
{
    QEMUFile *f = opaque;

    process_incoming_migration(f);
    qemu_set_fd_handler2(qemu_stdio_fd(f), NULL, NULL, NULL, NULL);
}

int raw_start_incoming_migration(const char *infd, raw_type type)
{
    int fd;
    int val;
    QEMUFile *f;
    
    val = strtol(infd, NULL, 0);
    if ((errno == ERANGE && (val == INT_MAX|| val == INT_MIN)) || (val == 0)) {
	DPRINTF("Attempting to start an incoming migration via raw\n");
	fd = open(infd, O_RDONLY);
    } else {
        fd = val;
    }

    f = qemu_fdopen(fd, "rb");
    if(f == NULL) {
	DPRINTF("Unable to apply qemu wrapper to file descriptor\n");
	return -errno;
    }

    // read ahead external header file, e.g. libvirt header
    // to have mmap file for memory
    long start_offset = lseek(fd, 0, SEEK_CUR);
    qemu_fseek(f, start_offset, SEEK_CUR);

    set_use_raw(f, type);

    qemu_set_fd_handler2(fd, NULL, raw_accept_incoming_migration, NULL, f);

    return 0;
}

/* stolen from migration.c */
enum {
    MIG_STATE_ERROR,
    MIG_STATE_SETUP,
    MIG_STATE_CANCELLED,
    MIG_STATE_ACTIVE,
    MIG_STATE_COMPLETED,
};

uint64_t raw_dump_device_state(bool suspend, bool print)
{
    MigrationState _s, *s = &_s;
    char fname[64];
    uint64_t num_pages = 0;

    strcpy(fname, "/tmp/qemu.XXXXXX");
    s->fd = mkostemp(fname, O_CREAT | O_WRONLY | O_TRUNC);

    if (s->fd == -1) {
	DPRINTF("raw_migration: failed to open file\n");
	goto err_after_get_fd;
    }

    s->get_error = raw_errno;
    s->write = raw_write;
    s->close = raw_close;

    s->state = MIG_STATE_ACTIVE;
    qemu_fopen_ops_buffered_wrapper(s);

    num_pages = qemu_savevm_dump_non_live(s->file, suspend, print);

    qemu_fclose(s->file);
    unlink(fname);

    return num_pages;

// err_after_open:
    close(s->fd);
err_after_get_fd:
    return 0;  // returns 0 pages on error
}
