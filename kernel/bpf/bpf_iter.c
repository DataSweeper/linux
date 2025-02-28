// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2020 Facebook */

#include <linux/fs.h>
#include <linux/anon_inodes.h>
#include <linux/filter.h>
#include <linux/bpf.h>

struct bpf_iter_target_info {
	struct list_head list;
	const struct bpf_iter_reg *reg_info;
	u32 btf_id;	/* cached value */
};

struct bpf_iter_link {
	struct bpf_link link;
	struct bpf_iter_aux_info aux;
	struct bpf_iter_target_info *tinfo;
};

struct bpf_iter_priv_data {
	struct bpf_iter_target_info *tinfo;
	const struct bpf_iter_seq_info *seq_info;
	struct bpf_prog *prog;
	u64 session_id;
	u64 seq_num;
	bool done_stop;
	u8 target_private[] __aligned(8);
};

static struct list_head targets = LIST_HEAD_INIT(targets);
static DEFINE_MUTEX(targets_mutex);

/* protect bpf_iter_link changes */
static DEFINE_MUTEX(link_mutex);

/* incremented on every opened seq_file */
static atomic64_t session_id;

static int prepare_seq_file(struct file *file, struct bpf_iter_link *link,
			    const struct bpf_iter_seq_info *seq_info);

static void bpf_iter_inc_seq_num(struct seq_file *seq)
{
	struct bpf_iter_priv_data *iter_priv;

	iter_priv = container_of(seq->private, struct bpf_iter_priv_data,
				 target_private);
	iter_priv->seq_num++;
}

static void bpf_iter_dec_seq_num(struct seq_file *seq)
{
	struct bpf_iter_priv_data *iter_priv;

	iter_priv = container_of(seq->private, struct bpf_iter_priv_data,
				 target_private);
	iter_priv->seq_num--;
}

static void bpf_iter_done_stop(struct seq_file *seq)
{
	struct bpf_iter_priv_data *iter_priv;

	iter_priv = container_of(seq->private, struct bpf_iter_priv_data,
				 target_private);
	iter_priv->done_stop = true;
}

/* bpf_seq_read, a customized and simpler version for bpf iterator.
 * no_llseek is assumed for this file.
 * The following are differences from seq_read():
 *  . fixed buffer size (PAGE_SIZE)
 *  . assuming no_llseek
 *  . stop() may call bpf program, handling potential overflow there
 */
static ssize_t bpf_seq_read(struct file *file, char __user *buf, size_t size,
			    loff_t *ppos)
{
	struct seq_file *seq = file->private_data;
	size_t n, offs, copied = 0;
	int err = 0;
	void *p;

	mutex_lock(&seq->lock);

	if (!seq->buf) {
		seq->size = PAGE_SIZE;
		seq->buf = kmalloc(seq->size, GFP_KERNEL);
		if (!seq->buf) {
			err = -ENOMEM;
			goto done;
		}
	}

	if (seq->count) {
		n = min(seq->count, size);
		err = copy_to_user(buf, seq->buf + seq->from, n);
		if (err) {
			err = -EFAULT;
			goto done;
		}
		seq->count -= n;
		seq->from += n;
		copied = n;
		goto done;
	}

	seq->from = 0;
	p = seq->op->start(seq, &seq->index);
	if (!p)
		goto stop;
	if (IS_ERR(p)) {
		err = PTR_ERR(p);
		seq->op->stop(seq, p);
		seq->count = 0;
		goto done;
	}

	err = seq->op->show(seq, p);
	if (err > 0) {
		/* object is skipped, decrease seq_num, so next
		 * valid object can reuse the same seq_num.
		 */
		bpf_iter_dec_seq_num(seq);
		seq->count = 0;
	} else if (err < 0 || seq_has_overflowed(seq)) {
		if (!err)
			err = -E2BIG;
		seq->op->stop(seq, p);
		seq->count = 0;
		goto done;
	}

	while (1) {
		loff_t pos = seq->index;

		offs = seq->count;
		p = seq->op->next(seq, p, &seq->index);
		if (pos == seq->index) {
			pr_info_ratelimited("buggy seq_file .next function %ps "
				"did not updated position index\n",
				seq->op->next);
			seq->index++;
		}

		if (IS_ERR_OR_NULL(p))
			break;

		/* got a valid next object, increase seq_num */
		bpf_iter_inc_seq_num(seq);

		if (seq->count >= size)
			break;

		err = seq->op->show(seq, p);
		if (err > 0) {
			bpf_iter_dec_seq_num(seq);
			seq->count = offs;
		} else if (err < 0 || seq_has_overflowed(seq)) {
			seq->count = offs;
			if (offs == 0) {
				if (!err)
					err = -E2BIG;
				seq->op->stop(seq, p);
				goto done;
			}
			break;
		}
	}
stop:
	offs = seq->count;
	/* bpf program called if !p */
	seq->op->stop(seq, p);
	if (!p) {
		if (!seq_has_overflowed(seq)) {
			bpf_iter_done_stop(seq);
		} else {
			seq->count = offs;
			if (offs == 0) {
				err = -E2BIG;
				goto done;
			}
		}
	}

	n = min(seq->count, size);
	err = copy_to_user(buf, seq->buf, n);
	if (err) {
		err = -EFAULT;
		goto done;
	}
	copied = n;
	seq->count -= n;
	seq->from = n;
done:
	if (!copied)
		copied = err;
	else
		*ppos += copied;
	mutex_unlock(&seq->lock);
	return copied;
}

