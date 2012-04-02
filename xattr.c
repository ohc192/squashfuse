/*
 * Copyright (c) 2012 Dave Vasilevsky <dave@vasilevsky.ca>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR(S) ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#include "xattr.h"

#include "squashfuse.h"
#include "nonstd.h"

#include <string.h>
#include <stdlib.h>


#define SQFS_XATTR_PREFIX_MAX SQUASHFS_XATTR_SECURITY

typedef struct {
	const char *pref;
	size_t len;
} sqfs_prefix;
 
sqfs_prefix sqfs_xattr_prefixes[] = {
	{"user.", 5},
	{"security.", 9},
	{"trusted.", 8},
};


typedef enum {
	CURS_VSIZE = 1,
	CURS_VAL = 2,
	CURS_NEXT = 4,
} sqfs_xattr_curs;

sqfs_err sqfs_xattr_init(sqfs *fs) {
	off_t start = fs->sb.xattr_id_table_start;
	if (start == SQUASHFS_INVALID_BLK)
		return SQFS_OK;
	
	ssize_t read = sqfs_pread(fs->fd, &fs->xattr_info, sizeof(fs->xattr_info),
		start);
	if (read != sizeof(fs->xattr_info))
		return SQFS_ERR;
	sqfs_swapin_xattr_id_table(&fs->xattr_info);
	
	return sqfs_table_init(&fs->xattr_table, fs->fd,
		start + sizeof(fs->xattr_info), sizeof(struct squashfs_xattr_id),
		fs->xattr_info.xattr_ids);
}

sqfs_err sqfs_xattr_open(sqfs *fs, sqfs_inode *inode, sqfs_xattr *x) {
	x->remain = 0; // assume none exist	
	if (fs->xattr_info.xattr_ids == 0 || inode->xattr == SQUASHFS_INVALID_XATTR)
		return SQFS_OK;
	
	sqfs_err err = sqfs_table_get(&fs->xattr_table, fs, inode->xattr,
		&x->info);
	if (err)
		return SQFS_ERR;
	sqfs_swapin_xattr_id(&x->info);
	
	sqfs_md_cursor_inode(&x->c_next, x->info.xattr,
		fs->xattr_info.xattr_table_start);
	
	x->fs = fs;
	x->remain = x->info.count;
	x->cursors = CURS_NEXT;
	return SQFS_OK;
}

sqfs_err sqfs_xattr_read(sqfs_xattr *x) {
	if (x->remain == 0)
		return SQFS_ERR;
	
	sqfs_err err;
	if (!(x->cursors & CURS_NEXT)) {
		x->ool = false; // force inline
		if ((err = sqfs_xattr_value(x, NULL)))
			return err;
	}
	
	x->c_name = x->c_next;
	if ((err = sqfs_md_read(x->fs, &x->c_name, &x->entry, sizeof(x->entry))))
		return err;
	sqfs_swapin_xattr_entry(&x->entry);
	
	x->type = x->entry.type & SQUASHFS_XATTR_PREFIX_MASK;
	x->ool = x->entry.type & SQUASHFS_XATTR_VALUE_OOL;
	if (x->type > SQFS_XATTR_PREFIX_MAX)
		return SQFS_ERR;
	
	--(x->remain);
	x->cursors = 0;
	return err;
}

size_t sqfs_xattr_name_size(sqfs_xattr *x) {
	return x->entry.size + sqfs_xattr_prefixes[x->type].len;
}

sqfs_err sqfs_xattr_name(sqfs_xattr *x, char *name, bool prefix) {
	if (name && prefix) {
		sqfs_prefix *p = &sqfs_xattr_prefixes[x->type];
		memcpy(name, p->pref, p->len);
		name += p->len;
	}
	
	x->c_vsize = x->c_name;
	sqfs_err err = sqfs_md_read(x->fs, &x->c_vsize, name, x->entry.size);
	if (err)
		return err;
	
	x->cursors |= CURS_VSIZE;
	return err;
}

sqfs_err sqfs_xattr_value_size(sqfs_xattr *x, size_t *size) {
	sqfs_err err;
	if (!(x->cursors & CURS_VSIZE))
		if ((err = sqfs_xattr_name(x, NULL, false)))
			return err;
	
	x->c_val = x->c_vsize;
	if ((err = sqfs_md_read(x->fs, &x->c_val, &x->val, sizeof(x->val))))
		return err;
	sqfs_swapin_xattr_val(&x->val);
	
	if (x->ool) {
		uint64_t pos;
		x->c_next = x->c_val;
		if ((err = sqfs_md_read(x->fs, &x->c_next, &pos, sizeof(pos))))
			return err;
		sqfs_swapin64(&pos);
		x->cursors |= CURS_NEXT;
		
		sqfs_md_cursor_inode(&x->c_val, pos,
			x->fs->xattr_info.xattr_table_start);
		if ((err = sqfs_md_read(x->fs, &x->c_val, &x->val, sizeof(x->val))))
			return err;
		sqfs_swapin_xattr_val(&x->val);
	}
	
	if (size)
		*size = x->val.vsize;	
	x->cursors |= CURS_VAL;
	return err;
}

sqfs_err sqfs_xattr_value(sqfs_xattr *x, void *buf) {
	sqfs_err err;
	if (!(x->cursors & CURS_VAL))
		if ((err = sqfs_xattr_value_size(x, NULL)))
			return err;
	
	sqfs_md_cursor c = x->c_val;
	if ((err = sqfs_md_read(x->fs, &c, buf, x->val.vsize)))
		return err;
	
	if (!x->ool) {
		x->c_next = c;
		x->cursors |= CURS_NEXT;
	}
	return err;
}

static sqfs_err sqfs_xattr_find_prefix(const char *name, uint16_t *type) {
	for (int i = 0; i <= SQFS_XATTR_PREFIX_MAX; ++i) {
		sqfs_prefix *p = &sqfs_xattr_prefixes[i];
		if (strncmp(name, p->pref, p->len) == 0) {
			*type = i;
			return SQFS_OK;
		}
	}
	return SQFS_ERR;
}

// FIXME: Indicate EINVAL, ENOMEM?
sqfs_err sqfs_xattr_find(sqfs_xattr *x, const char *name, bool *found) {
	sqfs_err err;
	
	uint16_t type;
	if ((err = sqfs_xattr_find_prefix(name, &type)))
		return err;
	name += sqfs_xattr_prefixes[type].len;
	size_t len = strlen(name);
	char *cmp = malloc(len);
	if (!cmp)
		return SQFS_ERR;
	
	while (x->remain) {
		if ((err = sqfs_xattr_read(x)))
			goto done;
		if (x->type != type && x->entry.size != len)
			continue;
		if ((err = sqfs_xattr_name(x, cmp, false)))
			goto done;
		if (strncmp(name, cmp, len) == 0) {
			*found = true;
			goto done;
		}
	}
	
	*found = false;
	return SQFS_OK;

done:
	free(cmp);
	return err;
}