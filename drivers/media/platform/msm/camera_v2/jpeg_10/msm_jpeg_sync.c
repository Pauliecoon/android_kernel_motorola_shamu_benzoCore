/* Copyright (c) 2012-2014, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <linux/module.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/compat.h>
#include <linux/ratelimit.h>
#include <media/msm_jpeg.h>
#include "msm_jpeg_sync.h"
#include "msm_jpeg_core.h"
#include "msm_jpeg_platform.h"
#include "msm_jpeg_common.h"

#define JPEG_REG_SIZE 0x308
#define JPEG_DEV_CNT 3
#define JPEG_DEC_ID 2
#define UINT32_MAX (0xFFFFFFFFU)

#ifdef CONFIG_COMPAT
#define MSM_JPEG_IOCTL_RESET32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 2, struct msm_jpeg_ctrl_cmd32)

#define MSM_JPEG_IOCTL_INPUT_BUF_ENQUEUE32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 5, struct msm_jpeg_buf32)

#define MSM_JPEG_IOCTL_INPUT_GET32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 6, struct msm_jpeg_buf32)

#define MSM_JPEG_IOCTL_OUTPUT_BUF_ENQUEUE32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 8, struct msm_jpeg_buf32)

#define MSM_JPEG_IOCTL_OUTPUT_GET32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 9, struct msm_jpeg_buf32)

#define MSM_JPEG_IOCTL_EVT_GET32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 11, struct msm_jpeg_ctrl_cmd32)

#define MSM_JPEG_IOCTL_TEST_DUMP_REGION32 \
	_IOW(MSM_JPEG_IOCTL_MAGIC, 15, compat_ulong_t)

struct msm_jpeg_ctrl_cmd32 {
	uint32_t type;
	uint32_t len;
	compat_uptr_t value;
};
struct msm_jpeg_buf32 {
	uint32_t type;
	int fd;

	compat_uptr_t vaddr;

	uint32_t y_off;
	uint32_t y_len;
	uint32_t framedone_len;

	uint32_t cbcr_off;
	uint32_t cbcr_len;

	uint32_t num_of_mcu_rows;
	uint32_t offset;
	uint32_t pln2_off;
	uint32_t pln2_len;
};

#endif


void msm_jpeg_q_init(char const *name, struct msm_jpeg_q *q_p)
{
	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, name);
	q_p->name = name;
	spin_lock_init(&q_p->lck);
	INIT_LIST_HEAD(&q_p->q);
	init_waitqueue_head(&q_p->wait);
	q_p->unblck = 0;
}

void *msm_jpeg_q_out(struct msm_jpeg_q *q_p)
{
	unsigned long flags;
	struct msm_jpeg_q_entry *q_entry_p = NULL;
	void *data = NULL;

	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
	spin_lock_irqsave(&q_p->lck, flags);
	if (!list_empty(&q_p->q)) {
		q_entry_p = list_first_entry(&q_p->q, struct msm_jpeg_q_entry,
			list);
		list_del_init(&q_entry_p->list);
	}
	spin_unlock_irqrestore(&q_p->lck, flags);

	if (q_entry_p) {
		data = q_entry_p->data;
		kfree(q_entry_p);
	} else {
		JPEG_DBG("%s:%d] %s no entry\n", __func__, __LINE__,
			q_p->name);
	}

	return data;
}

int msm_jpeg_q_in(struct msm_jpeg_q *q_p, void *data)
{
	unsigned long flags;

	struct msm_jpeg_q_entry *q_entry_p;

	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);

	q_entry_p = kmalloc(sizeof(struct msm_jpeg_q_entry), GFP_ATOMIC);
	if (!q_entry_p) {
		JPEG_PR_ERR("%s: no mem\n", __func__);
		return -EFAULT;
	}
	q_entry_p->data = data;

	spin_lock_irqsave(&q_p->lck, flags);
	list_add_tail(&q_entry_p->list, &q_p->q);
	spin_unlock_irqrestore(&q_p->lck, flags);

	return 0;
}

int msm_jpeg_q_in_buf(struct msm_jpeg_q *q_p,
	struct msm_jpeg_core_buf *buf)
{
	struct msm_jpeg_core_buf *buf_p;

	JPEG_DBG("%s:%d]\n", __func__, __LINE__);
	buf_p = kmalloc(sizeof(struct msm_jpeg_core_buf), GFP_ATOMIC);
	if (!buf_p) {
		JPEG_PR_ERR("%s: no mem\n", __func__);
		return -EFAULT;
	}

	memcpy(buf_p, buf, sizeof(struct msm_jpeg_core_buf));

	msm_jpeg_q_in(q_p, buf_p);
	return 0;
}

int msm_jpeg_q_wait(struct msm_jpeg_q *q_p)
{
	long tm = MAX_SCHEDULE_TIMEOUT; /* 500ms */
	int rc;

	JPEG_DBG("%s:%d] %s wait\n", __func__, __LINE__, q_p->name);
	rc = wait_event_interruptible_timeout(q_p->wait,
		(!list_empty_careful(&q_p->q) || q_p->unblck),
		msecs_to_jiffies(tm));
	JPEG_DBG("%s:%d] %s wait done\n", __func__, __LINE__, q_p->name);
	if (list_empty_careful(&q_p->q)) {
		if (rc == 0) {
			rc = -ETIMEDOUT;
			JPEG_PR_ERR("%s:%d] %s timeout\n", __func__, __LINE__,
				q_p->name);
		} else if (q_p->unblck) {
			JPEG_DBG("%s:%d] %s unblock is true\n", __func__,
				__LINE__, q_p->name);
			q_p->unblck = 0;
			rc = -ECANCELED;
		} else if (rc < 0) {
			JPEG_PR_ERR("%s:%d] %s rc %d\n", __func__, __LINE__,
				q_p->name, rc);
		}
	}
	return rc;
}

inline int msm_jpeg_q_wakeup(struct msm_jpeg_q *q_p)
{
	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
	wake_up(&q_p->wait);
	return 0;
}

