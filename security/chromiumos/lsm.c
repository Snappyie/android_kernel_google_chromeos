/*
 * Linux Security Module for Chromium OS
 *
 * Copyright 2011 Google Inc. All Rights Reserved
 *
 * Authors:
 *      Stephan Uphoff  <ups@google.com>
 *      Kees Cook       <keescook@chromium.org>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "Chromium OS LSM: " fmt

#include <linux/module.h>
#include <linux/security.h>
#include <linux/sched.h>	/* current and other task related stuff */
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/mount.h>
#include <linux/path.h>

#include "inode_mark.h"
#include "utils.h"

int chromiumos_security_sb_mount(const char *dev_name, struct path *path,
				 const char *type, unsigned long flags,
				 void *data)
{
#ifdef CONFIG_SECURITY_CHROMIUMOS_NO_SYMLINK_MOUNT
	if (current->total_link_count) {
		char *cmdline;

		cmdline = printable_cmdline(current);
		pr_notice("Mount path with symlinks prohibited - "
			"pid=%d cmdline=%s\n",
			task_pid_nr(current), cmdline);
		kfree(cmdline);
		return -ELOOP;
	}
#endif

	return 0;
}

static void report_load(const char *origin, struct path *path, char *operation)
{
	char *alloced = NULL, *cmdline;
	char *pathname; /* Pointer to either static string or "alloced". */

	if (!path)
		pathname = "<unknown>";
	else {
		/* We will allow 11 spaces for ' (deleted)' to be appended */
		alloced = pathname = kmalloc(PATH_MAX+11, GFP_KERNEL);
		if (!pathname)
			pathname = "<no_memory>";
		else {
			pathname = d_path(path, pathname, PATH_MAX+11);
			if (IS_ERR(pathname))
				pathname = "<too_long>";
			else {
				pathname = printable(pathname);
				kfree(alloced);
				alloced = pathname;
			}
		}
	}

	cmdline = printable_cmdline(current);

	pr_notice("%s %s obj=%s pid=%d cmdline=%s\n", origin,
		  operation, pathname, task_pid_nr(current), cmdline);

	kfree(cmdline);
	kfree(alloced);
}

static int module_locking = 1;
static struct super_block *locked_root;
static DEFINE_SPINLOCK(locked_root_spinlock);

#ifdef CONFIG_SYSCTL
static int zero;
static int one = 1;

static struct ctl_path chromiumos_sysctl_path[] = {
	{ .procname = "kernel", },
	{ .procname = "chromiumos", },
	{ }
};

static struct ctl_table chromiumos_sysctl_table[] = {
	{
		.procname       = "module_locking",
		.data           = &module_locking,
		.maxlen         = sizeof(int),
		.mode           = 0644,
		.proc_handler   = proc_dointvec_minmax,
		.extra1         = &zero,
		.extra2         = &one,
	},
	{ }
};

/*
 * This must be called after early kernel init, since then the rootdev
 * is available.
 */
static void check_locking_enforcement(struct super_block *mnt_sb)
{
	bool ro;

	/*
	 * If module locking is not enforced via a read-only block
	 * device, allow sysctl to change modes for testing.
	 */
	if (mnt_sb->s_bdev) {
		ro = bdev_read_only(mnt_sb->s_bdev);
		pr_info("dev(%u,%u): %s\n",
			MAJOR(mnt_sb->s_bdev->bd_dev),
			MINOR(mnt_sb->s_bdev->bd_dev),
			ro ? "read-only" : "writable");
	} else {
		/*
		 * In the weird case where there is no underlying block device
		 * (e.g. tmpfs), assume it is read-only.
		 */
		ro = 1;
		pr_info("dev(?,?): No s_bdev, assuming read-only.\n");
	}

	if (!ro) {
		if (!register_sysctl_paths(chromiumos_sysctl_path,
					   chromiumos_sysctl_table))
			pr_notice("sysctl registration failed!\n");
		else
			pr_info("module locking can be disabled.\n");
	} else
		pr_info("module locking engaged.\n");
}
#else
static void check_locking_enforcement(void) { }
#endif

void chromiumos_security_sb_free(struct super_block *sb)
{
	/*
	 * When unmounting the filesystem we were using for module
	 * pinning, we must release our reservation, but make sure
	 * no other modules can be loaded.
	 */
	if (!IS_ERR_OR_NULL(locked_root) && sb == locked_root) {
		locked_root = ERR_PTR(-EIO);
		pr_info("umount pinned fs: refusing further module loads\n");
	}
}

static int check_pinning(const char *origin, struct file *file)
{
	struct vfsmount *module_root;

	if (!file) {
		if (!module_locking) {
			report_load(origin, NULL, "old-api-locking-ignored");
			return 0;
		}

		report_load(origin, NULL, "old-api-denied");
		return -EPERM;
	}

	module_root = file->f_path.mnt;

	/* First loaded module defines the root for all others. */
	spin_lock(&locked_root_spinlock);
	/*
	 * locked_root is only NULL at startup. Otherwise, it is either
	 * a valid reference, or an ERR_PTR.
	 */
	if (!locked_root) {
		locked_root = module_root->mnt_sb;
		/*
		 * Unlock now since it's only locked_root we care about.
		 * In the worst case, we will (correctly) report locking
		 * failures before we have announced that locking is
		 * enabled. This would be purely cosmetic.
		 */
		spin_unlock(&locked_root_spinlock);
		check_locking_enforcement(locked_root);
		report_load(origin, &file->f_path, "locked");
	} else {
		spin_unlock(&locked_root_spinlock);
	}

	if (IS_ERR_OR_NULL(locked_root) || module_root->mnt_sb != locked_root) {
		if (unlikely(!module_locking)) {
			report_load(origin, &file->f_path,
				    "locking-ignored");
			return 0;
		}

		report_load(origin, &file->f_path, "denied");
		return -EPERM;
	}

	return 0;
}

int chromiumos_security_load_module(struct file *file)
{
	return check_pinning("init_module", file);
}

int chromiumos_security_load_firmware(struct file *file, char *buf, size_t size)
{
	return check_pinning("request_firmware", file);
}

int chromiumos_security_inode_follow_link(struct dentry *dentry,
					  struct nameidata *nd)
{
	static char accessed_path[PATH_MAX];
	enum chromiumos_symlink_traversal_policy policy;

	policy = chromiumos_get_symlink_traversal_policy(dentry);

	/*
	 * Emit a warning in cases of blocked symlink traversal attempts. These
	 * will show up in kernel warning reports collected by the crash
	 * reporter, so we have some insight on spurious failures that need
	 * addressing.
	 */
	WARN(policy == CHROMIUMOS_SYMLINK_TRAVERSAL_BLOCK,
	     "Blocked symlink traversal for path %x:%x:%s\n",
	     MAJOR(dentry->d_sb->s_dev), MINOR(dentry->d_sb->s_dev),
	     dentry_path(dentry, accessed_path, PATH_MAX));

	return policy == CHROMIUMOS_SYMLINK_TRAVERSAL_BLOCK ? -EACCES : 0;
}

static int __init chromiumos_security_init(void)
{
	pr_info("enabled");

	return 0;
}
security_initcall(chromiumos_security_init);

/* Should not be mutable after boot, so not listed in sysfs (perm == 0). */
module_param(module_locking, int, 0);
MODULE_PARM_DESC(module_locking, "Module loading restrictions (default: true)");
