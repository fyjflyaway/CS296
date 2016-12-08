#include <linux/module.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/magic.h>
#include <linux/of.h>


static struct kmem_cache *op_inode_cachep;

static DEFINE_MUTEX(op_mutex);

#define AUFS_ROOT_INO

enum op_inode_type {
	op_inode_node,
	op_inode_prop,
};

union op_inode_data {
	struct device_node *node;
	struct property *prop;
};

struct op_inode_info {
	struct inode vfs_inode;
	enum op_inode_type type;
	union op_inode_data u;
};

static int aufs_readdir(struct file *file, struct dir_context *ctx);

static inline struct op_inode_info *OP_I(struct inode* inode) {
	return container_of(inode, struct op_inode_info, vfs_inode);
}

static struct inode *aufs_iget(struct super_block *sb, ino_t ino) {
	struct inode *inode;
	inode = iget_locked(sb, ino);
	if (!inode) {
		return ERR_PTR(-ENOMEM);
	}
	return inode;
}

static const struct file_operations aufs_operations = {
	.read = generic_read_dir,
	.iterate = aufs_readdir,
	.llseek = generic_file_llseek,
};

static struct dentry *aufs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);

static const struct inode_operations aufs_inode_operation = {
	.lookup = aufs_lookup,
};

static const struct file_operations aufs_prop_ops = {
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release,
};

static struct dentry *aufs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct op_inode_info *ent_oi, *oi = OP_I(dir);
	struct device_node *dp, *child;
	struct property *prop;
	enum op_inode_type ent_type;
	union op_inode_data ent_data;
	const char *name;
	struct inode *inode;
	unsigned int ino;
	int len;

	BUG_ON(oi->type != op_inode_node);

	dp = oi->u.node;

	name = dentry->d_name.name;
	len = dentry->d_name.len;
	if (!strcmp(name, ".")) {
		return dentry;
	}

	mutex_lock(&op_mutex);

    child = dp->child;
	while (child) {
		if (!strcmp(child->properties->name, name)) {
			ent_type = op_inode_node;
			ent_data.node = child;
			ino = child->properties->unique_id;
			goto found;
		}
		child = child->sibling;
	}

	prop = dp->properties;
	while (prop) {
		if (!strcmp(prop->name, name)) {
			ent_type = op_inode_prop;
			ent_data.prop = prop;
			ino = prop->unique_id;
			goto found;
		}
		prop = prop->next;
	}

	mutex_unlock(&op_mutex);
	return ERR_PTR(-ENOENT);

found:
    inode = aufs_iget(dir->i_sb, ino);
    mutex_unlock(&op_mutex);
    if (IS_ERR(inode))
        return ERR_CAST(inode);
    ent_oi = OP_I(inode);
    ent_oi->type = ent_type;
    ent_oi->u = ent_data;
    switch (ent_type) {
    	case op_inode_node:
    	   inode->i_mode = S_IFDIR | S_IRUGO | S_IXUGO;
    	   inode->i_op = &aufs_inode_operation;
    	   inode->i_fop = &aufs_operations;
    	   set_nlink(inode, 2);
    	   break;
    	case op_inode_prop:
    	    inode->i_mode = S_IFREG | S_IRUGO;
    	    inode->i_fop = &aufs_prop_ops;
    	    set_nlink(inode, 1);
    	    inode->i_size = ent_oi->u.prop->length;
    	    break;
    } 

    d_add(dentry, inode);
	return NULL;
}