inline int msm_jpeg_q_unblock(struct msm_jpeg_q *q_p)
{
	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
	q_p->unblck = 1;
	wake_up(&q_p->wait);
	return 0;
}

static inline void msm_jpeg_outbuf_q_cleanup(struct msm_jpeg_device *pgmn_dev,
	struct msm_jpeg_q *q_p, int domain_num)
{
	struct msm_jpeg_core_buf *buf_p;
	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
	do {
		buf_p = msm_jpeg_q_out(q_p);
		if (buf_p) {
			msm_jpeg_platform_p2v(pgmn_dev, buf_p->file,
				&buf_p->handle, domain_num);
			JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
			kfree(buf_p);
		}
	} while (buf_p);
	q_p->unblck = 0;
}

static inline void msm_jpeg_q_cleanup(struct msm_jpeg_q *q_p)
{
	void *data;
	JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
	do {
		data = msm_jpeg_q_out(q_p);
		if (data) {
			JPEG_DBG("%s:%d] %s\n", __func__, __LINE__, q_p->name);
			kfree(data);
		}
	} while (data);
	q_p->unblck = 0;
}

/*************** event queue ****************/

int msm_jpeg_framedone_irq(struct msm_jpeg_device *pgmn_dev,
	struct msm_jpeg_core_buf *buf_in)
{
	int rc = 0;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);

	if (buf_in) {
		buf_in->vbuf.framedone_len = buf_in->framedone_len;
		buf_in->vbuf.type = MSM_JPEG_EVT_SESSION_DONE;
		JPEG_DBG("%s:%d] 0x%08x %d framedone_len %d\n",
			__func__, __LINE__,
			(int) buf_in->y_buffer_addr, buf_in->y_len,
			buf_in->vbuf.framedone_len);
		rc = msm_jpeg_q_in_buf(&pgmn_dev->evt_q, buf_in);
	} else {
		JPEG_PR_ERR("%s:%d] no output return buffer\n",
			__func__, __LINE__);
		rc = -1;
	}

	if (buf_in)
		rc = msm_jpeg_q_wakeup(&pgmn_dev->evt_q);

	return rc;
}

int msm_jpeg_evt_get(struct msm_jpeg_device *pgmn_dev,
	void __user *to)
{
	struct msm_jpeg_core_buf *buf_p;
	struct msm_jpeg_ctrl_cmd ctrl_cmd;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);

	msm_jpeg_q_wait(&pgmn_dev->evt_q);
	buf_p = msm_jpeg_q_out(&pgmn_dev->evt_q);

	if (!buf_p) {
		JPEG_DBG("%s:%d] no buffer\n", __func__, __LINE__);
		return -EAGAIN;
	}

	memset(&ctrl_cmd, 0, sizeof(ctrl_cmd));
	ctrl_cmd.type = buf_p->vbuf.type;
	kfree(buf_p);

	JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
		(int) ctrl_cmd.value, ctrl_cmd.len);

	if (copy_to_user(to, &ctrl_cmd, sizeof(ctrl_cmd))) {
		JPEG_PR_ERR("%s:%d]\n", __func__, __LINE__);
		return -EFAULT;
	}

	return 0;
}

int msm_jpeg_evt_get_unblock(struct msm_jpeg_device *pgmn_dev)
{
	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	msm_jpeg_q_unblock(&pgmn_dev->evt_q);
	return 0;
}

void msm_jpeg_reset_ack_irq(struct msm_jpeg_device *pgmn_dev)
{
	JPEG_DBG("%s:%d]\n", __func__, __LINE__);
}

void msm_jpeg_err_irq(struct msm_jpeg_device *pgmn_dev,
	int event)
{
	int rc = 0;
	struct msm_jpeg_core_buf buf;

	JPEG_PR_ERR("%s:%d] error: %d\n", __func__, __LINE__, event);

	buf.vbuf.type = MSM_JPEG_EVT_ERR;
	rc = msm_jpeg_q_in_buf(&pgmn_dev->evt_q, &buf);
	if (!rc)
		rc = msm_jpeg_q_wakeup(&pgmn_dev->evt_q);

	if (!rc)
		JPEG_PR_ERR("%s:%d] err err\n", __func__, __LINE__);

	return;
}

/*************** output queue ****************/

int msm_jpeg_we_pingpong_irq(struct msm_jpeg_device *pgmn_dev,
	struct msm_jpeg_core_buf *buf_in)
{
	int rc = 0;
	struct msm_jpeg_core_buf *buf_out;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	if (buf_in) {
		JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
			(int) buf_in->y_buffer_addr, buf_in->y_len);
		rc = msm_jpeg_q_in_buf(&pgmn_dev->output_rtn_q, buf_in);
	} else {
		JPEG_DBG("%s:%d] no output return buffer\n", __func__,
			__LINE__);
		rc = -1;
		return rc;
	}

	buf_out = msm_jpeg_q_out(&pgmn_dev->output_buf_q);

	if (buf_out) {
		JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
			(int) buf_out->y_buffer_addr, buf_out->y_len);
		rc = msm_jpeg_core_we_buf_update(pgmn_dev, buf_out);
		kfree(buf_out);
	} else {
		msm_jpeg_core_we_buf_reset(pgmn_dev, buf_in);
		JPEG_DBG("%s:%d] no output buffer\n", __func__, __LINE__);
		rc = -2;
	}

	if (buf_in)
		rc = msm_jpeg_q_wakeup(&pgmn_dev->output_rtn_q);

	return rc;
}

