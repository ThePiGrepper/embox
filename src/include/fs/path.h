/**
 * @file
 *
 * @date 27.03.2014
 * @author: Vita Loginova
 */

#ifndef FS_PATH_H_
#define FS_PATH_H_

struct inode;
struct mount_descriptor;

struct path {
	struct inode *node;
	struct mount_descriptor *mnt_desc;
};

#endif /* FS_PATH_H_ */