static int aufs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct op_inode_info *oi = OP_I(inode);
	struct device_node *dp = oi->u.node;
	struct device_node *child;
	struct property *prop;

	int i;

	mutex_lock(&op_mutex);

	if (ctx->pos == 0) {
		if (!dir_emit(ctx, ".", 1, inode->i_ino, DT_DIR))
			goto out;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		if (!dir_emit(ctx, "..", 2, ((dp->parent == NULL) ? AUFS_ROOT_INO : dp->parent->properties->unique_id), DT_DIR))
			goto out;
		ctx->pos = 2;
	}

	i = ctx->pos - 2;

	child = dp->child;
	while (i && child) {
		child = child->sibling;
		i --;
	}
	while (child) {
		if (!dir_emit(ctx, child->properties->name, strlen(child->properties->name), child->properties->unique_id, DT_DIR))
			goto out;
		ctx->pos ++;
		child = child->sibling;
	}

	prop = dp->properties;
	while (i && prop) {
		prop = prop->next;
		i --;
	}
	while (prop) {
		if (!dir_emit(ctx, prop->name, strlen(prop->name), prop->unique_id, DT_REG))
			goto out;
		ctx->pos ++;
		prop = prop->next;		
	}
out:
    mutex_unlock(&op_mutex);
    return 0;
}

static void aufs_put_super(struct super_block *sb)
{
	pr_debug("aufs super block destroyed\n");
}

static struct inode* aufs_alloc_inode(struct super_block *sb)
{
	struct op_inode_info *oi;

	oi = kmem_cache_alloc(op_inode_cachep, GFP_KERNEL);
	if (!oi)
		return NULL;

	return &oi->vfs_inode;
}

static void aufs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(op_inode_cachep, OP_I(inode));
}

static void aufs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, aufs_i_callback);
}

static int aufs_remount(struct super_block* sb, int *flags, char *data)
{
	sync_filesystem(sb);
	*flags |= MS_NOATIME;
	return 0;
}

static struct super_operations const aufs_super_ops = {
	.put_super = aufs_put_super,
	.alloc_inode = aufs_alloc_inode,
	.destroy_inode = aufs_destroy_inode,
	.statfs = simple_statfs,
	.remount_fs = aufs_remount,
};



static int aufs_fill_sb(struct super_block *sb, void *data, int silent)
{
	struct inode *root = NULL;
	struct op_inode_info *oi;

	
	sb->s_op = &aufs_super_ops;
	sb->s_flags |= MS_NOATIME;
	sb->s_blocksize = 1024;
	sb->s_blocksize_bits = 10;
	sb->s_time_gran = 1;

	root = new_inode(sb);
	if (!root)
	{
		pr_err("inode alloction failed\n");
		return -ENOMEM;
	}

	root->i_ino = 0;
	root->i_sb = sb;
	root->i_op = &aufs_inode_operation;
	root->i_atime = root->i_mtime = root->i_ctime = CURRENT_TIME;
	inode_init_owner(root, NULL, S_IFDIR);


	oi = OP_I(root);
	oi->type = op_inode_node;
	oi->u.node = of_find_node_by_path(".");
	
	sb->s_root = d_make_root(root);

	if (!sb->s_root)
	{
		pr_err("root creation failed\n");
		return -ENOMEM;
	}

	return 0;
}

static struct dentry *aufs_mount(struct file_system_type *type, int flags, char const *dev, void *data)
{
	struct dentry *const entry = mount_bdev(type, flags, dev, data, aufs_fill_sb);
	if (IS_ERR(entry))
		pr_err("aufs mounting failed\n");
	else
		pr_debug("aufs mounted\n");
	return entry;
}

static struct file_system_type aufs_type = {
	.owner = THIS_MODULE,
	.name = "aufs",
	.mount = aufs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};


static void op_inode_init_once(void *data)
{
	struct op_inode_info *oi = (struct op_inode_info *) data;

	inode_init_once(&oi->vfs_inode);
}

static int __init aufs_init(void)
{
	int err;
	pr_debug("aufs module loaded\n");
	op_inode_cachep = kmem_cache_create("op_inode_cache", sizeof(struct op_inode_info), 0, (SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD ), op_inode_init_once);
	if (!op_inode_cachep)
		return -ENOMEM;
	err = register_filesystem(&aufs_type);
	if (err)
		kmem_cache_destroy(op_inode_cachep);
	return err;
}

static void __exit aufs_fini(void)
{
	pr_debug("aufs module unloaded\n");
	unregister_filesystem(&aufs_type);

	kmem_cache_destroy(op_inode_cachep);
}

module_init(aufs_init);
module_exit(aufs_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("kmu");