/*
    V4L2 API compliance buffer ioctl tests.

    Copyright (C) 2012  Hans Verkuil <hverkuil@xs4all.nl>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA  02110-1335  USA
 */

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <sys/ioctl.h>
#include "v4l2-compliance.h"

static inline void *test_mmap(void *start, size_t length, int prot, int flags,
		int fd, int64_t offset)
{
 	return wrapper ? v4l2_mmap(start, length, prot, flags, fd, offset) :
		mmap(start, length, prot, flags, fd, offset);
}

static void *ptrs[VIDEO_MAX_FRAME];
static struct v4l2_format cur_fmt;
static int last_seq;
static unsigned last_field;
static unsigned field_nr;

enum QueryBufMode {
	Unqueued,
	Prepared,
	Queued,
	Dequeued
};

static std::string num2s(unsigned num)
{
	char buf[10];

	sprintf(buf, "%08x", num);
	return buf;
}

static std::string field2s(unsigned val)
{
	switch (val) {
	case V4L2_FIELD_ANY:
		return "Any";
	case V4L2_FIELD_NONE:
		return "None";
	case V4L2_FIELD_TOP:
		return "Top";
	case V4L2_FIELD_BOTTOM:
		return "Bottom";
	case V4L2_FIELD_INTERLACED:
		return "Interlaced";
	case V4L2_FIELD_SEQ_TB:
		return "Sequential Top-Bottom";
	case V4L2_FIELD_SEQ_BT:
		return "Sequential Bottom-Top";
	case V4L2_FIELD_ALTERNATE:
		return "Alternating";
	case V4L2_FIELD_INTERLACED_TB:
		return "Interlaced Top-Bottom";
	case V4L2_FIELD_INTERLACED_BT:
		return "Interlaced Bottom-Top";
	default:
		return "Unknown (" + num2s(val) + ")";
	}
}

