/* 
 * ifuse.c
 * A Fuse filesystem which exposes the iPhone's filesystem.
 *
 * Copyright (c) 2008 Matt Colyer All Rights Reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA 
 */

#define FUSE_USE_VERSION  26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <unistd.h>
#include <stdint.h>

typedef uint32_t uint32;		// this annoys me too

#include <libiphone/libiphone.h>

GHashTable *file_handles;
int fh_index = 0;

iphone_device_t phone = NULL;
iphone_lckd_client_t control = NULL;

int debug = 0;

static int ifuse_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	iphone_afc_client_t afc = fuse_get_context()->private_data;
	iphone_error_t ret = iphone_afc_get_file_attr(afc, path, stbuf);

	if (ret != IPHONE_E_SUCCESS)
		res = -ENOENT;

	return res;
}

static int ifuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	int i;
	char **dirs = NULL;
	iphone_afc_client_t afc = fuse_get_context()->private_data;

	iphone_afc_get_dir_list(afc, path, &dirs);

	if (!dirs)
		return -ENOENT;

	for (i = 0; dirs[i]; i++) {
		filler(buf, dirs[i], NULL, 0);
	}

	free_dictionary(dirs);

	return 0;
}

static int ifuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	// exactly the same as open but using a different mode
	iphone_afc_file_t file = NULL;
	uint32 *argh_filehandle = (uint32 *) malloc(sizeof(uint32));
	iphone_afc_client_t afc = fuse_get_context()->private_data;

	iphone_afc_open_file(afc, path, IPHONE_AFC_FILE_WRITE | IPHONE_AFC_FILE_CREAT, &file);
	*argh_filehandle = iphone_afc_get_file_handle(file);
	fi->fh = *argh_filehandle;
	g_hash_table_insert(file_handles, argh_filehandle, file);
	return 0;
}

static int ifuse_open(const char *path, struct fuse_file_info *fi)
{
	iphone_afc_file_t file = NULL;
	uint32 *argh_filehandle = (uint32 *) malloc(sizeof(uint32));
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	uint32_t mode = 0;

	if ((fi->flags & 3) == O_RDWR) {
		mode = IPHONE_AFC_FILE_RW;
	} else if ((fi->flags & 3) == O_WRONLY) {
		mode = IPHONE_AFC_FILE_WRITE;
	} else if ((fi->flags & 3) == O_RDONLY) {
		mode = IPHONE_AFC_FILE_READ;
	} else {
		mode = IPHONE_AFC_FILE_READ;
	}

	iphone_afc_open_file(afc, path, mode, &file);

	*argh_filehandle = iphone_afc_get_file_handle(file);
	fi->fh = *argh_filehandle;
	g_hash_table_insert(file_handles, argh_filehandle, file);

	return 0;
}

static int ifuse_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int bytes = 0;
	iphone_afc_file_t file;
	iphone_afc_client_t afc = fuse_get_context()->private_data;

	if (size == 0)
		return 0;

	file = g_hash_table_lookup(file_handles, &(fi->fh));
	if (!file) {
		return -ENOENT;
	}

	if (IPHONE_E_SUCCESS == iphone_afc_seek_file(afc, file, offset))
		iphone_afc_read_file(afc, file, buf, size, &bytes);
	return bytes;
}

static int ifuse_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	int bytes = 0;
	iphone_afc_file_t file = NULL;
	iphone_afc_client_t afc = fuse_get_context()->private_data;

	if (size == 0)
		return 0;

	file = g_hash_table_lookup(file_handles, &(fi->fh));
	if (!file)
		return -ENOENT;

	if (IPHONE_E_SUCCESS == iphone_afc_seek_file(afc, file, offset))
		iphone_afc_write_file(afc, file, buf, size, &bytes);
	return bytes;
}

static int ifuse_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	return 0;
}

static int ifuse_release(const char *path, struct fuse_file_info *fi)
{
	iphone_afc_file_t file = NULL;
	iphone_afc_client_t afc = fuse_get_context()->private_data;

	file = g_hash_table_lookup(file_handles, &(fi->fh));
	if (!file) {
		return -ENOENT;
	}
	iphone_afc_close_file(afc, file);

	g_hash_table_remove(file_handles, &(fi->fh));

	return 0;
}

void *ifuse_init_with_service(struct fuse_conn_info *conn, const char *service_name)
{
	int port = 0;
	iphone_afc_client_t afc = NULL;

	conn->async_read = 0;

	file_handles = g_hash_table_new(g_int_hash, g_int_equal);

	if (IPHONE_E_SUCCESS == iphone_lckd_start_service(control, service_name, &port) && !port) {
		iphone_lckd_free_client(control);
		iphone_free_device(phone);
		fprintf(stderr, "Something went wrong when starting AFC.");
		return NULL;
	}

	iphone_afc_new_client(phone, 3432, port, &afc);

	return afc;
}

void ifuse_cleanup(void *data)
{
	iphone_afc_client_t afc = (iphone_afc_client_t) data;

	iphone_afc_free_client(afc);
	iphone_lckd_free_client(control);
	iphone_free_device(phone);
}

int ifuse_flush(const char *path, struct fuse_file_info *fi)
{
	return 0;
}