static const struct bpf_iter_seq_info *
__get_seq_info(struct bpf_iter_link *link)
{
	const struct bpf_iter_seq_info *seq_info;

	if (link->aux.map) {
		seq_info = link->aux.map->ops->iter_seq_info;
		if (seq_info)
			return seq_info;
	}

	return link->tinfo->reg_info->seq_info;
}

static int iter_open(struct inode *inode, struct file *file)
{
	struct bpf_iter_link *link = inode->i_private;

	return prepare_seq_file(file, link, __get_seq_info(link));
}

static int iter_release(struct inode *inode, struct file *file)
{
	struct bpf_iter_priv_data *iter_priv;
	struct seq_file *seq;

	seq = file->private_data;
	if (!seq)
		return 0;

	iter_priv = container_of(seq->private, struct bpf_iter_priv_data,
				 target_private);

	if (iter_priv->seq_info->fini_seq_private)
		iter_priv->seq_info->fini_seq_private(seq->private);

	bpf_prog_put(iter_priv->prog);
	seq->private = iter_priv;

	return seq_release_private(inode, file);
}

const struct file_operations bpf_iter_fops = {
	.open		= iter_open,
	.llseek		= no_llseek,
	.read		= bpf_seq_read,
	.release	= iter_release,
};

/* The argument reg_info will be cached in bpf_iter_target_info.
 * The common practice is to declare target reg_info as
 * a const static variable and passed as an argument to
 * bpf_iter_reg_target().
 */
int bpf_iter_reg_target(const struct bpf_iter_reg *reg_info)
{
	struct bpf_iter_target_info *tinfo;

	tinfo = kmalloc(sizeof(*tinfo), GFP_KERNEL);
	if (!tinfo)
		return -ENOMEM;

	tinfo->reg_info = reg_info;
	INIT_LIST_HEAD(&tinfo->list);

	mutex_lock(&targets_mutex);
	list_add(&tinfo->list, &targets);
	mutex_unlock(&targets_mutex);

	return 0;
}

void bpf_iter_unreg_target(const struct bpf_iter_reg *reg_info)
{
	struct bpf_iter_target_info *tinfo;
	bool found = false;

	mutex_lock(&targets_mutex);
	list_for_each_entry(tinfo, &targets, list) {
		if (reg_info == tinfo->reg_info) {
			list_del(&tinfo->list);
			kfree(tinfo);
			found = true;
			break;
		}
	}
	mutex_unlock(&targets_mutex);

	WARN_ON(found == false);
}

static void cache_btf_id(struct bpf_iter_target_info *tinfo,
			 struct bpf_prog *prog)
{
	tinfo->btf_id = prog->aux->attach_btf_id;
}