static int checkQueryBuf(struct node *node, const struct v4l2_buffer &buf,
		__u32 type, __u32 memory, unsigned index, enum QueryBufMode mode,
		unsigned count)
{
	unsigned timestamp = buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
	unsigned frame_types = 0;
	unsigned buf_states = 0;

	fail_on_test(buf.type != type);
	fail_on_test(buf.memory == 0);
	fail_on_test(buf.memory != memory);
	fail_on_test(buf.index >= VIDEO_MAX_FRAME);
	fail_on_test(buf.index != index);
	fail_on_test(buf.reserved2 || buf.reserved);
	fail_on_test(timestamp != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
		     timestamp != V4L2_BUF_FLAG_TIMESTAMP_COPY);
	if (!node->is_m2m)
		fail_on_test(timestamp == V4L2_BUF_FLAG_TIMESTAMP_COPY);
	if (buf.flags & V4L2_BUF_FLAG_KEYFRAME)
		frame_types++;
	if (buf.flags & V4L2_BUF_FLAG_PFRAME)
		frame_types++;
	if (buf.flags & V4L2_BUF_FLAG_BFRAME)
		frame_types++;
	fail_on_test(frame_types > 1);
	fail_on_test(buf.flags & (V4L2_BUF_FLAG_NO_CACHE_INVALIDATE |
				  V4L2_BUF_FLAG_NO_CACHE_CLEAN));
	if (buf.flags & V4L2_BUF_FLAG_QUEUED)
		buf_states++;
	if (buf.flags & V4L2_BUF_FLAG_DONE)
		buf_states++;
	if (buf.flags & V4L2_BUF_FLAG_ERROR)
		buf_states++;
	if (buf.flags & V4L2_BUF_FLAG_PREPARED)
		buf_states++;
	fail_on_test(buf_states > 1);
	fail_on_test(buf.length == 0);
	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		fail_on_test(buf.length > VIDEO_MAX_PLANES);
		for (unsigned p = 0; p < buf.length; p++) {
			struct v4l2_plane *vp = buf.m.planes + p;

			fail_on_test(check_0(vp->reserved, sizeof(vp->reserved)));
			fail_on_test(vp->length == 0);
		}
	}

	if (mode == Dequeued || mode == Prepared) {
		fail_on_test(!(buf.flags & (V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_ERROR)));
		if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
			fail_on_test(buf.length <= VIDEO_MAX_PLANES);
			for (unsigned p = 0; p < buf.length; p++) {
				struct v4l2_plane *vp = buf.m.planes + p;

				if (buf.memory == V4L2_MEMORY_USERPTR)
					fail_on_test((void *)vp->m.userptr != ptrs[buf.index]);
				fail_on_test(vp->data_offset + vp->bytesused > vp->length);
			}
		} else {
			if (buf.memory == V4L2_MEMORY_USERPTR)
				fail_on_test((void *)buf.m.userptr != ptrs[buf.index]);
		}
	}

	if (mode == Dequeued) {
		fail_on_test(!buf.bytesused);
		fail_on_test(buf.bytesused > buf.length);
		fail_on_test(!buf.timestamp.tv_sec && !buf.timestamp.tv_usec);
		fail_on_test(buf.field == V4L2_FIELD_ALTERNATE);
		fail_on_test(buf.field == V4L2_FIELD_ANY);
		if (cur_fmt.fmt.pix.field == V4L2_FIELD_ALTERNATE) {
			fail_on_test(buf.field != V4L2_FIELD_BOTTOM &&
				     buf.field != V4L2_FIELD_TOP);
			fail_on_test(buf.field == last_field);
			field_nr ^= 1;
			if (field_nr)
				fail_on_test((int)buf.sequence != last_seq);
			else
				fail_on_test((int)buf.sequence != last_seq + 1);
		} else {
			fail_on_test(buf.field != cur_fmt.fmt.pix.field);
			fail_on_test((int)buf.sequence != last_seq + 1);
		}
		last_seq = (int)buf.sequence;
		last_field = buf.field;

		if (buf.flags & V4L2_BUF_FLAG_TIMECODE)
			warn("V4L2_BUF_FLAG_TIMECODE was used!\n");
	} else {
		fail_on_test(buf.sequence);
		fail_on_test(buf.timestamp.tv_sec || buf.timestamp.tv_usec);
		fail_on_test(buf.flags & V4L2_BUF_FLAG_TIMECODE);
		fail_on_test(frame_types);
		if (mode == Unqueued)
			fail_on_test(buf.flags & (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_PREPARED |
						  V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_ERROR));
		else if (mode == Prepared)
			fail_on_test((buf.flags & (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_PREPARED |
						   V4L2_BUF_FLAG_DONE | V4L2_BUF_FLAG_ERROR)) !=
					V4L2_BUF_FLAG_PREPARED);
		else
			fail_on_test(!(buf.flags & (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_PREPARED)));
	}
	return 0;
}

static int testQueryBuf(struct node *node, unsigned type, unsigned count)
{
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	struct v4l2_buffer buf;
	int ret;
	unsigned i;

	memset(&buf, 0, sizeof(buf));
	buf.type = type;
	if (V4L2_TYPE_IS_MULTIPLANAR(type)) {
		buf.m.planes = planes;
		buf.length = VIDEO_MAX_PLANES;
	}
	for (i = 0; i < count; i++) {
		buf.index = i;
		fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
		if (V4L2_TYPE_IS_MULTIPLANAR(type))
			fail_on_test(buf.m.planes != planes);
		fail_on_test(checkQueryBuf(node, buf, type, buf.memory, i, Unqueued, 0));
	}
	buf.index = count;
	ret = doioctl(node, VIDIOC_QUERYBUF, &buf);
	fail_on_test(ret != EINVAL);
	return 0;
}