int ifuse_statfs(const char *path, struct statvfs *stats)
{
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	char **info_raw = NULL;
	uint32_t totalspace = 0, freespace = 0, blocksize = 0, i = 0;

	iphone_afc_get_devinfo(afc, &info_raw);
	if (!info_raw)
		return -ENOENT;

	for (i = 0; info_raw[i]; i++) {
		if (!strcmp(info_raw[i], "FSTotalBytes")) {
			totalspace = atoi(info_raw[i + 1]);
		} else if (!strcmp(info_raw[i], "FSFreeBytes")) {
			freespace = atoi(info_raw[i + 1]);
		} else if (!strcmp(info_raw[i], "FSBlockSize")) {
			blocksize = atoi(info_raw[i + 1]);
		}
	}
	free_dictionary(info_raw);

	// Now to fill the struct.
	stats->f_bsize = stats->f_frsize = blocksize;
	stats->f_blocks = totalspace / blocksize;	// gets the blocks by dividing bytes by blocksize
	stats->f_bfree = stats->f_bavail = freespace / blocksize;	// all bytes are free to everyone, I guess.
	stats->f_namemax = 255;		// blah
	stats->f_files = stats->f_ffree = 1000000000;	// make up any old thing, I guess
	return 0;
}

int ifuse_truncate(const char *path, off_t size)
{
	int result = 0;
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	iphone_afc_file_t tfile = NULL;
	iphone_afc_open_file(afc, path, IPHONE_AFC_FILE_READ, &tfile);
	if (!tfile)
		return -1;

	result = iphone_afc_truncate_file(afc, tfile, size);
	iphone_afc_close_file(afc, tfile);
	return result;
}

int ifuse_ftruncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	iphone_afc_file_t file = g_hash_table_lookup(file_handles, &fi->fh);
	if (!file)
		return -ENOENT;

	return iphone_afc_truncate_file(afc, file, size);
}

int ifuse_unlink(const char *path)
{
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	if (IPHONE_E_SUCCESS == iphone_afc_delete_file(afc, path))
		return 0;
	else
		return -1;
}

int ifuse_rename(const char *from, const char *to)
{
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	if (IPHONE_E_SUCCESS == iphone_afc_rename_file(afc, from, to))
		return 0;
	else
		return -1;
}

int ifuse_mkdir(const char *dir, mode_t ignored)
{
	iphone_afc_client_t afc = fuse_get_context()->private_data;
	if (IPHONE_E_SUCCESS == iphone_afc_mkdir(afc, dir))
		return 0;
	else
		return -1;
}

void *ifuse_init_normal(struct fuse_conn_info *conn)
{
	return ifuse_init_with_service(conn, "com.apple.afc");
}

void *ifuse_init_jailbroken(struct fuse_conn_info *conn)
{
	return ifuse_init_with_service(conn, "com.apple.afc2");
}


static struct fuse_operations ifuse_oper = {
	.getattr = ifuse_getattr,
	.statfs = ifuse_statfs,
	.readdir = ifuse_readdir,
	.mkdir = ifuse_mkdir,
	.rmdir = ifuse_unlink,		// AFC uses the same op for both.
	.create = ifuse_create,
	.open = ifuse_open,
	.read = ifuse_read,
	.write = ifuse_write,
	.truncate = ifuse_truncate,
	.ftruncate = ifuse_ftruncate,
	.unlink = ifuse_unlink,
	.rename = ifuse_rename,
	.fsync = ifuse_fsync,
	.release = ifuse_release,
	.init = ifuse_init_normal,
	.destroy = ifuse_cleanup
};

static int ifuse_opt_proc(void *data, const char *arg, int key, struct fuse_args *outargs)
{
	char *tmp;
	static int option_num = 0;
	(void) data;

	switch (key) {
	case FUSE_OPT_KEY_OPT:
		if (strcmp(arg, "allow_other") == 0 || strcmp(arg, "-d") == 0 || strcmp(arg, "-s") == 0)
			return 1;
		else if (strcmp(arg, "--afc2") == 0) {
			ifuse_oper.init = ifuse_init_jailbroken;
			return 0;
		} else
			return 0;
		break;
	case FUSE_OPT_KEY_NONOPT:
		option_num++;

		// Throw the first nameless option away (the mountpoint)
		if (option_num == 1)
			return 0;
		else
			return 1;
	}
	return 1;
}

int main(int argc, char *argv[])
{
	char **ammended_argv;
	int i, j;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	if (fuse_opt_parse(&args, NULL, NULL, ifuse_opt_proc) == -1) {
		return -1;
	}

	if (argc < 2) {
		fprintf(stderr, "A path to the USB device must be specified\n");
		return -1;
	}

	iphone_get_device(&phone);
	if (!phone) {
		fprintf(stderr, "No iPhone found, is it connected?\n");
		fprintf(stderr, "If it is make sure that your user has permissions to access the raw usb device.\n");
		fprintf(stderr, "If you're still having issues try unplugging the device and reconnecting it.\n");
		return 0;
	}

	if (IPHONE_E_SUCCESS != iphone_lckd_new_client(phone, &control)) {
		iphone_free_device(phone);
		fprintf(stderr, "Something went in lockdown handshake.\n");
		fprintf(stderr, "Did you run libiphone-initconf as the current user?\n");
		return 0;
	}

	return fuse_main(args.argc, args.argv, &ifuse_oper, NULL);
}