int msm_jpeg_output_get(struct msm_jpeg_device *pgmn_dev, void __user *to)
{
	struct msm_jpeg_core_buf *buf_p;
	struct msm_jpeg_buf buf_cmd;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);

	msm_jpeg_q_wait(&pgmn_dev->output_rtn_q);
	buf_p = msm_jpeg_q_out(&pgmn_dev->output_rtn_q);

	if (!buf_p) {
		JPEG_DBG("%s:%d] no output buffer return\n",
			__func__, __LINE__);
		return -EAGAIN;
	}

	buf_cmd = buf_p->vbuf;
	msm_jpeg_platform_p2v(pgmn_dev, buf_p->file, &buf_p->handle,
		pgmn_dev->domain_num);
	kfree(buf_p);

	JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
		(int) buf_cmd.vaddr, buf_cmd.y_len);

	if (copy_to_user(to, &buf_cmd, sizeof(buf_cmd))) {
		JPEG_PR_ERR("%s:%d]", __func__, __LINE__);
		return -EFAULT;
	}

	return 0;
}

int msm_jpeg_output_get_unblock(struct msm_jpeg_device *pgmn_dev)
{
	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	msm_jpeg_q_unblock(&pgmn_dev->output_rtn_q);
	return 0;
}

static inline int msm_jpeg_add_u32_check(uint32_t *p, uint32_t n, uint32_t *res)
{
	*res = 0;

	while (n--) {
		if ((*res + *p) < *res)
			return -EFAULT;
		*res += *p++;
	}
	return 0;
}

int msm_jpeg_output_buf_enqueue(struct msm_jpeg_device *pgmn_dev,
	void __user *arg)
{
	struct msm_jpeg_buf buf_cmd;
	struct msm_jpeg_core_buf *buf_p;
	uint32_t buf_len_params[10];
	uint32_t total_len = 0;
	int n = 0;

	memset(&buf_cmd, 0x0, sizeof(struct msm_jpeg_buf));

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	if (copy_from_user(&buf_cmd, arg, sizeof(struct msm_jpeg_buf))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	buf_len_params[n++] = buf_cmd.y_len;
	buf_len_params[n++] = buf_cmd.cbcr_len;
	buf_len_params[n++] = buf_cmd.pln2_len;
	buf_len_params[n++] = buf_cmd.offset;
	buf_len_params[n++] = buf_cmd.y_off;
	if (msm_jpeg_add_u32_check(buf_len_params, n, &total_len) < 0) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	buf_p = kmalloc(sizeof(struct msm_jpeg_core_buf), GFP_ATOMIC);
	if (!buf_p) {
		JPEG_PR_ERR("%s:%d] no mem\n", __func__, __LINE__);
		return -EFAULT;
	}

	JPEG_DBG("%s:%d] vaddr = 0x%08lx y_len = %d\n, fd = %d",
		__func__, __LINE__, (unsigned long) buf_cmd.vaddr,
		buf_cmd.y_len, buf_cmd.fd);

	buf_p->y_buffer_addr = msm_jpeg_platform_v2p(pgmn_dev, buf_cmd.fd,
		total_len, &buf_p->file, &buf_p->handle, pgmn_dev->domain_num);

	if (!buf_p->y_buffer_addr) {
		JPEG_PR_ERR("%s:%d] v2p wrong\n", __func__, __LINE__);
		kfree(buf_p);
		return -EFAULT;
	}

	buf_p->y_buffer_addr += buf_cmd.offset + buf_cmd.y_off;

	if (buf_cmd.cbcr_len)
		buf_p->cbcr_buffer_addr = buf_p->y_buffer_addr +
			buf_cmd.y_len;
	else
		buf_p->cbcr_buffer_addr = 0x0;

	if (buf_cmd.pln2_len)
		buf_p->pln2_addr = buf_p->cbcr_buffer_addr +
			buf_cmd.cbcr_len;
	else
		buf_p->pln2_addr = 0x0;

	JPEG_DBG("%s:%d]After v2p pln0_addr %x pln0_len %d",
		__func__, __LINE__, buf_p->y_buffer_addr,
		buf_cmd.y_len);

	JPEG_DBG("pl1_len %d, pln1_addr %x, pln2_adrr %x,pln2_len %d",
		buf_cmd.cbcr_len, buf_p->cbcr_buffer_addr,
		buf_p->pln2_addr, buf_cmd.pln2_len);

	buf_p->y_len = buf_cmd.y_len;
	buf_p->cbcr_len = buf_cmd.cbcr_len;
	buf_p->pln2_len = buf_cmd.pln2_len;
	buf_p->vbuf = buf_cmd;

	msm_jpeg_q_in(&pgmn_dev->output_buf_q, buf_p);
	return 0;
}

/*************** input queue ****************/

int msm_jpeg_fe_pingpong_irq(struct msm_jpeg_device *pgmn_dev,
	struct msm_jpeg_core_buf *buf_in)
{
	struct msm_jpeg_core_buf *buf_out;
	int rc = 0;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	if (buf_in) {
		JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
			(int) buf_in->y_buffer_addr, buf_in->y_len);
		rc = msm_jpeg_q_in_buf(&pgmn_dev->input_rtn_q, buf_in);
	} else {
		JPEG_DBG("%s:%d] no input return buffer\n", __func__,
			__LINE__);
		rc = -EFAULT;
	}

	buf_out = msm_jpeg_q_out(&pgmn_dev->input_buf_q);

	if (buf_out) {
		rc = msm_jpeg_core_fe_buf_update(pgmn_dev, buf_out);
		kfree(buf_out);
		msm_jpeg_core_fe_start(pgmn_dev);
	} else {
		JPEG_DBG("%s:%d] no input buffer\n", __func__, __LINE__);
		rc = -EFAULT;
	}

	if (buf_in)
		rc = msm_jpeg_q_wakeup(&pgmn_dev->input_rtn_q);

	return rc;
}

int msm_jpeg_input_get(struct msm_jpeg_device *pgmn_dev, void __user *to)
{
	struct msm_jpeg_core_buf *buf_p;
	struct msm_jpeg_buf buf_cmd;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	msm_jpeg_q_wait(&pgmn_dev->input_rtn_q);
	buf_p = msm_jpeg_q_out(&pgmn_dev->input_rtn_q);

	if (!buf_p) {
		JPEG_DBG("%s:%d] no input buffer return\n",
			__func__, __LINE__);
		return -EAGAIN;
	}

	buf_cmd = buf_p->vbuf;
	msm_jpeg_platform_p2v(pgmn_dev, buf_p->file, &buf_p->handle,
		pgmn_dev->domain_num);
	kfree(buf_p);

	JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
		(int) buf_cmd.vaddr, buf_cmd.y_len);

	if (copy_to_user(to, &buf_cmd, sizeof(buf_cmd))) {
		JPEG_PR_ERR("%s:%d]\n", __func__, __LINE__);
		return -EFAULT;
	}

	return 0;
}