int testReqBufs(struct node *node)
{
	struct v4l2_requestbuffers bufs;
	struct v4l2_create_buffers cbufs;
	struct v4l2_format fmt;
	bool can_stream = node->caps & V4L2_CAP_STREAMING;
	bool can_rw = node->caps & V4L2_CAP_READWRITE;
	bool mmap_valid;
	bool userptr_valid;
	bool dmabuf_valid;
	int ret;
	unsigned i;
	
	reopen(node);
	memset(&bufs, 0, sizeof(bufs));
	memset(&cbufs, 0, sizeof(cbufs));
	ret = doioctl(node, VIDIOC_REQBUFS, &bufs);
	if (ret == ENOTTY) {
		fail_on_test(can_stream);
		return ret;
	}
	fail_on_test(ret != EINVAL);
	fail_on_test(node->node2 == NULL);
	for (i = 1; i <= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; i++) {
		bool is_overlay = i == V4L2_BUF_TYPE_VIDEO_OVERLAY ||
				  i == V4L2_BUF_TYPE_VIDEO_OUTPUT_OVERLAY;

		if (node->buftype_pixfmts[i].empty())
			continue;
		info("test buftype %d\n", i);
		memset(&bufs, 0, sizeof(bufs));
		memset(&cbufs, 0, sizeof(cbufs));
		if (node->valid_buftype == 0)
			node->valid_buftype = i;
		fmt.type = i;
		fail_on_test(doioctl(node, VIDIOC_G_FMT, &fmt));
		bufs.type = fmt.type;

		fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs) != EINVAL);
		bufs.memory = V4L2_MEMORY_MMAP;
		ret = doioctl(node, VIDIOC_REQBUFS, &bufs);
		fail_on_test(ret && ret != EINVAL);
		mmap_valid = !ret;

		bufs.memory = V4L2_MEMORY_USERPTR;
		ret = doioctl(node, VIDIOC_REQBUFS, &bufs);
		fail_on_test(ret && ret != EINVAL);
		userptr_valid = !ret;

		bufs.memory = V4L2_MEMORY_DMABUF;
		ret = doioctl(node, VIDIOC_REQBUFS, &bufs);
		fail_on_test(ret && ret != EINVAL);
		dmabuf_valid = !ret;
		fail_on_test((can_stream && !is_overlay) && !mmap_valid && !userptr_valid && !dmabuf_valid);
		fail_on_test((!can_stream || is_overlay) && (mmap_valid || userptr_valid || dmabuf_valid));
		if (!can_stream || is_overlay)
			continue;

		if (mmap_valid) {
			bufs.count = 1;
			bufs.memory = V4L2_MEMORY_MMAP;
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			fail_on_test(bufs.count == 0);
			fail_on_test(bufs.memory != V4L2_MEMORY_MMAP);
			fail_on_test(bufs.type != i);
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			fail_on_test(testQueryBuf(node, i, bufs.count));
		}

		if (userptr_valid) {
			bufs.count = 1;
			bufs.memory = V4L2_MEMORY_USERPTR;
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			fail_on_test(bufs.count == 0);
			fail_on_test(bufs.memory != V4L2_MEMORY_USERPTR);
			fail_on_test(bufs.type != i);
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			fail_on_test(testQueryBuf(node, i, bufs.count));
		}

		if (dmabuf_valid) {
			bufs.count = 1;
			bufs.memory = V4L2_MEMORY_DMABUF;
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			fail_on_test(bufs.count == 0);
			fail_on_test(bufs.memory != V4L2_MEMORY_DMABUF);
			fail_on_test(bufs.type != i);
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			fail_on_test(testQueryBuf(node, i, bufs.count));
		}

		if (can_rw) {
			char buf = 0;

			if (node->can_capture)
				ret = read(node->fd, &buf, 1);
			else
				ret = write(node->fd, &buf, 1);
			if (ret != -1)
				return fail("Expected -1, got %d\n", ret);
			if (errno != EBUSY)
				return fail("Expected EBUSY, got %d\n", errno);
		}
		if (!node->is_m2m) {
			bufs.count = 1;
			fail_on_test(doioctl(node->node2, VIDIOC_REQBUFS, &bufs) != EBUSY);
			bufs.count = 0;
			fail_on_test(doioctl(node->node2, VIDIOC_REQBUFS, &bufs) != EBUSY);
			fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
			bufs.count = 1;
			fail_on_test(doioctl(node->node2, VIDIOC_REQBUFS, &bufs));
			bufs.count = 0;
			fail_on_test(doioctl(node->node2, VIDIOC_REQBUFS, &bufs));
		}
		cbufs.format = fmt;
		cbufs.count = 1;
		cbufs.memory = bufs.memory;
		ret = doioctl(node, VIDIOC_CREATE_BUFS, &cbufs);
		if (ret == ENOTTY) {
			warn("VIDIOC_CREATE_BUFS not supported\n");
			continue;
		}
		fail_on_test(cbufs.count == 0);
		fail_on_test(cbufs.memory != bufs.memory);
		fail_on_test(cbufs.format.type != i);
		fail_on_test(testQueryBuf(node, i, cbufs.count));
		cbufs.count = 1;
		fail_on_test(doioctl(node, VIDIOC_CREATE_BUFS, &cbufs));
		if (!node->is_m2m) {
			bufs.count = 1;
			fail_on_test(doioctl(node->node2, VIDIOC_REQBUFS, &bufs) != EBUSY);
		}
		bufs.count = 0;
		fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
	}
	return 0;
}