bool bpf_iter_prog_supported(struct bpf_prog *prog)
{
	const char *attach_fname = prog->aux->attach_func_name;
	u32 prog_btf_id = prog->aux->attach_btf_id;
	const char *prefix = BPF_ITER_FUNC_PREFIX;
	struct bpf_iter_target_info *tinfo;
	int prefix_len = strlen(prefix);
	bool supported = false;

	if (strncmp(attach_fname, prefix, prefix_len))
		return false;

	mutex_lock(&targets_mutex);
	list_for_each_entry(tinfo, &targets, list) {
		if (tinfo->btf_id && tinfo->btf_id == prog_btf_id) {
			supported = true;
			break;
		}
		if (!strcmp(attach_fname + prefix_len, tinfo->reg_info->target)) {
			cache_btf_id(tinfo, prog);
			supported = true;
			break;
		}
	}
	mutex_unlock(&targets_mutex);

	if (supported) {
		prog->aux->ctx_arg_info_size = tinfo->reg_info->ctx_arg_info_size;
		prog->aux->ctx_arg_info = tinfo->reg_info->ctx_arg_info;
	}

	return supported;
}

static void bpf_iter_link_release(struct bpf_link *link)
{
	struct bpf_iter_link *iter_link =
		container_of(link, struct bpf_iter_link, link);

	if (iter_link->aux.map)
		bpf_map_put_with_uref(iter_link->aux.map);
}

static void bpf_iter_link_dealloc(struct bpf_link *link)
{
	struct bpf_iter_link *iter_link =
		container_of(link, struct bpf_iter_link, link);

	kfree(iter_link);
}

static int bpf_iter_link_replace(struct bpf_link *link,
				 struct bpf_prog *new_prog,
				 struct bpf_prog *old_prog)
{
	int ret = 0;

	mutex_lock(&link_mutex);
	if (old_prog && link->prog != old_prog) {
		ret = -EPERM;
		goto out_unlock;
	}

	if (link->prog->type != new_prog->type ||
	    link->prog->expected_attach_type != new_prog->expected_attach_type ||
	    link->prog->aux->attach_btf_id != new_prog->aux->attach_btf_id) {
		ret = -EINVAL;
		goto out_unlock;
	}

	old_prog = xchg(&link->prog, new_prog);
	bpf_prog_put(old_prog);

out_unlock:
	mutex_unlock(&link_mutex);
	return ret;
}

static const struct bpf_link_ops bpf_iter_link_lops = {
	.release = bpf_iter_link_release,
	.dealloc = bpf_iter_link_dealloc,
	.update_prog = bpf_iter_link_replace,
};

bool bpf_link_is_iter(struct bpf_link *link)
{
	return link->ops == &bpf_iter_link_lops;
}

int bpf_iter_link_attach(const union bpf_attr *attr, struct bpf_prog *prog)
{
	struct bpf_link_primer link_primer;
	struct bpf_iter_target_info *tinfo;
	struct bpf_iter_aux_info aux = {};
	struct bpf_iter_link *link;
	u32 prog_btf_id, target_fd;
	bool existed = false;
	struct bpf_map *map;
	int err;

	prog_btf_id = prog->aux->attach_btf_id;
	mutex_lock(&targets_mutex);
	list_for_each_entry(tinfo, &targets, list) {
		if (tinfo->btf_id == prog_btf_id) {
			existed = true;
			break;
		}
	}
	mutex_unlock(&targets_mutex);
	if (!existed)
		return -ENOENT;

	/* Make sure user supplied flags are target expected. */
	target_fd = attr->link_create.target_fd;
	if (attr->link_create.flags != tinfo->reg_info->req_linfo)
		return -EINVAL;
	if (!attr->link_create.flags && target_fd)
		return -EINVAL;

	link = kzalloc(sizeof(*link), GFP_USER | __GFP_NOWARN);
	if (!link)
		return -ENOMEM;

	bpf_link_init(&link->link, BPF_LINK_TYPE_ITER, &bpf_iter_link_lops, prog);
	link->tinfo = tinfo;

	err  = bpf_link_prime(&link->link, &link_primer);
	if (err) {
		kfree(link);
		return err;
	}

	if (tinfo->reg_info->req_linfo == BPF_ITER_LINK_MAP_FD) {
		map = bpf_map_get_with_uref(target_fd);
		if (IS_ERR(map)) {
			err = PTR_ERR(map);
			goto cleanup_link;
		}

		aux.map = map;
		err = tinfo->reg_info->check_target(prog, &aux);
		if (err) {
			bpf_map_put_with_uref(map);
			goto cleanup_link;
		}

		link->aux.map = map;
	}

	return bpf_link_settle(&link_primer);

cleanup_link:
	bpf_link_cleanup(&link_primer);
	return err;
}