int msm_jpeg_input_get_unblock(struct msm_jpeg_device *pgmn_dev)
{
	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	msm_jpeg_q_unblock(&pgmn_dev->input_rtn_q);
	return 0;
}

int msm_jpeg_input_buf_enqueue(struct msm_jpeg_device *pgmn_dev,
	void __user *arg)
{
	struct msm_jpeg_core_buf *buf_p;
	struct msm_jpeg_buf buf_cmd;
	uint32_t buf_len_params[10];
	uint32_t total_len = 0;
	int n = 0;

	memset(&buf_cmd, 0x0, sizeof(struct msm_jpeg_buf));

	if (copy_from_user(&buf_cmd, arg, sizeof(struct msm_jpeg_buf))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	buf_len_params[n++] = buf_cmd.y_len;
	buf_len_params[n++] = buf_cmd.cbcr_len;
	buf_len_params[n++] = buf_cmd.pln2_len;
	buf_len_params[n++] = buf_cmd.offset;
	buf_len_params[n++] = buf_cmd.y_off;
	if (buf_cmd.cbcr_len)
		buf_len_params[n++] = buf_cmd.cbcr_off;
	if (buf_cmd.pln2_len)
		buf_len_params[n++] = buf_cmd.pln2_off;

	if (msm_jpeg_add_u32_check(buf_len_params, n, &total_len) < 0) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	buf_p = kmalloc(sizeof(struct msm_jpeg_core_buf), GFP_ATOMIC);
	if (!buf_p) {
		JPEG_PR_ERR("%s:%d] no mem\n", __func__, __LINE__);
		return -EFAULT;
	}

	JPEG_DBG("%s:%d] 0x%08x %d\n", __func__, __LINE__,
		(int) buf_cmd.vaddr, buf_cmd.y_len);

	buf_p->y_buffer_addr    = msm_jpeg_platform_v2p(pgmn_dev, buf_cmd.fd,
		total_len, &buf_p->file, &buf_p->handle, pgmn_dev->domain_num);

	if (!buf_p->y_buffer_addr) {
		JPEG_PR_ERR("%s:%d] v2p wrong\n", __func__, __LINE__);
		kfree(buf_p);
		return -EFAULT;
	}

	buf_p->y_buffer_addr += buf_cmd.offset + buf_cmd.y_off;

	buf_p->y_len          = buf_cmd.y_len;
	buf_p->cbcr_len       = buf_cmd.cbcr_len;
	buf_p->pln2_len       = buf_cmd.pln2_len;
	buf_p->num_of_mcu_rows = buf_cmd.num_of_mcu_rows;

	if (buf_cmd.cbcr_len)
		buf_p->cbcr_buffer_addr = buf_p->y_buffer_addr +
		buf_cmd.y_len + buf_cmd.cbcr_off;
	else
		buf_p->cbcr_buffer_addr = 0x0;

	if (buf_cmd.pln2_len)
		buf_p->pln2_addr = buf_p->cbcr_buffer_addr +
		buf_cmd.cbcr_len + buf_cmd.pln2_off;
	else
		buf_p->pln2_addr = 0x0;

	JPEG_DBG("%s: y_addr=%x, y_len=%x, cbcr_addr=%x, cbcr_len=%d",
		__func__, buf_p->y_buffer_addr, buf_p->y_len,
		buf_p->cbcr_buffer_addr, buf_p->cbcr_len);
	JPEG_DBG("pln2_addr = %x, pln2_len = %d, fd =%d\n",
		buf_p->pln2_addr, buf_p->pln2_len, buf_cmd.fd);

	buf_p->vbuf           = buf_cmd;

	msm_jpeg_q_in(&pgmn_dev->input_buf_q, buf_p);

	return 0;
}

int msm_jpeg_irq(int event, void *context, void *data)
{
	struct msm_jpeg_device *pgmn_dev =
		(struct msm_jpeg_device *) context;

	switch (event) {
	case MSM_JPEG_EVT_SESSION_DONE:
		msm_jpeg_framedone_irq(pgmn_dev, data);
		msm_jpeg_we_pingpong_irq(pgmn_dev, data);
		break;

	case MSM_JPEG_HW_MASK_COMP_FE:
		msm_jpeg_fe_pingpong_irq(pgmn_dev, data);
		break;

	case MSM_JPEG_HW_MASK_COMP_WE:
		msm_jpeg_we_pingpong_irq(pgmn_dev, data);
		break;

	case MSM_JPEG_HW_MASK_COMP_RESET_ACK:
		msm_jpeg_reset_ack_irq(pgmn_dev);
		break;

	case MSM_JPEG_HW_MASK_COMP_ERR:
	default:
		msm_jpeg_err_irq(pgmn_dev, event);
		break;
	}

	return 0;
}

int __msm_jpeg_open(struct msm_jpeg_device *pgmn_dev)
{
	int rc;

	mutex_lock(&pgmn_dev->lock);
	if (pgmn_dev->open_count) {
		/* only open once */
		JPEG_PR_ERR("%s:%d] busy\n", __func__, __LINE__);
		mutex_unlock(&pgmn_dev->lock);
		return -EBUSY;
	}
	pgmn_dev->open_count++;
	mutex_unlock(&pgmn_dev->lock);

	msm_jpeg_core_irq_install(msm_jpeg_irq);
	rc = msm_jpeg_platform_init(pgmn_dev->pdev,
		&pgmn_dev->mem, &pgmn_dev->base,
		&pgmn_dev->irq, msm_jpeg_core_irq, pgmn_dev);
	if (rc) {
		JPEG_PR_ERR("%s:%d] platform_init fail %d\n", __func__,
			__LINE__, rc);
		return rc;
	}

	JPEG_DBG("%s:%d] platform resources - mem %p, base %p, irq %d\n",
		__func__, __LINE__,
		pgmn_dev->mem, pgmn_dev->base, pgmn_dev->irq);
	pgmn_dev->res_size = resource_size(pgmn_dev->mem);

	msm_jpeg_q_cleanup(&pgmn_dev->evt_q);
	msm_jpeg_q_cleanup(&pgmn_dev->output_rtn_q);
	msm_jpeg_outbuf_q_cleanup(pgmn_dev, &pgmn_dev->output_buf_q,
		pgmn_dev->domain_num);
	msm_jpeg_q_cleanup(&pgmn_dev->input_rtn_q);
	msm_jpeg_q_cleanup(&pgmn_dev->input_buf_q);
	msm_jpeg_core_init(pgmn_dev);

	JPEG_DBG("%s:%d] success\n", __func__, __LINE__);
	return rc;
}

int __msm_jpeg_release(struct msm_jpeg_device *pgmn_dev)
{
	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	mutex_lock(&pgmn_dev->lock);
	if (!pgmn_dev->open_count) {
		JPEG_PR_ERR(KERN_ERR "%s: not opened\n", __func__);
		mutex_unlock(&pgmn_dev->lock);
		return -EINVAL;
	}
	pgmn_dev->open_count--;
	mutex_unlock(&pgmn_dev->lock);

	msm_jpeg_core_release(pgmn_dev, pgmn_dev->domain_num);
	msm_jpeg_q_cleanup(&pgmn_dev->evt_q);
	msm_jpeg_q_cleanup(&pgmn_dev->output_rtn_q);
	msm_jpeg_outbuf_q_cleanup(pgmn_dev, &pgmn_dev->output_buf_q,
		pgmn_dev->domain_num);
	msm_jpeg_q_cleanup(&pgmn_dev->input_rtn_q);
	msm_jpeg_outbuf_q_cleanup(pgmn_dev, &pgmn_dev->input_buf_q,
		pgmn_dev->domain_num);

	JPEG_DBG("%s:%d]\n", __func__, __LINE__);
	if (pgmn_dev->open_count)
		JPEG_PR_ERR(KERN_ERR "%s: multiple opens\n", __func__);

	msm_jpeg_platform_release(pgmn_dev->mem, pgmn_dev->base,
		pgmn_dev->irq, pgmn_dev);

	JPEG_DBG("%s:%d]\n", __func__, __LINE__);
	return 0;
}

int msm_jpeg_ioctl_hw_cmd(struct msm_jpeg_device *pgmn_dev,
	void * __user arg)
{
	struct msm_jpeg_hw_cmd hw_cmd;
	int is_copy_to_user;

	if (copy_from_user(&hw_cmd, arg, sizeof(struct msm_jpeg_hw_cmd))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	is_copy_to_user = msm_jpeg_hw_exec_cmds(&hw_cmd, 1,
		pgmn_dev->res_size, pgmn_dev->base);
	JPEG_DBG("%s:%d] type %d, n %d, offset %d, mask %x, data %x,pdata %x\n",
		__func__, __LINE__, hw_cmd.type, hw_cmd.n, hw_cmd.offset,
		hw_cmd.mask, hw_cmd.data, (int) hw_cmd.pdata);

	if (is_copy_to_user >= 0) {
		if (copy_to_user(arg, &hw_cmd, sizeof(hw_cmd))) {
			JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
			return -EFAULT;
		}
	} else {
		return is_copy_to_user;
	}

	return 0;
}

int msm_jpeg_ioctl_hw_cmds(struct msm_jpeg_device *pgmn_dev,
	void * __user arg)
{
	int is_copy_to_user;
	uint32_t len;
	uint32_t m;
	struct msm_jpeg_hw_cmds *hw_cmds_p;
	struct msm_jpeg_hw_cmd *hw_cmd_p;

	if (copy_from_user(&m, arg, sizeof(m))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	if ((m == 0) || (m > ((UINT32_MAX - sizeof(struct msm_jpeg_hw_cmds)) /
		sizeof(struct msm_jpeg_hw_cmd)))) {
		JPEG_PR_ERR("%s:%d] m_cmds out of range\n", __func__, __LINE__);
		return -EFAULT;
	}

	len = sizeof(struct msm_jpeg_hw_cmds) +
		sizeof(struct msm_jpeg_hw_cmd) * (m - 1);
	hw_cmds_p = kmalloc(len, GFP_KERNEL);
	if (!hw_cmds_p) {
		JPEG_PR_ERR("%s:%d] no mem %d\n", __func__, __LINE__, len);
		return -EFAULT;
	}

	if (copy_from_user(hw_cmds_p, arg, len)) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		kfree(hw_cmds_p);
		return -EFAULT;
	}

	hw_cmd_p = (struct msm_jpeg_hw_cmd *) &(hw_cmds_p->hw_cmd);

	is_copy_to_user = msm_jpeg_hw_exec_cmds(hw_cmd_p, m,
		 pgmn_dev->res_size, pgmn_dev->base);

	if (is_copy_to_user >= 0) {
		if (copy_to_user(arg, hw_cmds_p, len)) {
			JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
			kfree(hw_cmds_p);
			return -EFAULT;
		}
	} else {
		kfree(hw_cmds_p);
		return is_copy_to_user;
	}
	kfree(hw_cmds_p);
	return 0;
}

int msm_jpeg_start(struct msm_jpeg_device *pgmn_dev, void * __user arg)
{
	struct msm_jpeg_core_buf *buf_out;
	struct msm_jpeg_core_buf *buf_out_free[2] = {NULL, NULL};
	int i, rc;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);

	pgmn_dev->release_buf = 1;
	for (i = 0; i < 2; i++) {
		buf_out = msm_jpeg_q_out(&pgmn_dev->input_buf_q);

		if (buf_out) {
			msm_jpeg_core_fe_buf_update(pgmn_dev, buf_out);
			kfree(buf_out);
		} else {
			JPEG_DBG("%s:%d] no input buffer\n", __func__,
					__LINE__);
			break;
		}
	}

	for (i = 0; i < 2; i++) {
		buf_out_free[i] = msm_jpeg_q_out(&pgmn_dev->output_buf_q);

		if (buf_out_free[i]) {
			msm_jpeg_core_we_buf_update(pgmn_dev, buf_out_free[i]);
			pgmn_dev->release_buf = 0;
		} else {
			JPEG_DBG("%s:%d] no output buffer\n",
			__func__, __LINE__);
			break;
		}
	}

	for (i = 0; i < 2; i++)
		kfree(buf_out_free[i]);

	pgmn_dev->state = MSM_JPEG_EXECUTING;
	JPEG_DBG_HIGH("%s:%d] START\n", __func__, __LINE__);
	wmb();
	rc = msm_jpeg_ioctl_hw_cmds(pgmn_dev, arg);
	wmb();

	JPEG_DBG("%s:%d]", __func__, __LINE__);
	return rc;
}

int msm_jpeg_ioctl_reset(struct msm_jpeg_device *pgmn_dev,
	void * __user arg)
{
	int rc;
	struct msm_jpeg_ctrl_cmd ctrl_cmd;

	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);

	if (pgmn_dev->state == MSM_JPEG_INIT) {
		if (copy_from_user(&ctrl_cmd, arg, sizeof(ctrl_cmd))) {
			JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
			return -EFAULT;
		}
	pgmn_dev->op_mode = ctrl_cmd.type;

	rc = msm_jpeg_core_reset(pgmn_dev, pgmn_dev->op_mode, pgmn_dev->base,
		resource_size(pgmn_dev->mem));
	} else {
		JPEG_PR_ERR("%s:%d] JPEG not been initialized Wrong state\n",
			__func__, __LINE__);
		rc = -1;
	}
	return rc;
}