int testReadWrite(struct node *node)
{
	bool can_rw = node->caps & V4L2_CAP_READWRITE;
	int fd_flags = fcntl(node->fd, F_GETFL);
	char buf = 0;
	int ret;

	fcntl(node->fd, F_SETFL, fd_flags | O_NONBLOCK);
	if (node->can_capture)
		ret = read(node->fd, &buf, 1);
	else
		ret = write(node->fd, &buf, 1);
	// Note: RDS can only return multiples of 3, so we accept
	// both 0 and 1 as return code.
	if (can_rw)
		fail_on_test((ret < 0 && errno != EAGAIN) || ret > 1);
	else
		fail_on_test(ret >= 0 || errno != EINVAL);
	if (!can_rw)
		return 0;

	reopen(node);
	fcntl(node->fd, F_SETFL, fd_flags | O_NONBLOCK);

	/* check that the close cleared the busy flag */
	if (node->can_capture)
		ret = read(node->fd, &buf, 1);
	else
		ret = write(node->fd, &buf, 1);
	fail_on_test((ret < 0 && errno != EAGAIN) || ret > 1);
	return 0;
}

static int setupMmap(struct node *node, struct v4l2_requestbuffers &bufs)
{
	for (unsigned i = 0; i < bufs.count; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		struct v4l2_buffer buf;
		int ret;

		memset(&buf, 0, sizeof(buf));
		buf.type = bufs.type;
		buf.memory = bufs.memory;
		buf.index = i;
		if (V4L2_TYPE_IS_MULTIPLANAR(bufs.type)) {
			buf.m.planes = planes;
			buf.length = VIDEO_MAX_PLANES;
		}
		fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
		fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, i, Unqueued, 0));
		ptrs[i] = test_mmap(NULL, buf.length,
				  PROT_READ | PROT_WRITE, MAP_SHARED, node->fd, buf.m.offset);

		fail_on_test(ptrs[i] == MAP_FAILED);

		ret = doioctl(node, VIDIOC_PREPARE_BUF, &buf);
		fail_on_test(ret && ret != ENOTTY);
		if (ret == 0) {
			fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
			fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, i, Prepared, 0));
		}

		fail_on_test(doioctl(node, VIDIOC_QBUF, &buf));
		fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
		fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, i, Queued, 0));
	}
	return 0;
}

static int releaseMmap(struct node *node, struct v4l2_requestbuffers &bufs)
{
	for (unsigned i = 0; i < bufs.count; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		struct v4l2_buffer buf;

		memset(&buf, 0, sizeof(buf));
		buf.type = bufs.type;
		buf.memory = bufs.memory;
		buf.index = i;
		if (V4L2_TYPE_IS_MULTIPLANAR(bufs.type)) {
			buf.m.planes = planes;
			buf.length = VIDEO_MAX_PLANES;
		}
		fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
		munmap(ptrs[i], buf.length);
	}
	return 0;
}