static void init_seq_meta(struct bpf_iter_priv_data *priv_data,
			  struct bpf_iter_target_info *tinfo,
			  const struct bpf_iter_seq_info *seq_info,
			  struct bpf_prog *prog)
{
	priv_data->tinfo = tinfo;
	priv_data->seq_info = seq_info;
	priv_data->prog = prog;
	priv_data->session_id = atomic64_inc_return(&session_id);
	priv_data->seq_num = 0;
	priv_data->done_stop = false;
}

static int prepare_seq_file(struct file *file, struct bpf_iter_link *link,
			    const struct bpf_iter_seq_info *seq_info)
{
	struct bpf_iter_priv_data *priv_data;
	struct bpf_iter_target_info *tinfo;
	struct bpf_prog *prog;
	u32 total_priv_dsize;
	struct seq_file *seq;
	int err = 0;

	mutex_lock(&link_mutex);
	prog = link->link.prog;
	bpf_prog_inc(prog);
	mutex_unlock(&link_mutex);

	tinfo = link->tinfo;
	total_priv_dsize = offsetof(struct bpf_iter_priv_data, target_private) +
			   seq_info->seq_priv_size;
	priv_data = __seq_open_private(file, seq_info->seq_ops,
				       total_priv_dsize);
	if (!priv_data) {
		err = -ENOMEM;
		goto release_prog;
	}

	if (seq_info->init_seq_private) {
		err = seq_info->init_seq_private(priv_data->target_private, &link->aux);
		if (err)
			goto release_seq_file;
	}

	init_seq_meta(priv_data, tinfo, seq_info, prog);
	seq = file->private_data;
	seq->private = priv_data->target_private;

	return 0;

release_seq_file:
	seq_release_private(file->f_inode, file);
	file->private_data = NULL;
release_prog:
	bpf_prog_put(prog);
	return err;
}

int bpf_iter_new_fd(struct bpf_link *link)
{
	struct bpf_iter_link *iter_link;
	struct file *file;
	unsigned int flags;
	int err, fd;

	if (link->ops != &bpf_iter_link_lops)
		return -EINVAL;

	flags = O_RDONLY | O_CLOEXEC;
	fd = get_unused_fd_flags(flags);
	if (fd < 0)
		return fd;

	file = anon_inode_getfile("bpf_iter", &bpf_iter_fops, NULL, flags);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		goto free_fd;
	}

	iter_link = container_of(link, struct bpf_iter_link, link);
	err = prepare_seq_file(file, iter_link, __get_seq_info(iter_link));
	if (err)
		goto free_file;

	fd_install(fd, file);
	return fd;

free_file:
	fput(file);
free_fd:
	put_unused_fd(fd);
	return err;
}

struct bpf_prog *bpf_iter_get_info(struct bpf_iter_meta *meta, bool in_stop)
{
	struct bpf_iter_priv_data *iter_priv;
	struct seq_file *seq;
	void *seq_priv;

	seq = meta->seq;
	if (seq->file->f_op != &bpf_iter_fops)
		return NULL;

	seq_priv = seq->private;
	iter_priv = container_of(seq_priv, struct bpf_iter_priv_data,
				 target_private);

	if (in_stop && iter_priv->done_stop)
		return NULL;

	meta->session_id = iter_priv->session_id;
	meta->seq_num = iter_priv->seq_num;

	return iter_priv->prog;
}

int bpf_iter_run_prog(struct bpf_prog *prog, void *ctx)
{
	int ret;

	rcu_read_lock();
	migrate_disable();
	ret = BPF_PROG_RUN(prog, ctx);
	migrate_enable();
	rcu_read_unlock();

	/* bpf program can only return 0 or 1:
	 *  0 : okay
	 *  1 : retry the same object
	 * The bpf_iter_run_prog() return value
	 * will be seq_ops->show() return value.
	 */
	return ret == 0 ? 0 : -EAGAIN;
}