int msm_jpeg_ioctl_test_dump_region(struct msm_jpeg_device *pgmn_dev,
	unsigned long arg)
{
	JPEG_DBG("%s:%d] Enter\n", __func__, __LINE__);
	msm_jpeg_io_dump(pgmn_dev->base, JPEG_REG_SIZE);
	return 0;
}

int msm_jpeg_ioctl_set_clk_rate(struct msm_jpeg_device *pgmn_dev,
	void * __user arg)
{
	long clk_rate;
	int rc;

	if ((pgmn_dev->state != MSM_JPEG_INIT) &&
		(pgmn_dev->state != MSM_JPEG_RESET)) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	if (get_user(clk_rate, (unsigned int __user *)arg)) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	JPEG_DBG("%s:%d] Requested clk rate %ld\n", __func__, __LINE__,
		clk_rate);
	if (clk_rate < 0) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	rc = msm_jpeg_platform_set_clk_rate(pgmn_dev, clk_rate);
	if (rc < 0) {
		JPEG_PR_ERR("%s: clk failed rc = %d\n", __func__, rc);
		return -EFAULT;
	}

	return 0;
}
#ifdef CONFIG_COMPAT
int msm_jpeg_get_compat_ctrl_cmd(struct msm_jpeg_ctrl_cmd *ctrl_cmd,
	void __user  *arg)
{
	struct msm_jpeg_ctrl_cmd32 ctrl_cmd32;
	unsigned long temp;
	if (copy_from_user(&ctrl_cmd32, arg,
		sizeof(struct msm_jpeg_ctrl_cmd32))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	ctrl_cmd->type = ctrl_cmd32.type;
	ctrl_cmd->len = ctrl_cmd32.len;
	temp = (unsigned long) ctrl_cmd32.value;
	ctrl_cmd->value = (void *) temp;

	return 0;
}
int msm_jpeg_put_compat_ctrl_cmd(struct msm_jpeg_ctrl_cmd *ctrl_cmd,
	void __user  *arg)
{
	struct msm_jpeg_ctrl_cmd32 ctrl_cmd32;
	unsigned long temp;

	ctrl_cmd32.type   = ctrl_cmd->type;
	ctrl_cmd32.len    = ctrl_cmd->len;
	temp = (unsigned long) ctrl_cmd->value;
	ctrl_cmd32.value  = (compat_uptr_t) temp;

	if (copy_from_user(arg, &ctrl_cmd32,
		sizeof(struct msm_jpeg_ctrl_cmd32))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}

	return 0;
}

int msm_jpeg_get_jpeg_buf(struct msm_jpeg_buf *jpeg_buf,
	void __user  *arg)
{
	struct msm_jpeg_buf32 jpeg_buf32;
	unsigned long temp;
	if (copy_from_user(&jpeg_buf32, arg, sizeof(struct msm_jpeg_buf32))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	jpeg_buf->type            = jpeg_buf32.type;
	jpeg_buf->fd              = jpeg_buf32.fd;
	temp = (unsigned long) jpeg_buf32.vaddr;
	jpeg_buf->vaddr           = (void *) temp;
	jpeg_buf->y_off           = jpeg_buf32.y_off;
	jpeg_buf->y_len           = jpeg_buf32.y_len;
	jpeg_buf->framedone_len   = jpeg_buf32.framedone_len;
	jpeg_buf->cbcr_off        = jpeg_buf32.cbcr_off;
	jpeg_buf->cbcr_len        = jpeg_buf32.cbcr_len;
	jpeg_buf->num_of_mcu_rows = jpeg_buf32.num_of_mcu_rows;
	jpeg_buf->offset          = jpeg_buf32.offset;
	jpeg_buf->pln2_off        = jpeg_buf32.pln2_off;
	jpeg_buf->pln2_len        = jpeg_buf32.pln2_len;

	return 0;
}
int msm_jpeg_put_jpeg_buf(struct msm_jpeg_buf *jpeg_buf,
	void __user  *arg)
{
	struct msm_jpeg_buf32 jpeg_buf32;
	unsigned long temp;

	jpeg_buf32.type            = jpeg_buf->type;
	jpeg_buf32.fd              = jpeg_buf->fd;
	temp = (unsigned long) jpeg_buf->vaddr;
	jpeg_buf32.vaddr           = (compat_uptr_t) temp;
	jpeg_buf32.y_off           = jpeg_buf->y_off;
	jpeg_buf32.y_len           = jpeg_buf->y_len;
	jpeg_buf32.framedone_len   = jpeg_buf->framedone_len;
	jpeg_buf32.cbcr_off        = jpeg_buf->cbcr_off;
	jpeg_buf32.cbcr_len        = jpeg_buf->cbcr_len;
	jpeg_buf32.num_of_mcu_rows = jpeg_buf->num_of_mcu_rows;
	jpeg_buf32.offset          = jpeg_buf->offset;
	jpeg_buf32.pln2_off        = jpeg_buf->pln2_off;
	jpeg_buf32.pln2_len        = jpeg_buf->pln2_len;

	if (copy_to_user(arg, &jpeg_buf32, sizeof(struct msm_jpeg_buf32))) {
		JPEG_PR_ERR("%s:%d] failed\n", __func__, __LINE__);
		return -EFAULT;
	}
	return 0;
}

long __msm_jpeg_compat_ioctl(struct msm_jpeg_device *pgmn_dev,
	unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	struct msm_jpeg_ctrl_cmd ctrl_cmd;
	struct msm_jpeg_buf jpeg_buf;
	switch (cmd) {
	case MSM_JPEG_IOCTL_GET_HW_VERSION:
		JPEG_DBG("%s:%d] VERSION 1\n", __func__, __LINE__);
		rc = msm_jpeg_ioctl_hw_cmd(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_RESET:
		rc = msm_jpeg_ioctl_reset(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_RESET32:
		rc = msm_jpeg_get_compat_ctrl_cmd(&ctrl_cmd,
			(void __user *) arg);
		if (rc < 0)
			break;
		rc = msm_jpeg_ioctl_reset(pgmn_dev, (void __user *) &ctrl_cmd);
		break;

	case MSM_JPEG_IOCTL_STOP:
		rc = msm_jpeg_ioctl_hw_cmds(pgmn_dev, (void __user *) arg);
		pgmn_dev->state = MSM_JPEG_STOPPED;
		break;

	case MSM_JPEG_IOCTL_START:
		rc = msm_jpeg_start(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_INPUT_BUF_ENQUEUE:
		rc = msm_jpeg_input_buf_enqueue(pgmn_dev,
			(void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_INPUT_BUF_ENQUEUE32:
		rc = msm_jpeg_get_jpeg_buf(&jpeg_buf, (void __user *) arg);
		if (rc < 0)
			break;
		rc = msm_jpeg_input_buf_enqueue(pgmn_dev,
			(void __user *) &jpeg_buf);
		break;

	case MSM_JPEG_IOCTL_INPUT_GET:
		rc = msm_jpeg_input_get(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_INPUT_GET32:
		rc = msm_jpeg_input_get(pgmn_dev, (void __user *) &jpeg_buf);
		if (rc < 0)
			break;
		rc = msm_jpeg_put_jpeg_buf(&jpeg_buf, (void __user *) arg);

		break;

	case MSM_JPEG_IOCTL_INPUT_GET_UNBLOCK:
		rc = msm_jpeg_input_get_unblock(pgmn_dev);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_BUF_ENQUEUE:
		rc = msm_jpeg_output_buf_enqueue(pgmn_dev,
			(void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_BUF_ENQUEUE32:
		rc = msm_jpeg_get_jpeg_buf(&jpeg_buf, (void __user *) arg);
		if (rc < 0)
			break;
		rc = msm_jpeg_output_buf_enqueue(pgmn_dev,
			(void __user *) &jpeg_buf);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_GET:
		rc = msm_jpeg_output_get(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_GET32:
		rc = msm_jpeg_output_get(pgmn_dev, (void __user *) &jpeg_buf);
		if (rc < 0)
			break;
		rc = msm_jpeg_put_jpeg_buf(&jpeg_buf, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_GET_UNBLOCK:
		rc = msm_jpeg_output_get_unblock(pgmn_dev);
		break;

	case MSM_JPEG_IOCTL_EVT_GET:
		rc = msm_jpeg_evt_get(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_EVT_GET32:
		rc = msm_jpeg_evt_get(pgmn_dev, (void __user *) &ctrl_cmd);
		if (rc < 0)
			break;
		msm_jpeg_put_compat_ctrl_cmd(&ctrl_cmd, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_EVT_GET_UNBLOCK:
		rc = msm_jpeg_evt_get_unblock(pgmn_dev);
		break;

	case MSM_JPEG_IOCTL_HW_CMD:
		rc = msm_jpeg_ioctl_hw_cmd(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_HW_CMDS:
		rc = msm_jpeg_ioctl_hw_cmds(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_TEST_DUMP_REGION:
		rc = msm_jpeg_ioctl_test_dump_region(pgmn_dev, arg);
		break;

	case MSM_JPEG_IOCTL_TEST_DUMP_REGION32:
		rc = msm_jpeg_ioctl_test_dump_region(pgmn_dev, arg);
		break;

	case MSM_JPEG_IOCTL_SET_CLK_RATE:
		rc = msm_jpeg_ioctl_set_clk_rate(pgmn_dev,
			(void __user *) arg);
		break;

	default:
		JPEG_PR_ERR(KERN_INFO "%s:%d] cmd = %d not supported\n",
		__func__, __LINE__, _IOC_NR(cmd));
		rc = -EINVAL;
		break;
	}
	return rc;
}
#else
long __msm_jpeg_compat_ioctl(struct msm_jpeg_device *pgmn_dev,
	unsigned int cmd, unsigned long arg)
{
	return 0;
}
#endif

long __msm_jpeg_ioctl(struct msm_jpeg_device *pgmn_dev,
	unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	switch (cmd) {
	case MSM_JPEG_IOCTL_GET_HW_VERSION:
		JPEG_DBG("%s:%d] VERSION 1\n", __func__, __LINE__);
		rc = msm_jpeg_ioctl_hw_cmd(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_RESET:
		rc = msm_jpeg_ioctl_reset(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_STOP:
		rc = msm_jpeg_ioctl_hw_cmds(pgmn_dev, (void __user *) arg);
		pgmn_dev->state = MSM_JPEG_STOPPED;
		break;

	case MSM_JPEG_IOCTL_START:
		rc = msm_jpeg_start(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_INPUT_BUF_ENQUEUE:
		rc = msm_jpeg_input_buf_enqueue(pgmn_dev,
			(void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_INPUT_GET:
		rc = msm_jpeg_input_get(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_INPUT_GET_UNBLOCK:
		rc = msm_jpeg_input_get_unblock(pgmn_dev);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_BUF_ENQUEUE:
		rc = msm_jpeg_output_buf_enqueue(pgmn_dev,
			(void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_GET:
		rc = msm_jpeg_output_get(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_OUTPUT_GET_UNBLOCK:
		rc = msm_jpeg_output_get_unblock(pgmn_dev);
		break;

	case MSM_JPEG_IOCTL_EVT_GET:
		rc = msm_jpeg_evt_get(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_EVT_GET_UNBLOCK:
		rc = msm_jpeg_evt_get_unblock(pgmn_dev);
		break;

	case MSM_JPEG_IOCTL_HW_CMD:
		rc = msm_jpeg_ioctl_hw_cmd(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_HW_CMDS:
		rc = msm_jpeg_ioctl_hw_cmds(pgmn_dev, (void __user *) arg);
		break;

	case MSM_JPEG_IOCTL_TEST_DUMP_REGION:
		rc = msm_jpeg_ioctl_test_dump_region(pgmn_dev, arg);
		break;

	case MSM_JPEG_IOCTL_SET_CLK_RATE:
		rc = msm_jpeg_ioctl_set_clk_rate(pgmn_dev, (void __user *) arg);
		break;
	default:
		pr_err_ratelimited("%s:%d] cmd = %d not supported\n",
			__func__, __LINE__, _IOC_NR(cmd));
		rc = -EINVAL;
		break;
	}
	return rc;
}
#ifdef CONFIG_MSM_IOMMU
static int camera_register_domain(void)
{
	struct msm_iova_partition camera_fw_partition = {
		.start = SZ_128K,
		.size = SZ_2G - SZ_128K,
	};

	struct msm_iova_layout camera_fw_layout = {
		.partitions = &camera_fw_partition,
		.npartitions = 1,
		.client_name = "camera_jpeg",
		.domain_flags = 0,
	};
	return msm_register_domain(&camera_fw_layout);
}
#endif

int __msm_jpeg_init(struct msm_jpeg_device *pgmn_dev)
{
	int rc = 0;
	int idx = 0;
#ifdef CONFIG_MSM_IOMMU
	int i = 0, j = 0;
	char *iommu_name[JPEG_DEV_CNT] = {"jpeg_enc0", "jpeg_enc1",
		"jpeg_dec"};
#endif

	mutex_init(&pgmn_dev->lock);

	pr_err("%s:%d] Jpeg Device id %d", __func__, __LINE__,
		   pgmn_dev->pdev->id);
	idx = pgmn_dev->pdev->id;
	pgmn_dev->idx = idx;
	pgmn_dev->iommu_cnt = 1;
	pgmn_dev->decode_flag = (idx == JPEG_DEC_ID);

	msm_jpeg_q_init("evt_q", &pgmn_dev->evt_q);
	msm_jpeg_q_init("output_rtn_q", &pgmn_dev->output_rtn_q);
	msm_jpeg_q_init("output_buf_q", &pgmn_dev->output_buf_q);
	msm_jpeg_q_init("input_rtn_q", &pgmn_dev->input_rtn_q);
	msm_jpeg_q_init("input_buf_q", &pgmn_dev->input_buf_q);

#ifdef CONFIG_MSM_IOMMU
	j = (pgmn_dev->iommu_cnt <= 1) ? idx : 0;
	/*get device context for IOMMU*/
	for (i = 0; i < pgmn_dev->iommu_cnt; i++) {
		pgmn_dev->iommu_ctx_arr[i] = msm_iommu_get_ctx(iommu_name[j]);
		JPEG_DBG("%s:%d] name %s", __func__, __LINE__, iommu_name[j]);
		JPEG_DBG("%s:%d] ctx 0x%x", __func__, __LINE__,
			(uint32_t)pgmn_dev->iommu_ctx_arr[i]);
		if (!pgmn_dev->iommu_ctx_arr[i]) {
			JPEG_PR_ERR("%s: No iommu fw context found\n",
					__func__);
			goto error;
		}
		j++;
	}
	pgmn_dev->domain_num = camera_register_domain();
	JPEG_DBG("%s:%d] dom_num 0x%x", __func__, __LINE__,
		pgmn_dev->domain_num);
	if (pgmn_dev->domain_num < 0) {
		JPEG_PR_ERR("%s: could not register domain\n", __func__);
		goto error;
	}
	pgmn_dev->domain = msm_get_iommu_domain(pgmn_dev->domain_num);
	JPEG_DBG("%s:%d] dom 0x%x", __func__, __LINE__,
					(uint32_t)pgmn_dev->domain);
	if (!pgmn_dev->domain) {
		JPEG_PR_ERR("%s: cannot find domain\n", __func__);
		goto error;
	}
#endif

	return rc;
#ifdef CONFIG_MSM_IOMMU
error:
#endif
	mutex_destroy(&pgmn_dev->lock);
	return -EFAULT;
}

int __msm_jpeg_exit(struct msm_jpeg_device *pgmn_dev)
{
	mutex_destroy(&pgmn_dev->lock);
	kfree(pgmn_dev);
	return 0;
}