static int captureBufs(struct node *node, const struct v4l2_requestbuffers &bufs,
		       unsigned count, bool use_poll)
{
	int fd_flags = fcntl(node->fd, F_GETFL);
	struct v4l2_buffer buf;
	int ret;

	if (use_poll)
		fcntl(node->fd, F_SETFL, fd_flags | O_NONBLOCK);
	for (;;) {
		if (use_poll) {
			struct timeval tv = { 2, 0 };
			fd_set read_fds;

			FD_ZERO(&read_fds);
			FD_SET(node->fd, &read_fds);
			ret = select(node->fd + 1, &read_fds, NULL, NULL, &tv);
			fail_on_test(ret <= 0);
			fail_on_test(!FD_ISSET(node->fd, &read_fds));
		}

		buf.type = bufs.type;
		buf.memory = bufs.memory;

		ret = doioctl(node, VIDIOC_DQBUF, &buf);
		if (ret == EAGAIN)
			continue;
		if (show_info)
			printf("\t\tBuffer: %d Sequence: %d Field: %s Timestamp: %ld.%06lds\n",
				buf.index, buf.sequence, field2s(buf.field).c_str(),
				buf.timestamp.tv_sec, buf.timestamp.tv_usec);
		fail_on_test(ret);
		fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, buf.index, Dequeued, 100 - count));
		if (!show_info) {
			printf("\r\tFrame #%03d%s", 100 - count, use_poll ? " (polling)" : "");
			fflush(stdout);
		}
		fail_on_test(doioctl(node, VIDIOC_QBUF, &buf));
		if (--count == 0)
			break;
	}
	if (use_poll)
		fcntl(node->fd, F_SETFL, fd_flags);
	if (!show_info)
		printf("\r\t                       \r");
	return 0;
}

int testMmap(struct node *node)
{
	struct v4l2_requestbuffers bufs;
	struct v4l2_create_buffers cbufs;
	struct v4l2_input input;
	bool can_stream = node->caps & V4L2_CAP_STREAMING;
	bool have_createbufs = true;
	int ret;
	
	if (!(node->caps & V4L2_CAP_VIDEO_CAPTURE))
		return 0;

	memset(&input, 0, sizeof(input));
	doioctl(node, VIDIOC_G_INPUT, &input.index);
	doioctl(node, VIDIOC_ENUMINPUT, &input);

	if (input.capabilities & V4L2_IN_CAP_STD) {
		v4l2_std_id std;

		doioctl(node, VIDIOC_QUERYSTD, &std);
		if (std)
			doioctl(node, VIDIOC_S_STD, &std);
	}

	if (input.capabilities & V4L2_IN_CAP_DV_TIMINGS) {
		struct v4l2_dv_timings t;

		if (doioctl(node, VIDIOC_QUERY_DV_TIMINGS, &t) == 0)
			doioctl(node, VIDIOC_S_DV_TIMINGS, &t);
	}

	memset(&bufs, 0, sizeof(bufs));
	bufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufs.memory = V4L2_MEMORY_MMAP;
	ret = doioctl(node, VIDIOC_REQBUFS, &bufs);
	if (ret) {
		fail_on_test(can_stream);
		return ret;
	}

	fail_on_test(doioctl(node, VIDIOC_STREAMON, &bufs.type));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));

	cur_fmt.type = bufs.type;
	doioctl(node, VIDIOC_G_FMT, &cur_fmt);
	bufs.count = 1;
	fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));
	last_seq = -1;
	field_nr = 1;
	last_field = cur_fmt.fmt.pix.field;

	cbufs.format = cur_fmt;
	cbufs.format.fmt.pix.height /= 2;
	cbufs.format.fmt.pix.sizeimage /= 2;
	cbufs.count = 1;
	cbufs.memory = bufs.memory;
	ret = doioctl(node, VIDIOC_CREATE_BUFS, &cbufs);
	if (ret == ENOTTY)
		have_createbufs = false;
	else
		fail_on_test(ret != EINVAL);
	fail_on_test(testQueryBuf(node, cur_fmt.type, bufs.count));
	if (have_createbufs) {
		cbufs.format = cur_fmt;
		cbufs.format.fmt.pix.sizeimage *= 2;
		cbufs.count = 1;
		cbufs.memory = bufs.memory;
		fail_on_test(doioctl(node, VIDIOC_CREATE_BUFS, &cbufs));
	}
	fail_on_test(setupMmap(node, bufs));

	fail_on_test(doioctl(node, VIDIOC_STREAMON, &bufs.type));
	fail_on_test(doioctl(node, VIDIOC_STREAMON, &bufs.type));
	fail_on_test(captureBufs(node, bufs, 100, false));
	fail_on_test(captureBufs(node, bufs, 100, true));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));
	fail_on_test(releaseMmap(node, bufs));
	bufs.count = 0;
	fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
	return 0;
}

