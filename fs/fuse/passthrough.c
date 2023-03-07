// SPDX-License-Identifier: GPL-2.0

#include "fuse_i.h"

#include <linux/aio.h>
#include <linux/fs_stack.h>

extern struct file *fget_raw(unsigned int fd);
extern void fput(struct file *file);

void fuse_setup_passthrough(struct fuse_conn *fc, struct fuse_req *req)
{
	struct super_block *passthrough_sb;
	struct inode *passthrough_inode;
	struct fuse_open_out *open_out;
	struct file *passthrough_filp;
	unsigned short open_out_index;
	int fs_stack_depth;

	req->passthrough_filp = NULL;

	if (!fc->passthrough)
		return;

	if (!(req->in.h.opcode == FUSE_OPEN && req->out.numargs == 1) &&
	    !(req->in.h.opcode == FUSE_CREATE && req->out.numargs == 2))
		return;

	open_out_index = req->out.numargs - 1;

	if (req->out.args[open_out_index].size != sizeof(*open_out))
		return;

	open_out = req->out.args[open_out_index].value;

	if (!(open_out->open_flags & FOPEN_PASSTHROUGH))
		return;

	if (open_out->fd < 0)
		return;

	passthrough_filp = fget_raw(open_out->fd);
	if (!passthrough_filp)
		return;

	passthrough_inode = file_inode(passthrough_filp);
	passthrough_sb = passthrough_inode->i_sb;
	fs_stack_depth = passthrough_sb->s_stack_depth + 1;
	if (fs_stack_depth > FILESYSTEM_MAX_STACK_DEPTH) {
		fput(passthrough_filp);
		return;
	}

	req->passthrough_filp = passthrough_filp;
}

static inline ssize_t fuse_passthrough_read_write_iter(struct kiocb *iocb,
						       struct iov_iter *iter,
						       bool write)
{
	struct file *fuse_filp = iocb->ki_filp;
	struct fuse_file *ff = fuse_filp->private_data;
	struct file *passthrough_filp = ff->passthrough_filp;
	struct inode *passthrough_inode;
	struct inode *fuse_inode;
    struct fuse_conn *fc;
	ssize_t ret = -EIO;

	fuse_inode = fuse_filp->f_path.dentry->d_inode;
	passthrough_inode = file_inode(passthrough_filp);
    fc = ff->fc;

	iocb->ki_filp = passthrough_filp;

	if (write) {
		if (!passthrough_filp->f_op->write_iter)
			goto out;

		ret = call_write_iter(passthrough_filp, iocb, iter);
		if (ret >= 0 || ret == -EIOCBQUEUED) {
			struct fuse_inode *fi = get_fuse_inode(fuse_inode);
			fsstack_copy_inode_size(fuse_inode, passthrough_inode);
			fi->attr_version = ++fc->attr_version;
			fsstack_copy_attr_times(fuse_inode, passthrough_inode);
		}
	} else {
		if (!passthrough_filp->f_op->read_iter)
			goto out;

		ret = call_read_iter(passthrough_filp, iocb, iter);
		if (ret >= 0 || ret == -EIOCBQUEUED)
			fsstack_copy_attr_atime(fuse_inode, passthrough_inode);
	}

out:
	iocb->ki_filp = fuse_filp;

	return ret;
}

ssize_t fuse_passthrough_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	return fuse_passthrough_read_write_iter(iocb, to, false);
}

ssize_t fuse_passthrough_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	return fuse_passthrough_read_write_iter(iocb, from, true);
}

void fuse_passthrough_release(struct fuse_file *ff)
{
	if (ff->passthrough_filp) {
		fput(ff->passthrough_filp);
		ff->passthrough_filp = NULL;
	}
}