static int setupUserPtr(struct node *node, struct v4l2_requestbuffers &bufs)
{
	for (unsigned i = 0; i < bufs.count; i++) {
		struct v4l2_plane planes[VIDEO_MAX_PLANES];
		struct v4l2_buffer buf;
		int ret;

		memset(&buf, 0, sizeof(buf));
		buf.type = bufs.type;
		buf.memory = bufs.memory;
		buf.index = i;
		if (V4L2_TYPE_IS_MULTIPLANAR(bufs.type)) {
			buf.m.planes = planes;
			buf.length = VIDEO_MAX_PLANES;
		}
		fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
		fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, i, Unqueued, 0));
		ptrs[i] = malloc(buf.length);
		fail_on_test(ptrs[i] == NULL);
		buf.m.userptr = (unsigned long)ptrs[i];

		ret = doioctl(node, VIDIOC_PREPARE_BUF, &buf);
		fail_on_test(ret && ret != ENOTTY);
		if (ret == 0) {
			fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
			fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, i, Prepared, 0));
		}

		fail_on_test(doioctl(node, VIDIOC_QBUF, &buf));
		fail_on_test(doioctl(node, VIDIOC_QUERYBUF, &buf));
		fail_on_test(checkQueryBuf(node, buf, bufs.type, bufs.memory, i, Queued, 0));
	}
	return 0;
}

static int releaseUserPtr(struct node *node, struct v4l2_requestbuffers &bufs)
{
	for (unsigned i = 0; i < bufs.count; i++)
		free(ptrs[i]);
	return 0;
}

int testUserPtr(struct node *node)
{
	struct v4l2_requestbuffers bufs;
	bool can_stream = node->caps & V4L2_CAP_STREAMING;
	int ret;
	
	if (!(node->caps & V4L2_CAP_VIDEO_CAPTURE))
		return 0;

	memset(&bufs, 0, sizeof(bufs));
	bufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	bufs.memory = V4L2_MEMORY_USERPTR;
	ret = doioctl(node, VIDIOC_REQBUFS, &bufs);
	if (ret) {
		fail_on_test(can_stream);
		return ret;
	}

	bufs.count = 1;
	fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));
	last_seq = -1;
	field_nr = 1;
	last_field = cur_fmt.fmt.pix.field;

	fail_on_test(setupUserPtr(node, bufs));

	fail_on_test(doioctl(node, VIDIOC_STREAMON, &bufs.type));
	fail_on_test(doioctl(node, VIDIOC_STREAMON, &bufs.type));
	fail_on_test(captureBufs(node, bufs, 100, false));
	fail_on_test(captureBufs(node, bufs, 100, true));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));
	fail_on_test(doioctl(node, VIDIOC_STREAMOFF, &bufs.type));
	fail_on_test(releaseUserPtr(node, bufs));
	bufs.count = 0;
	fail_on_test(doioctl(node, VIDIOC_REQBUFS, &bufs));
	return 0;
}
