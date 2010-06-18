/*
 * Copyright 2009-2010 Freescale Semiconductor, Inc. All Rights Reserved.
 *
 */

/*
 * The code contained herein is licensed under the GNU Lesser General
 * Public License.  You may obtain a copy of the GNU Lesser General
 * Public License Version 2.1 or later at the following locations:
 *
 * http://www.opensource.org/licenses/lgpl-license.html
 * http://www.gnu.org/copyleft/lgpl.html
 */

/*!
 * @file mxc_ipu_hl_lib.c
 *
 * @brief IPU high level library implementation
 *
 * @ingroup IPU
 */

#ifdef __cplusplus
extern "C"{
#endif

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <malloc.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev.h>
#include "mxc_ipu_hl_lib.h"

#define FBDEV0	"/dev/fb0"
#define FBDEV1	"/dev/fb1"
#define FBDEV2	"/dev/fb2"

#define DBG_DEBUG		3
#define DBG_INFO		2
#define DBG_WARNING		1
#define DBG_ERR			0

static int debug_level = DBG_ERR;
#define dbg(flag, fmt, args...)	{ if(flag <= debug_level)  printf("%s:%d "fmt, __FILE__, __LINE__,##args); }

/* this mutex only can protect within same process context,
 * for different process, pls add other mutex*/
pthread_mutex_t prp_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t pp_mutex = PTHREAD_MUTEX_INITIALIZER;
int g_task_in_use = 0;

int _ipu_task_enable(ipu_lib_handle_t * ipu_handle);
void _ipu_task_disable(ipu_lib_handle_t * ipu_handle);
int ipu_get_interrupt_event(ipu_event_info *ev);
int _ipu_wait_for_irq(int irq, int ms);

enum {
        IC_ENC = 0x1,
        IC_VF = 0x2,
        IC_PP = 0x4,
        ROT_ENC = 0x8,
        ROT_VF = 0x10,
        ROT_PP = 0x20,
};

enum {
	LEFT_STRIPE,
	RIGHT_STRIPE,
};

typedef enum {
	NULL_MODE = 0,
	IC_MODE = 0x1,
	ROT_MODE = 0x2,
	COPY_MODE = 0x4,
} task_mode_t;

typedef enum {
	RGB_CS,
	YUV_CS,
	NULL_CS
} cs_t;

typedef struct {
        int fd_ipu;
        int mode;
	int enabled;

        int irq;
	int output_bufnum;
	int update_bufnum;
	int tri_output_bufnum;
        ipu_mem_info i_minfo[2];
        ipu_mem_info ov_minfo[2];
	ipu_mem_info ov_alpha_minfo[2];

	/* org input param */
	int input_width;
	int input_height;
	int input_fmt;
	int input_stride;
	/* input param after cropping */
	int iwidth;
	int iheight;
	int i_off;
	int i_uoff;
	int i_voff;

	/* overlay param */
	int overlay_en;
	int overlay_local_alpha_en;
	int ovwidth;
	int ovheight;
	int ov_off;
	int ov_uoff;
	int ov_voff;
	int ov_alpha_off;

	int split_mode;
	struct stripe_param left_stripe;
	struct stripe_param right_stripe;

	int input_fr_cnt;
	int output_fr_cnt;

	struct {
		unsigned int task_mode;
		unsigned int ipu_task;
		ipu_channel_t ic_chan;
		ipu_channel_t rot_chan;
		ipu_channel_t begin_chan;
		ipu_channel_t end_chan;

		ipu_mem_info r_minfo[2];
		ipu_mem_info o_minfo[3];

		int show_to_fb;
		int fd_fb;
		int fb_stride;
		void * fb_mem;
		int screen_size;
		ipu_channel_t fb_chan;

		/* org output param */
		int output_width;
		int output_height;
		int output_fmt;
		int output_stride;

		/* output param after cropping */
		int owidth;
		int oheight;
		int o_off;
		int o_uoff;
		int o_voff;
	} output;
} ipu_lib_priv_handle_t;

static u32 fmt_to_bpp(u32 pixelformat)
{
	u32 bpp;

	switch (pixelformat)
	{
		case IPU_PIX_FMT_RGB565:
		/*interleaved 422*/
		case IPU_PIX_FMT_YUYV:
		case IPU_PIX_FMT_UYVY:
		/*non-interleaved 422*/
		case IPU_PIX_FMT_YUV422P:
		case IPU_PIX_FMT_YVU422P:
			bpp = 16;
			break;
		case IPU_PIX_FMT_BGR24:
		case IPU_PIX_FMT_RGB24:
		case IPU_PIX_FMT_YUV444:
			bpp = 24;
			break;
		case IPU_PIX_FMT_BGR32:
		case IPU_PIX_FMT_BGRA32:
		case IPU_PIX_FMT_RGB32:
		case IPU_PIX_FMT_RGBA32:
		case IPU_PIX_FMT_ABGR32:
			bpp = 32;
			break;
		/*non-interleaved 420*/
		case IPU_PIX_FMT_YUV420P:
		case IPU_PIX_FMT_YUV420P2:
		case IPU_PIX_FMT_NV12:
			bpp = 12;
			break;
		default:
			bpp = 8;
			break;
	}
	return bpp;
}

static cs_t colorspaceofpixel(int fmt)
{
	switch(fmt)
	{
		case IPU_PIX_FMT_RGB565:
		case IPU_PIX_FMT_BGR24:
		case IPU_PIX_FMT_RGB24:
		case IPU_PIX_FMT_BGRA32:
		case IPU_PIX_FMT_BGR32:
		case IPU_PIX_FMT_RGBA32:
		case IPU_PIX_FMT_RGB32:
		case IPU_PIX_FMT_ABGR32:
			return RGB_CS;
			break;
		case IPU_PIX_FMT_UYVY:
		case IPU_PIX_FMT_YUYV:
		case IPU_PIX_FMT_YUV420P2:
		case IPU_PIX_FMT_YUV420P:
		case IPU_PIX_FMT_YVU422P:
		case IPU_PIX_FMT_YUV422P:
		case IPU_PIX_FMT_YUV444:
		case IPU_PIX_FMT_NV12:
			return YUV_CS;
			break;
		default:
			return NULL_CS;
	}
}

static int need_csc(int ifmt, int ofmt)
{
	cs_t ics,ocs;

	ics = colorspaceofpixel(ifmt);
	ocs = colorspaceofpixel(ofmt);

	if((ics == NULL_CS) || (ocs == NULL_CS)){
		dbg(DBG_ERR, "Color Space not recognized!\n");
		return -1;
	}else if(ics != ocs)
		return 1;

	return 0;
}

static int get_system_rev(unsigned int * system_rev)
{
        FILE *fp;
        char buf[1024];
        int nread;
        char *tmp, *rev;
        int ret = -1;

        fp = fopen("/proc/cpuinfo", "r");
        if (fp == NULL) {
                dbg(DBG_ERR, "Open /proc/cpuinfo failed!\n");
                return ret;
        }

        nread = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        if ((nread == 0) || (nread == sizeof(buf))) {
                return ret;
        }

        buf[nread] = '\0';

        tmp = strstr(buf, "Revision");
        if (tmp != NULL) {
                rev = index(tmp, ':');
                if (rev != NULL) {
                        rev++;
                        *system_rev = strtoul(rev, NULL, 16);
                        ret = 0;
                }
        }

	dbg(DBG_INFO, "system_rev is 0x%x\n", ret);

        return ret;
}

static int _ipu_get_arch_rot_begin()
{
	unsigned int system_rev, arch;

	if (get_system_rev(&system_rev) < 0)
		return IPU_ROTATE_90_RIGHT;

	arch = system_rev & 0xff000;
	/* for mx37 */
	if (arch == 0x37000)
		return IPU_ROTATE_HORIZ_FLIP;
	else
		return IPU_ROTATE_90_RIGHT;
}

static int _ipu_get_arch_ic_out_max_width()
{
	unsigned int system_rev, arch;

	if (get_system_rev(&system_rev) < 0)
		return 1024;

	arch = system_rev & 0xff000;
	/* for ipuv3 */
	if ((arch == 0x37000) || (arch == 0x51000))
		return 1024;
	return 1024;
}

static int _ipu_task_busy_in_hw(int ipu_task)
{
	int ret = 0;

	if (ipu_task & IC_ENC)
		ret |= ipu_is_channel_busy(MEM_PRP_ENC_MEM);
	if (ipu_task & IC_VF)
		ret |= ipu_is_channel_busy(MEM_PRP_VF_MEM);
	if (ipu_task & IC_PP)
		ret |= ipu_is_channel_busy(MEM_PP_MEM);
	if (ipu_task & ROT_ENC)
		ret |= ipu_is_channel_busy(MEM_ROT_ENC_MEM);
	if (ipu_task & ROT_VF)
		ret |= ipu_is_channel_busy(MEM_ROT_VF_MEM);
	if (ipu_task & ROT_PP)
		ret |= ipu_is_channel_busy(MEM_ROT_PP_MEM);

	return ret;
}

static int _ipu_is_task_busy(int ipu_task)
{
	/* g_task_in_use is only useful in same process context*/
	if (g_task_in_use & ipu_task)
		return 1;
	/* IC_ENC and IC_VF can not be enabled together in different task*/
	if (((g_task_in_use & IC_ENC) && (ipu_task & IC_VF)) ||
		((g_task_in_use & IC_VF) && (ipu_task & IC_ENC)))
		return 1;
	/* we need to check low level HW busy status */
	if (_ipu_task_busy_in_hw(ipu_task))
		return 1;
	return 0;
}

static void _ipu_update_offset(unsigned int fmt, unsigned int width, unsigned int height,
				unsigned int pos_x, unsigned int pos_y,
				int * off, int * uoff, int * voff)
{
	/* NOTE: u v offset should based on start point of off*/
	switch (fmt) {
		case IPU_PIX_FMT_YUV420P2:
		case IPU_PIX_FMT_YUV420P:
			*off = pos_y * width + pos_x;
			*uoff = (width * (height - pos_y) - pos_x)
				+ ((width/2 * pos_y/2) + pos_x/2);
			*voff = *uoff + (width/2 * height/2);
			break;
		case IPU_PIX_FMT_YVU422P:
			*off = pos_y * width + pos_x;
			*voff = (width * (height - pos_y) - pos_x)
				+ ((width * pos_y)/2 + pos_x/2);
			*uoff = *voff + (width * height)/2;
			break;
		case IPU_PIX_FMT_YUV422P:
			*off = pos_y * width + pos_x;
			*uoff = (width * (height - pos_y) - pos_x)
				+ (width * pos_y)/2 + pos_x/2;
			*voff = *uoff + (width * height)/2;
			break;
		case IPU_PIX_FMT_NV12:
			*off = pos_y * width + pos_x;
			*uoff = (width * (height - pos_y) - pos_x)
				+ width * pos_y/2 + pos_x;
			break;
		default:
			*off = (pos_y * width + pos_x) * fmt_to_bpp(fmt)/8;
			break;
	}
}

int _ipu_split_mode_set_stripe(ipu_lib_priv_handle_t * ipu_priv_handle, dma_addr_t in_buf_paddr,
				dma_addr_t out_buf_paddr, int stripe, int select_buf)
{
	int i_off, o_off;
	int buf_idx, ret = 0;

	if (stripe == LEFT_STRIPE) {
		dbg(DBG_DEBUG, "split mode set buffer for left stripe!\n");
		buf_idx = 0;
		i_off = ipu_priv_handle->left_stripe.input_column;
		o_off = ipu_priv_handle->left_stripe.output_column;
	} else {
		dbg(DBG_DEBUG, "split mode set buffer for right stripe!\n");
		buf_idx = 1;
		i_off = ipu_priv_handle->right_stripe.input_column;
		o_off = ipu_priv_handle->right_stripe.output_column;
	}

	ret = ipu_update_channel_buffer(ipu_priv_handle->output.ic_chan,
			IPU_OUTPUT_BUFFER,
			buf_idx,
			out_buf_paddr + ipu_priv_handle->output.o_off +
			o_off);
	ret += ipu_update_channel_offset(ipu_priv_handle->output.ic_chan,
			IPU_OUTPUT_BUFFER,
			ipu_priv_handle->output.output_fmt,
			ipu_priv_handle->output.owidth,
			ipu_priv_handle->output.oheight,
			ipu_priv_handle->output.output_stride,
			ipu_priv_handle->output.o_uoff,
			ipu_priv_handle->output.o_voff,
			0,
			o_off);
	ret += ipu_update_channel_buffer(ipu_priv_handle->output.ic_chan,
			IPU_INPUT_BUFFER,
			buf_idx,
			in_buf_paddr + ipu_priv_handle->i_off + i_off);
	ret += ipu_update_channel_offset(ipu_priv_handle->output.ic_chan,
			IPU_INPUT_BUFFER,
			ipu_priv_handle->input_fmt,
			ipu_priv_handle->iwidth,
			ipu_priv_handle->iheight,
			ipu_priv_handle->input_stride,
			ipu_priv_handle->i_uoff,
			ipu_priv_handle->i_voff,
			0,
			i_off);
	if (ret < 0) {
		dbg(DBG_ERR, "_ipu_split_mode_set_stripe failed!\n");
		return ret;
	}

	if (select_buf) {
		ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_INPUT_BUFFER, buf_idx);
		ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_OUTPUT_BUFFER, buf_idx);
	}
	return ret;
}

static task_mode_t __ipu_task_check(ipu_lib_priv_handle_t * ipu_priv_handle,
		ipu_lib_input_param_t * input,
		ipu_lib_output_param_t * output)
{
	task_mode_t task_mode = NULL_MODE;
	int tmp;

	if(output->rot >= _ipu_get_arch_rot_begin()){
		if(output->rot >= IPU_ROTATE_90_RIGHT){
			/*output swap*/
			tmp = ipu_priv_handle->output.owidth;
			ipu_priv_handle->output.owidth =
				ipu_priv_handle->output.oheight;
			ipu_priv_handle->output.oheight = tmp;
		}
		task_mode |= ROT_MODE;
	}

	/* make sure width is 8 pixel align*/
	if (task_mode & ROT_MODE)
		ipu_priv_handle->output.oheight -= ipu_priv_handle->output.oheight%8;

	/*need resize or CSC?*/
	if((ipu_priv_handle->iwidth != ipu_priv_handle->output.owidth) ||
			(ipu_priv_handle->iheight != ipu_priv_handle->output.oheight) ||
			need_csc(input->fmt,output->fmt))
		task_mode |= IC_MODE;

	/*need flip?*/
	if((task_mode == NULL_MODE) && (output->rot > IPU_ROTATE_NONE ))
		task_mode |= IC_MODE;

	/*need IDMAC do format(same color space)?*/
	if((task_mode == NULL_MODE) && (input->fmt != output->fmt))
		task_mode |= IC_MODE;

	if(output->rot >= _ipu_get_arch_rot_begin()){
		if(output->rot >= IPU_ROTATE_90_RIGHT){
			/*output swap*/
			tmp = ipu_priv_handle->output.owidth;
			ipu_priv_handle->output.owidth =
				ipu_priv_handle->output.oheight;
			ipu_priv_handle->output.oheight = tmp;
		}
	}

	return task_mode;
}

static int _ipu_task_check(ipu_lib_input_param_t * input,
		ipu_lib_overlay_param_t * overlay,
		ipu_lib_output_param_t * output,
		ipu_lib_handle_t * ipu_handle)
{
	int ipu_task_busy = 0;
	int ret = 0, hope_task_mode;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	hope_task_mode = ipu_priv_handle->mode & 0x07;
	if (overlay && hope_task_mode) {
		if (!(hope_task_mode & (TASK_VF_MODE | TASK_PP_MODE))) {
			dbg(DBG_ERR, "Must use PP or VF task for overlay!\n");
			ret = -1;
			goto done;
		}
	}
	if (overlay && overlay->global_alpha_en && overlay->local_alpha_en) {
		dbg(DBG_ERR, "Choose overlay global or local alpha only!\n");
		ret = -1;
		goto done;
	}

	ipu_priv_handle->input_width = input->width;
	ipu_priv_handle->input_height = input->height;
	ipu_priv_handle->input_fmt = input->fmt;
	if ((input->input_crop_win.win_w > 0) || (input->input_crop_win.win_h > 0)) {
		if ((input->input_crop_win.win_w + input->input_crop_win.pos.x) > input->width)
			input->input_crop_win.win_w = input->width - input->input_crop_win.pos.x;
		if ((input->input_crop_win.win_h + input->input_crop_win.pos.y) > input->height)
			input->input_crop_win.win_h = input->height - input->input_crop_win.pos.y;
		ipu_priv_handle->iwidth = input->input_crop_win.win_w;
		ipu_priv_handle->iwidth -= ipu_priv_handle->iwidth%8;
		ipu_priv_handle->iheight = input->input_crop_win.win_h;
		ipu_priv_handle->iheight -= ipu_priv_handle->iheight%8;

		if ((ipu_priv_handle->iwidth != input->width) || (ipu_priv_handle->iheight != input->height)) {
			_ipu_update_offset(input->fmt, input->width, input->height,
						input->input_crop_win.pos.x, input->input_crop_win.pos.y,
						&ipu_priv_handle->i_off,
						&ipu_priv_handle->i_uoff,
						&ipu_priv_handle->i_voff);
		}
	} else {
		ipu_priv_handle->iwidth = input->width;
		ipu_priv_handle->iwidth -= ipu_priv_handle->iwidth%8;
		ipu_priv_handle->iheight = input->height;
	}

	if (overlay) {
		if ((overlay->ov_crop_win.win_w > 0) || (overlay->ov_crop_win.win_h > 0)) {
			if ((overlay->ov_crop_win.win_w + overlay->ov_crop_win.pos.x) > overlay->width)
				overlay->ov_crop_win.win_w = overlay->width - overlay->ov_crop_win.pos.x;
			if ((overlay->ov_crop_win.win_h + overlay->ov_crop_win.pos.y) > overlay->height)
				overlay->ov_crop_win.win_h = overlay->height - overlay->ov_crop_win.pos.y;
			ipu_priv_handle->ovwidth = overlay->ov_crop_win.win_w;
			ipu_priv_handle->ovwidth -= ipu_priv_handle->ovwidth%8;
			ipu_priv_handle->ovheight = overlay->ov_crop_win.win_h;
			ipu_priv_handle->ovheight -= ipu_priv_handle->ovheight%8;

			if ((ipu_priv_handle->ovwidth != overlay->width) || (ipu_priv_handle->ovheight != overlay->height)) {
				_ipu_update_offset(overlay->fmt, overlay->width, overlay->height,
						overlay->ov_crop_win.pos.x, overlay->ov_crop_win.pos.y,
						&ipu_priv_handle->ov_off,
						&ipu_priv_handle->ov_uoff,
						&ipu_priv_handle->ov_voff);

				if (overlay && overlay->local_alpha_en)
					ipu_priv_handle->ov_alpha_off = overlay->ov_crop_win.pos.y * overlay->width +
									overlay->ov_crop_win.pos.x;
			}
		} else {
			ipu_priv_handle->ovwidth = overlay->width;
			ipu_priv_handle->ovwidth -= ipu_priv_handle->ovwidth%8;
			ipu_priv_handle->ovheight = overlay->height;
		}
	}

	ipu_priv_handle->output.output_width = output->width;
	ipu_priv_handle->output.output_height = output->height;
	ipu_priv_handle->output.output_fmt = output->fmt;
	if ((output->output_win.win_w > 0) || (output->output_win.win_h > 0)) {
		if ((output->output_win.win_w + output->output_win.pos.x) > output->width)
			output->output_win.win_w = output->width - output->output_win.pos.x;
		if ((output->output_win.win_h + output->output_win.pos.y) > output->height)
			output->output_win.win_h = output->height - output->output_win.pos.y;
		ipu_priv_handle->output.owidth = output->output_win.win_w;
		ipu_priv_handle->output.owidth -= ipu_priv_handle->output.owidth % 8;
		ipu_priv_handle->output.oheight = output->output_win.win_h;
		if (output->show_to_fb)
			ipu_priv_handle->output.oheight -= ipu_priv_handle->output.oheight % 8;

		if ((ipu_priv_handle->output.owidth != output->width) ||
				(ipu_priv_handle->output.oheight != output->height)) {
			_ipu_update_offset(output->fmt, output->width, output->height,
					output->output_win.pos.x, output->output_win.pos.y,
					&ipu_priv_handle->output.o_off,
					&ipu_priv_handle->output.o_uoff,
					&ipu_priv_handle->output.o_voff);
		}
	} else {
		ipu_priv_handle->output.owidth = output->width;
		ipu_priv_handle->output.owidth -= ipu_priv_handle->output.owidth % 8;
		ipu_priv_handle->output.oheight = output->height;
		if (output->show_to_fb)
			ipu_priv_handle->output.oheight -= ipu_priv_handle->output.oheight % 8;
	}
	/* whether output size is too big, if so, enable split mode */
	if (ipu_priv_handle->output.owidth > _ipu_get_arch_ic_out_max_width())
		ipu_priv_handle->split_mode = 1;

	if (overlay) {
		if ((ipu_priv_handle->ovwidth != ipu_priv_handle->output.owidth) ||
				(ipu_priv_handle->ovheight != ipu_priv_handle->output.oheight)) {
			dbg(DBG_ERR, "width/height of overlay and output should be same!\n");
			ret = -1;
			goto done;
		}
	}

	ipu_priv_handle->output.task_mode = __ipu_task_check(ipu_priv_handle, input, output);

	if (overlay) {
		ipu_priv_handle->output.task_mode |= IC_MODE;
		ipu_priv_handle->overlay_en = 1;
		if (overlay->local_alpha_en)
			ipu_priv_handle->overlay_local_alpha_en = 1;
	}

	if (ipu_priv_handle->split_mode) {
		if (ipu_priv_handle->output.task_mode & ROT_MODE) {
			dbg(DBG_ERR, "Not support split mode with rotation!\n");
			ret = -1;
			goto done;
		}
		if (overlay) {
			dbg(DBG_ERR, "Not support split mode with overlay!\n");
			ret = -1;
			goto done;
		}
		ipu_calc_stripes_sizes(ipu_priv_handle->iwidth,
					ipu_priv_handle->output.owidth,
					_ipu_get_arch_ic_out_max_width(),
					(((unsigned long long)1) << 32), /* 32bit for fractional*/
					1, /* equal stripes */
					input->fmt,
					output->fmt,
					&ipu_priv_handle->left_stripe,
					&ipu_priv_handle->right_stripe);
	}

	if (ipu_priv_handle->output.task_mode == NULL_MODE) {
		ipu_priv_handle->output.task_mode = COPY_MODE;
		dbg(DBG_INFO, "Copy case!\n");
		goto done;
	}

	/* try ENC first */
	if (ipu_priv_handle->output.task_mode & ROT_MODE)
		ipu_priv_handle->output.ipu_task |= ROT_ENC;
	if (ipu_priv_handle->output.task_mode & IC_MODE)
		ipu_priv_handle->output.ipu_task |= IC_ENC;

	if (overlay || _ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
			(hope_task_mode && ((hope_task_mode & TASK_ENC_MODE) == 0))) {

		/* hope mode ENC task is busy ? */
		if (!overlay && hope_task_mode && (hope_task_mode & TASK_ENC_MODE)) {
			ipu_task_busy = 1;
			goto done;
		}

		/* try PP */
		ipu_priv_handle->output.ipu_task = 0;
		if (ipu_priv_handle->output.task_mode & ROT_MODE)
			ipu_priv_handle->output.ipu_task |= ROT_PP;
		if (ipu_priv_handle->output.task_mode & IC_MODE)
			ipu_priv_handle->output.ipu_task |= IC_PP;

		if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
				(hope_task_mode && ((hope_task_mode & TASK_PP_MODE) == 0))) {

			/* hope mode PP task is busy ? */
			if (hope_task_mode && (hope_task_mode & TASK_PP_MODE)) {
				ipu_task_busy = 1;
				goto done;
			}

			/* try VF */
			ipu_priv_handle->output.ipu_task = 0;
			if (ipu_priv_handle->output.task_mode & ROT_MODE)
				ipu_priv_handle->output.ipu_task |= ROT_VF;
			if (ipu_priv_handle->output.task_mode & IC_MODE)
				ipu_priv_handle->output.ipu_task |= IC_VF;

			/* left only VF task to try */
			if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
					(hope_task_mode && ((hope_task_mode & TASK_VF_MODE) == 0)))
				ipu_task_busy = 1;
		}
	}
done:
	if (ipu_task_busy) {
		ret = -1;
		dbg(DBG_ERR, "ipu is busy\n");
		if (hope_task_mode)
			dbg(DBG_ERR, " for hope task mode 0x%x!\n", hope_task_mode);
	} else if (ret == 0){
		unsigned int task = ipu_priv_handle->output.ipu_task;
		dbg(DBG_INFO, "\033[0;34mWill take ipu task\033[0m\n");
		if (task & IC_ENC)
			dbg(DBG_INFO, "\tIC_ENC\n");
		if (task & IC_VF)
			dbg(DBG_INFO, "\tIC_VF\n");
		if (task & IC_PP)
			dbg(DBG_INFO, "\tIC_PP\n");
		if (task & ROT_ENC)
			dbg(DBG_INFO, "\tROT_ENC\n");
		if (task & ROT_VF)
			dbg(DBG_INFO, "\tROT_VF\n");
		if (task & ROT_PP)
			dbg(DBG_INFO, "\tROT_PP\n");
	}
	return ret;
}

int fit_fb_setting(struct fb_var_screeninfo * var, int width,
	int height, int fmt, ipu_channel_t fb_chan, int bufs)
{
	if (fb_chan == MEM_BG_SYNC)
		return ((var->xres_virtual == var->xres) &&
			(var->yres_virtual == bufs*var->yres));

	if ((colorspaceofpixel(fmt) == YUV_CS) &&
			(var->nonstd != fmt))
		return 0;
	if ((colorspaceofpixel(fmt) == RGB_CS) &&
			(var->nonstd != 0) &&
			(var->nonstd != fmt))
		return 0;
	if (fb_chan == MEM_DC_SYNC)
		return ((var->xres_virtual == var->xres) &&
			(var->yres_virtual == bufs*var->yres));
	if (fb_chan == MEM_FG_SYNC) {
		return ((var->xres == width) &&
			(var->xres_virtual == width) &&
			(var->yres == height) &&
			(var->yres_virtual == bufs*height) &&
			(var->bits_per_pixel == fmt_to_bpp(fmt)));
	}

	return 1;
}

static int _ipu_mem_alloc(ipu_lib_input_param_t * input,
		ipu_lib_overlay_param_t * overlay,
		ipu_lib_output_param_t * output,
		ipu_lib_handle_t * ipu_handle)
{
	int i, ret = 0, bufcnt;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	if (ipu_priv_handle->mode & OP_STREAM_MODE)
		bufcnt = 2;
	else
		bufcnt = 1;

	ipu_priv_handle->output.show_to_fb = output->show_to_fb;

	for (i=0;i<bufcnt;i++) {
		/* user can choose other input phy addr*/
		if (input->user_def_paddr[i] == 0) {
			ipu_handle->ifr_size = ipu_priv_handle->i_minfo[i].size =
					input->width/8*input->height*fmt_to_bpp(input->fmt);
			if (ioctl(ipu_priv_handle->fd_ipu, IPU_ALOC_MEM, &(ipu_priv_handle->i_minfo[i])) < 0) {
				dbg(DBG_ERR, "Ioctl IPU_ALOC_MEM failed!\n");
				ret = -1;
				goto err;
			}
			/* mmap virtual addr for user*/
			ipu_handle->inbuf_start[i] = mmap (NULL, ipu_priv_handle->i_minfo[i].size,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					ipu_priv_handle->fd_ipu, ipu_priv_handle->i_minfo[i].paddr);
			if (ipu_handle->inbuf_start[i] == MAP_FAILED) {
				dbg(DBG_ERR, "mmap failed!\n");
				ret = -1;
				goto err;
			}
			dbg(DBG_INFO, "\033[0;35mAlocate %d dma mem [%d] for input, dma addr 0x%x, mmap to %p!\033[0m\n",
					ipu_handle->ifr_size, i, ipu_priv_handle->i_minfo[i].paddr, ipu_handle->inbuf_start[i]);
		} else {
			ipu_priv_handle->i_minfo[i].paddr = input->user_def_paddr[i];
			dbg(DBG_INFO, "\033[0;35mSet input dma mem [%d] addr 0x%x by user!\033[0m\n", i, input->user_def_paddr[i]);
		}

		if (overlay) {
			if (overlay->user_def_paddr[i] == 0) {
				ipu_handle->ovfr_size = ipu_priv_handle->ov_minfo[i].size =
					overlay->width/8*overlay->height*fmt_to_bpp(overlay->fmt);
				if (ioctl(ipu_priv_handle->fd_ipu, IPU_ALOC_MEM, &(ipu_priv_handle->ov_minfo[i])) < 0) {
					dbg(DBG_ERR, "Ioctl IPU_ALOC_MEM failed!\n");
					ret = -1;
					goto err;
				}
				/* mmap virtual addr for user*/
				ipu_handle->ovbuf_start[i] = mmap (NULL, ipu_priv_handle->ov_minfo[i].size,
						PROT_READ | PROT_WRITE, MAP_SHARED,
						ipu_priv_handle->fd_ipu, ipu_priv_handle->ov_minfo[i].paddr);
				if (ipu_handle->ovbuf_start[i] == MAP_FAILED) {
					dbg(DBG_ERR, "mmap failed!\n");
					ret = -1;
					goto err;
				}
				dbg(DBG_INFO, "\033[0;35mAlocate %d dma mem [%d] for overlay, dma addr 0x%x, mmap to %p!\033[0m\n",
						ipu_handle->ovfr_size, i, ipu_priv_handle->ov_minfo[i].paddr, ipu_handle->ovbuf_start[i]);
			} else {
				ipu_priv_handle->ov_minfo[i].paddr = overlay->user_def_paddr[i];
				dbg(DBG_INFO, "\033[0;35mSet overlay dma mem [%d] addr 0x%x by user!\033[0m\n", i, overlay->user_def_paddr[i]);
			}

			if (overlay->local_alpha_en) {
				if (overlay->user_def_alpha_paddr[i] == 0) {
					ipu_handle->ovfr_alpha_size = ipu_priv_handle->ov_alpha_minfo[i].size =
						overlay->width * overlay->height;
					if (ioctl(ipu_priv_handle->fd_ipu, IPU_ALOC_MEM, &(ipu_priv_handle->ov_alpha_minfo[i])) < 0) {
						dbg(DBG_ERR, "Ioctl IPU_ALOC_MEM failed!\n");
						ret = -1;
						goto err;
					}
					/* mmap virtual addr for user*/
					ipu_handle->ovbuf_alpha_start[i] = mmap (NULL, ipu_priv_handle->ov_alpha_minfo[i].size,
							PROT_READ | PROT_WRITE, MAP_SHARED,
							ipu_priv_handle->fd_ipu, ipu_priv_handle->ov_alpha_minfo[i].paddr);
					if (ipu_handle->ovbuf_alpha_start[i] == MAP_FAILED) {
						dbg(DBG_ERR, "mmap failed!\n");
						ret = -1;
						goto err;
					}
					dbg(DBG_INFO,
						    "\033[0;35mAlocate %d dma mem [%d] for overlay local alpha blending, dma addr 0x%x, mmap to %p!\033[0m\n",
							ipu_handle->ovfr_alpha_size, i, ipu_priv_handle->ov_alpha_minfo[i].paddr,
							ipu_handle->ovbuf_alpha_start[i]);
				} else {
					ipu_priv_handle->ov_alpha_minfo[i].paddr = overlay->user_def_alpha_paddr[i];
					dbg(DBG_INFO, "\033[0;35mSet overlay local alpha blending dma mem [%d] addr 0x%x by user!\033[0m\n", i, overlay->user_def_alpha_paddr[i]);
				}
			}
		}

		/* allocate dma buffer for rotation? */
		if(ipu_priv_handle->output.task_mode == (ROT_MODE | IC_MODE)) {
			ipu_priv_handle->output.r_minfo[i].size =
				ipu_priv_handle->output.owidth/8*ipu_priv_handle->output.oheight
				*fmt_to_bpp(output->fmt);
			if (ioctl(ipu_priv_handle->fd_ipu, IPU_ALOC_MEM,
						&(ipu_priv_handle->output.r_minfo[i])) < 0) {
				dbg(DBG_ERR, "Ioctl IPU_ALOC_MEM failed!\n");
				ret = -1;
				goto err;
			}
			dbg(DBG_INFO, "\033[0;35mAlocate %d dma mem [%d] for rotation, dma addr 0x%x!\033[0m\n",
					ipu_priv_handle->output.r_minfo[i].size, i,
					ipu_priv_handle->output.r_minfo[i].paddr);
		}

again:
		/* user can choose other output phy addr*/
		if ((output->show_to_fb == 0) && (output->user_def_paddr[i] == 0)) {
			ipu_handle->ofr_size = ipu_priv_handle->output.o_minfo[i].size =
				output->width/8*output->height*fmt_to_bpp(output->fmt);
			if (ioctl(ipu_priv_handle->fd_ipu, IPU_ALOC_MEM,
						&(ipu_priv_handle->output.o_minfo[i])) < 0) {
				dbg(DBG_ERR, "Ioctl IPU_ALOC_MEM failed!\n");
				ret = -1;
				goto err;
			}
			/* mmap virtual addr for user*/
			ipu_handle->outbuf_start[i] = mmap (NULL, ipu_priv_handle->output.o_minfo[i].size,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					ipu_priv_handle->fd_ipu, ipu_priv_handle->output.o_minfo[i].paddr);
			if (ipu_handle->outbuf_start[i] == MAP_FAILED) {
				dbg(DBG_ERR, "mmap failed!\n");
				ret = -1;
				goto err;
			}
			dbg(DBG_INFO, "\033[0;35mAlocate %d dma mem [%d] for output, dma addr 0x%x, mmap to %p!\033[0m\n",
					ipu_handle->ofr_size, i, ipu_priv_handle->output.o_minfo[i].paddr,
					ipu_handle->outbuf_start[i]);
		} else if (output->user_def_paddr[i] != 0) {
			ipu_priv_handle->output.o_minfo[i].paddr = output->user_def_paddr[i];
			dbg(DBG_INFO, "\033[0;35mSet output dma mem [%d] addr 0x%x by user!\033[0m\n",
					i, output->user_def_paddr[i]);
		}
		/* allocate 3rd buf for output */
		if(i == 1) {
			i = 2;
			goto again;
		}
	}

	/*for the case output direct to framebuffer*/
	if (output->show_to_fb) {
		int owidth, oheight;
		struct fb_fix_screeninfo fb_fix;
		struct fb_var_screeninfo fb_var;
		int offset = 0;
		int blank;
		int fbbufs;
		char *fbdev;

		if (output->fb_disp.fb_num == 0)
			fbdev = FBDEV0;
		else if (output->fb_disp.fb_num == 1)
			fbdev = FBDEV1;
		else
			fbdev = FBDEV2;

		dbg(DBG_INFO, "Output Show to %s\n", fbdev);

		if ((ipu_priv_handle->output.fd_fb = open(fbdev, O_RDWR, 0)) < 0) {
			dbg(DBG_ERR, "Unable to open %s\n", fbdev);
			ret = -1;
			goto err;
		}

		if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOGET_FSCREENINFO, &fb_fix) < 0) {
			dbg(DBG_ERR, "Get FB fix info failed!\n");
			close(ipu_priv_handle->output.fd_fb);
			ret = -1;
			goto err;
		}
		if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOGET_VSCREENINFO, &fb_var) < 0) {
			dbg(DBG_ERR, "Get FB var info failed!\n");
			close(ipu_priv_handle->output.fd_fb);
			ret = -1;
			goto err;
		}

		if (ioctl(ipu_priv_handle->output.fd_fb, MXCFB_GET_FB_IPU_CHAN,
					&ipu_priv_handle->output.fb_chan) < 0) {
			dbg(DBG_WARNING,"Get FB ipu channel failed, use default\n");
			if (output->fb_disp.fb_num == 0)
				ipu_priv_handle->output.fb_chan = MEM_BG_SYNC;
			else if (output->fb_disp.fb_num == 1)
				ipu_priv_handle->output.fb_chan = MEM_DC_SYNC;
			else
				ipu_priv_handle->output.fb_chan = MEM_FG_SYNC;
		}

		owidth = ipu_priv_handle->output.owidth;
		oheight = ipu_priv_handle->output.oheight;

		fbbufs = 3;
		if (!fit_fb_setting(&fb_var, owidth, oheight, output->fmt,
					ipu_priv_handle->output.fb_chan, fbbufs)) {
			dbg(DBG_INFO,"reconfig fb setting\n");
			if (ipu_priv_handle->output.fb_chan == MEM_FG_SYNC) {
				fb_var.xres = owidth;
				fb_var.xres_virtual = fb_var.xres;
				fb_var.yres = oheight;
				fb_var.yres_virtual = fb_var.yres * fbbufs;
				fb_var.activate |= FB_ACTIVATE_FORCE;
				fb_var.nonstd = output->fmt;
				fb_var.bits_per_pixel = fmt_to_bpp(output->fmt);
			} else if (ipu_priv_handle->output.fb_chan == MEM_DC_SYNC) {
				fb_var.xres_virtual = fb_var.xres;
				fb_var.yres_virtual = fb_var.yres * fbbufs;
				fb_var.activate |= FB_ACTIVATE_FORCE;
				fb_var.nonstd = output->fmt;
				fb_var.bits_per_pixel = fmt_to_bpp(output->fmt);
			} else {
				fb_var.xres_virtual = fb_var.xres;
				fb_var.yres_virtual = fb_var.yres * fbbufs;
			}

			if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOPUT_VSCREENINFO, &fb_var) < 0) {
				dbg(DBG_ERR, "Set FB var info failed!\n");
				close(ipu_priv_handle->output.fd_fb);
				ret = -1;
				goto err;
			}

			if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOGET_FSCREENINFO, &fb_fix) < 0) {
				dbg(DBG_ERR, "Get FB fix info failed!\n");
				close(ipu_priv_handle->output.fd_fb);
				ret = -1;
				goto err;
			}

			if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOGET_VSCREENINFO, &fb_var) < 0) {
				dbg(DBG_ERR, "Get FB var info failed!\n");
				close(ipu_priv_handle->output.fd_fb);
				ret = -1;
				goto err;
			}
		}

		if (ipu_priv_handle->output.fb_chan == MEM_FG_SYNC) {
			if ( ioctl(ipu_priv_handle->output.fd_fb, MXCFB_SET_OVERLAY_POS,
						&(output->fb_disp.pos)) < 0)
				dbg(DBG_ERR, "Set FB position failed!\n");
		}

		dbg(DBG_INFO, "fb xres %d\n", fb_var.xres);
		dbg(DBG_INFO, "fb yres %d\n", fb_var.yres);
		dbg(DBG_INFO, "fb xres_virtual %d\n", fb_var.xres_virtual);
		dbg(DBG_INFO, "fb yres_virtual %d\n", fb_var.yres_virtual);

		if ((owidth > fb_var.xres) || (oheight > fb_var.yres)
				|| (fmt_to_bpp(output->fmt) != fb_var.bits_per_pixel)) {
			dbg(DBG_ERR, "Output image is not fit for %s!\n", fbdev);
			close(ipu_priv_handle->output.fd_fb);
			ret = -1;
			goto err;
		}

		ipu_priv_handle->output.fb_stride = fb_var.xres * fb_var.bits_per_pixel/8;

		if (ipu_priv_handle->output.fb_chan != MEM_FG_SYNC)
			offset = output->fb_disp.pos.y * ipu_priv_handle->output.fb_stride
				+ output->fb_disp.pos.x * fb_var.bits_per_pixel/8;

		ipu_priv_handle->output.screen_size = fb_var.yres * fb_fix.line_length;

		ipu_priv_handle->output.o_minfo[0].paddr = fb_fix.smem_start +
			ipu_priv_handle->output.screen_size + offset;
		ipu_priv_handle->output.o_minfo[1].paddr = fb_fix.smem_start + offset;
		ipu_priv_handle->output.o_minfo[2].paddr = fb_fix.smem_start
			+ 2*ipu_priv_handle->output.screen_size + offset;

		ipu_priv_handle->output.fb_mem = mmap(0,
				fbbufs*ipu_priv_handle->output.screen_size,
				PROT_READ | PROT_WRITE, MAP_SHARED,
				ipu_priv_handle->output.fd_fb, 0);
		if (ipu_priv_handle->output.fb_mem == MAP_FAILED) {
			dbg(DBG_ERR, "mmap failed!\n");
			close(ipu_priv_handle->output.fd_fb);
			ret = -1;
			goto err;
		}

		if ((ipu_priv_handle->output.fb_chan != MEM_FG_SYNC) &&
				(bufcnt > 1) && ((owidth < fb_var.xres) || (oheight < fb_var.yres))) {
			/*make two buffer be the same to avoid flick*/
			memcpy(ipu_priv_handle->output.fb_mem +
					ipu_priv_handle->output.screen_size,
					ipu_priv_handle->output.fb_mem,
					ipu_priv_handle->output.screen_size);
			memcpy(ipu_priv_handle->output.fb_mem +
					2*ipu_priv_handle->output.screen_size,
					ipu_priv_handle->output.fb_mem,
					ipu_priv_handle->output.screen_size);
		}

		dbg(DBG_INFO, "fb stride %d\n", ipu_priv_handle->output.fb_stride);
		dbg(DBG_INFO, "fb screen_size %d\n", ipu_priv_handle->output.screen_size);
		dbg(DBG_INFO, "fb phyaddr0 0x%x\n", ipu_priv_handle->output.o_minfo[0].paddr);
		dbg(DBG_INFO, "fb phyaddr1 0x%x\n", ipu_priv_handle->output.o_minfo[1].paddr);
		dbg(DBG_INFO, "fb phyaddr2 0x%x\n", ipu_priv_handle->output.o_minfo[2].paddr);

		blank = FB_BLANK_UNBLANK;
		if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOBLANK, blank) < 0) {
			dbg(DBG_ERR, "UNBLANK FB failed!\n");
		}
	}
err:
	return ret;
}

static void _ipu_mem_free(ipu_lib_handle_t * ipu_handle)
{
	int i, bufcnt;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	if (ipu_priv_handle->mode & OP_STREAM_MODE)
		bufcnt = 2;
	else
		bufcnt = 1;

	for (i=0;i<bufcnt;i++) {
		if (ipu_priv_handle->i_minfo[i].vaddr) {
			if (ipu_handle->inbuf_start[i])
				munmap(ipu_handle->inbuf_start[i], ipu_priv_handle->i_minfo[i].size);
			ioctl(ipu_priv_handle->fd_ipu, IPU_FREE_MEM, &(ipu_priv_handle->i_minfo[i]));
			dbg(DBG_INFO, "\033[0;35mFree %d dma mem [%d] for input, dma addr 0x%x!\033[0m\n",
					ipu_handle->ifr_size, i, ipu_priv_handle->i_minfo[i].paddr);
		}
		if (ipu_priv_handle->ov_minfo[i].vaddr) {
			if (ipu_handle->ovbuf_start[i])
				munmap(ipu_handle->ovbuf_start[i], ipu_priv_handle->ov_minfo[i].size);
			ioctl(ipu_priv_handle->fd_ipu, IPU_FREE_MEM, &(ipu_priv_handle->ov_minfo[i]));
			dbg(DBG_INFO, "\033[0;35mFree %d dma mem [%d] for overlay, dma addr 0x%x!\033[0m\n",
					ipu_handle->ovfr_size, i, ipu_priv_handle->ov_minfo[i].paddr);
		}
		if (ipu_priv_handle->ov_alpha_minfo[i].vaddr) {
			if (ipu_handle->ovbuf_alpha_start[i])
				munmap(ipu_handle->ovbuf_alpha_start[i], ipu_priv_handle->ov_alpha_minfo[i].size);
			ioctl(ipu_priv_handle->fd_ipu, IPU_FREE_MEM, &(ipu_priv_handle->ov_alpha_minfo[i]));
			dbg(DBG_INFO, "\033[0;35mFree %d dma mem [%d] for overlay local alpha blending, dma addr 0x%x!\033[0m\n",
					ipu_handle->ovfr_alpha_size, i, ipu_priv_handle->ov_alpha_minfo[i].paddr);
		}

		if (ipu_priv_handle->output.r_minfo[i].vaddr) {
			ioctl(ipu_priv_handle->fd_ipu, IPU_FREE_MEM, &(ipu_priv_handle->output.r_minfo[i]));
			dbg(DBG_INFO, "\033[0;35mFree %d dma mem [%d] for rotation, dma addr 0x%x!\033[0m\n",
					ipu_priv_handle->output.r_minfo[i].size, i,
					ipu_priv_handle->output.r_minfo[i].paddr);
		}

again:
		if (ipu_priv_handle->output.show_to_fb == 0) {
			if (ipu_priv_handle->output.o_minfo[i].vaddr) {
				if (ipu_handle->outbuf_start[i])
					munmap(ipu_handle->outbuf_start[i],
							ipu_priv_handle->output.o_minfo[i].size);
				ioctl(ipu_priv_handle->fd_ipu, IPU_FREE_MEM,
						&(ipu_priv_handle->output.o_minfo[i]));

				dbg(DBG_INFO, "\033[0;35mFree %d dma mem [%d] for output, dma addr 0x%x!\033[0m\n",
						ipu_handle->ofr_size, i, ipu_priv_handle->output.o_minfo[i].paddr);
			}
		}
		if (i == 1) {
			i = 2;
			goto again;
		}
	}

	if (ipu_priv_handle->output.show_to_fb){
		struct fb_var_screeninfo fb_var;

		/* make sure buffer1 still at fbmem base*/
		memcpy(ipu_priv_handle->output.fb_mem,
				ipu_priv_handle->output.fb_mem +
				ipu_priv_handle->output.screen_size,
				ipu_priv_handle->output.screen_size);

		ioctl(ipu_priv_handle->output.fd_fb, FBIOGET_VSCREENINFO, &fb_var);
		fb_var.activate |= FB_ACTIVATE_FORCE;
		ioctl(ipu_priv_handle->output.fd_fb, FBIOPUT_VSCREENINFO, &fb_var);

		if (ipu_priv_handle->output.fb_mem)
			munmap(ipu_priv_handle->output.fb_mem, 3*ipu_priv_handle->output.screen_size);

		if (ipu_priv_handle->output.fb_chan == MEM_FG_SYNC) {
			int blank = FB_BLANK_POWERDOWN;
			if ( ioctl(ipu_priv_handle->output.fd_fb, FBIOBLANK, blank) < 0) {
				dbg(DBG_ERR, "POWERDOWN FB failed!\n");
			}
		}

		close(ipu_priv_handle->output.fd_fb);
	}
}

static int _ipu_channel_setup(ipu_lib_input_param_t * input,
		ipu_lib_overlay_param_t * overlay,
		ipu_lib_output_param_t * output,
		ipu_lib_handle_t * ipu_handle)
{
	ipu_channel_params_t params;
	int tmp, ret = 0;
	unsigned int task_mode;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	dbg(DBG_INFO, "\033[0;34mmode:\033[0m\n");
	if (ipu_priv_handle->mode & TASK_ENC_MODE)
		dbg(DBG_INFO, "\tTASK_ENC_MODE\n");
	if (ipu_priv_handle->mode & TASK_VF_MODE)
		dbg(DBG_INFO, "\tTASK_VF_MODE\n");
	if (ipu_priv_handle->mode & TASK_PP_MODE)
		dbg(DBG_INFO, "\tTASK_PP_MODE\n");
	if (ipu_priv_handle->mode & OP_NORMAL_MODE)
		dbg(DBG_INFO, "\tOP_NORMAL_MODE\n");
	if (ipu_priv_handle->mode & OP_STREAM_MODE)
		dbg(DBG_INFO, "\tOP_STREAM_MODE\n");

	dbg(DBG_INFO, "\033[0;34minput info:\033[0m\n");
	dbg(DBG_INFO, "\tw: %d\n", input->width);
	dbg(DBG_INFO, "\th: %d\n", input->height);
	dbg(DBG_INFO, "\tfmt: 0x%x\n", input->fmt);
	dbg(DBG_INFO, "\t\tw_posx: %d\n", input->input_crop_win.pos.x);
	dbg(DBG_INFO, "\t\tw_posy: %d\n", input->input_crop_win.pos.y);
	dbg(DBG_INFO, "\t\tw_w: %d\n", input->input_crop_win.win_w);
	dbg(DBG_INFO, "\t\tw_h: %d\n", input->input_crop_win.win_h);

	dbg(DBG_INFO, "\t\033[0;34minput crop:\033[0m\n");
	dbg(DBG_INFO, "\t\tiwidth: %d\n", ipu_priv_handle->iwidth);
	dbg(DBG_INFO, "\t\tiheight: %d\n", ipu_priv_handle->iheight);
	dbg(DBG_INFO, "\t\ti_off 0x%x\n", ipu_priv_handle->i_off);
	dbg(DBG_INFO, "\t\ti_uoff 0x%x\n", ipu_priv_handle->i_uoff);
	dbg(DBG_INFO, "\t\ti_voff 0x%x\n", ipu_priv_handle->i_voff);

	dbg(DBG_INFO, "\t\033[0;34minput buf paddr:\033[0m\n");
	dbg(DBG_INFO, "\t\tbuf0 0x%x\n", ipu_priv_handle->i_minfo[0].paddr);
	dbg(DBG_INFO, "\t\tbuf1 0x%x\n", ipu_priv_handle->i_minfo[1].paddr);

	if (overlay) {
		dbg(DBG_INFO, "\033[0;34moverlay info:\033[0m\n");
		dbg(DBG_INFO, "\tw: %d\n", overlay->width);
		dbg(DBG_INFO, "\th: %d\n", overlay->height);
		dbg(DBG_INFO, "\tfmt: 0x%x\n", overlay->fmt);
		dbg(DBG_INFO, "\t\tw_posx: %d\n", overlay->ov_crop_win.pos.x);
		dbg(DBG_INFO, "\t\tw_posy: %d\n", overlay->ov_crop_win.pos.y);
		dbg(DBG_INFO, "\t\tw_w: %d\n", overlay->ov_crop_win.win_w);
		dbg(DBG_INFO, "\t\tw_h: %d\n", overlay->ov_crop_win.win_h);

		dbg(DBG_INFO, "\t\033[0;34moverlay crop:\033[0m\n");
		dbg(DBG_INFO, "\t\tovwidth: %d\n", ipu_priv_handle->ovwidth);
		dbg(DBG_INFO, "\t\tovheight: %d\n", ipu_priv_handle->ovheight);
		dbg(DBG_INFO, "\t\tov_off 0x%x\n", ipu_priv_handle->ov_off);
		dbg(DBG_INFO, "\t\tov_uoff 0x%x\n", ipu_priv_handle->ov_uoff);
		dbg(DBG_INFO, "\t\tov_voff 0x%x\n", ipu_priv_handle->ov_voff);
		if (overlay->local_alpha_en)
			dbg(DBG_INFO, "\t\tov_alpha_off 0x%x\n", ipu_priv_handle->ov_alpha_off);

		dbg(DBG_INFO, "\t\033[0;34moverlay buf paddr:\033[0m\n");
		dbg(DBG_INFO, "\t\tbuf0 0x%x\n", ipu_priv_handle->ov_minfo[0].paddr);
		dbg(DBG_INFO, "\t\tbuf1 0x%x\n", ipu_priv_handle->ov_minfo[1].paddr);

		if (overlay->local_alpha_en) {
			dbg(DBG_INFO, "\t\033[0;34moverlay local alpha buf paddr:\033[0m\n");
			dbg(DBG_INFO, "\t\tbuf0 0x%x\n", ipu_priv_handle->ov_alpha_minfo[0].paddr);
			dbg(DBG_INFO, "\t\tbuf1 0x%x\n", ipu_priv_handle->ov_alpha_minfo[1].paddr);
		}
	}

	dbg(DBG_INFO, "\033[0;34moutput info:\033[0m\n");
	dbg(DBG_INFO, "\tw: %d\n", output->width);
	dbg(DBG_INFO, "\th: %d\n", output->height);
	dbg(DBG_INFO, "\trot: %d\n", output->rot);
	dbg(DBG_INFO, "\tfmt: 0x%x\n", output->fmt);
	dbg(DBG_INFO, "\tshow_to_fb: %d\n", output->show_to_fb);
	if (output->show_to_fb) {
		dbg(DBG_INFO, "\t\tfb_num: %d\n", output->fb_disp.fb_num);
		dbg(DBG_INFO, "\t\tfb_w_posx: %d\n", output->fb_disp.pos.x);
		dbg(DBG_INFO, "\t\tfb_w_posy: %d\n", output->fb_disp.pos.y);
	}

	dbg(DBG_INFO, "\t\033[0;34moutput window:\033[0m\n");
	dbg(DBG_INFO, "\t\towidth: %d\n", ipu_priv_handle->output.owidth);
	dbg(DBG_INFO, "\t\toheight: %d\n", ipu_priv_handle->output.oheight);
	dbg(DBG_INFO, "\t\toff 0x%x\n", ipu_priv_handle->output.o_off);
	dbg(DBG_INFO, "\t\tuoff 0x%x\n", ipu_priv_handle->output.o_uoff);
	dbg(DBG_INFO, "\t\tvoff 0x%x\n", ipu_priv_handle->output.o_voff);

	dbg(DBG_INFO, "\t\033[0;34moutput buf paddr:\033[0m\n");
	dbg(DBG_INFO, "\t\tbuf0 0x%x\n", ipu_priv_handle->output.o_minfo[0].paddr);
	dbg(DBG_INFO, "\t\tbuf1 0x%x\n", ipu_priv_handle->output.o_minfo[1].paddr);
	dbg(DBG_INFO, "\t\tbuf2 0x%x\n", ipu_priv_handle->output.o_minfo[2].paddr);

	if (ipu_priv_handle->split_mode) {
		dbg(DBG_INFO, "\033[0;34msplit mode enable:\033[0m\n");
		dbg(DBG_INFO, "left stripe:\n");
		dbg(DBG_INFO, "\tinput width: %d\n", ipu_priv_handle->left_stripe.input_width);
		dbg(DBG_INFO, "\toutput width: %d\n", ipu_priv_handle->left_stripe.output_width);
		dbg(DBG_INFO, "\tinput column: %d\n", ipu_priv_handle->left_stripe.input_column);
		dbg(DBG_INFO, "\toutput column: %d\n", ipu_priv_handle->left_stripe.output_column);
		dbg(DBG_INFO, "\tirr: %d\n", ipu_priv_handle->left_stripe.irr);
		dbg(DBG_INFO, "right stripe:\n");
		dbg(DBG_INFO, "\tinput width: %d\n", ipu_priv_handle->right_stripe.input_width);
		dbg(DBG_INFO, "\toutput width: %d\n", ipu_priv_handle->right_stripe.output_width);
		dbg(DBG_INFO, "\tinput column: %d\n", ipu_priv_handle->right_stripe.input_column);
		dbg(DBG_INFO, "\toutput column: %d\n", ipu_priv_handle->right_stripe.output_column);
	}

	dbg(DBG_INFO, "\033[0;34mEnabling:\033[0m\n");
	/*Setup ipu channel*/
	task_mode = ipu_priv_handle->output.task_mode & ~(COPY_MODE);
	if(task_mode == IC_MODE){
		dma_addr_t buf0, buf1;

		dbg(DBG_INFO, "\tOnly IC, begin & end chan:\n");

		if (ipu_priv_handle->output.ipu_task & IC_ENC) {
			ipu_priv_handle->output.ic_chan = MEM_PRP_ENC_MEM;
			dbg(DBG_INFO, "\t\tMEM_PRP_ENC_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & IC_VF) {
			ipu_priv_handle->output.ic_chan = MEM_PRP_VF_MEM;
			dbg(DBG_INFO, "\t\tMEM_PRP_VF_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & IC_PP) {
			ipu_priv_handle->output.ic_chan = MEM_PP_MEM;
			dbg(DBG_INFO, "\t\tMEM_PP_MEM\n");
		}

		memset(&params, 0, sizeof (params));

		if (ipu_priv_handle->split_mode)
			params.mem_prp_vf_mem.in_width = ipu_priv_handle->left_stripe.input_width;
		else
			params.mem_prp_vf_mem.in_width = ipu_priv_handle->iwidth;
		params.mem_prp_vf_mem.in_height = ipu_priv_handle->iheight;
		params.mem_prp_vf_mem.in_pixel_fmt = input->fmt;

		if (ipu_priv_handle->split_mode) {
			params.mem_prp_vf_mem.out_width = ipu_priv_handle->left_stripe.output_width;
			params.mem_prp_vf_mem.outh_resize_ratio = ipu_priv_handle->left_stripe.irr;
		} else
			params.mem_prp_vf_mem.out_width = ipu_priv_handle->output.owidth;
		params.mem_prp_vf_mem.out_height = ipu_priv_handle->output.oheight;
		params.mem_prp_vf_mem.out_pixel_fmt = output->fmt;

		if (overlay) {
			params.mem_prp_vf_mem.in_g_pixel_fmt = overlay->fmt;
			params.mem_prp_vf_mem.graphics_combine_en = 1;
			params.mem_prp_vf_mem.global_alpha_en = overlay->global_alpha_en;
			params.mem_prp_vf_mem.alpha = overlay->alpha;
			params.mem_prp_vf_mem.key_color_en = overlay->key_color_en;
			params.mem_prp_vf_mem.key_color = overlay->key_color;
			params.mem_prp_vf_mem.alpha_chan_en = overlay->local_alpha_en;
		}

		ret = ipu_init_channel(ipu_priv_handle->output.ic_chan, &params);
		if (ret < 0)
			goto done;

		ipu_priv_handle->input_stride = input->width*bytes_per_pixel(input->fmt);
		buf0 = ipu_priv_handle->i_minfo[0].paddr + ipu_priv_handle->i_off;
		buf1 = ipu_priv_handle->mode & OP_STREAM_MODE ?
			ipu_priv_handle->i_minfo[1].paddr + ipu_priv_handle->i_off : 0;
		/* split mode must use pingpang buffer, set a nonull value to enable double buf */
		if (buf1 == 0 && ipu_priv_handle->split_mode)
			buf1 = buf0;
		ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
				IPU_INPUT_BUFFER,
				input->fmt,
				params.mem_prp_vf_mem.in_width,
				params.mem_prp_vf_mem.in_height,
				ipu_priv_handle->input_stride,
				IPU_ROTATE_NONE,
				buf0,
				buf1,
				ipu_priv_handle->i_uoff, ipu_priv_handle->i_voff);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			goto done;
		}

		if (overlay) {
			ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
					IPU_GRAPH_IN_BUFFER,
					overlay->fmt,
					ipu_priv_handle->ovwidth,
					ipu_priv_handle->ovheight,
					overlay->width*bytes_per_pixel(overlay->fmt),
					IPU_ROTATE_NONE,
					ipu_priv_handle->ov_minfo[0].paddr + ipu_priv_handle->ov_off,
					ipu_priv_handle->mode & OP_STREAM_MODE ?
					ipu_priv_handle->ov_minfo[1].paddr + ipu_priv_handle->ov_off : 0,
					ipu_priv_handle->ov_uoff, ipu_priv_handle->ov_voff);
			if (ret < 0) {
				ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
				goto done;
			}

			if (overlay->local_alpha_en) {
				ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
						IPU_ALPHA_IN_BUFFER,
						IPU_PIX_FMT_GENERIC,
						ipu_priv_handle->ovwidth,
						ipu_priv_handle->ovheight,
						overlay->width,
						IPU_ROTATE_NONE,
						ipu_priv_handle->ov_alpha_minfo[0].paddr + ipu_priv_handle->ov_alpha_off,
						ipu_priv_handle->mode & OP_STREAM_MODE ?
						ipu_priv_handle->ov_alpha_minfo[1].paddr + ipu_priv_handle->ov_alpha_off : 0,
						0, 0);
				if (ret < 0) {
					ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
					goto done;
				}
			}
		}

		if (output->show_to_fb) {
			ipu_priv_handle->output.output_stride = ipu_priv_handle->output.fb_stride;
		} else
			ipu_priv_handle->output.output_stride = output->width*bytes_per_pixel(output->fmt);

		buf0 = ipu_priv_handle->output.o_minfo[0].paddr + ipu_priv_handle->output.o_off;
		buf1 = ipu_priv_handle->mode & OP_STREAM_MODE ?
			ipu_priv_handle->output.o_minfo[1].paddr +
			ipu_priv_handle->output.o_off : 0;
		/* split mode must use pingpang buffer, set a nonull value to enable double buf */
		if (buf1 == 0 && ipu_priv_handle->split_mode)
			buf1 = buf0;
		ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
				IPU_OUTPUT_BUFFER,
				output->fmt,
				params.mem_prp_vf_mem.out_width,
				params.mem_prp_vf_mem.out_height,
				ipu_priv_handle->output.output_stride,
				output->rot,
				buf0,
				buf1,
				ipu_priv_handle->output.o_uoff, ipu_priv_handle->output.o_voff);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			goto done;
		}

		/* fix EBAs for IDMAC channels, for split mode, double buffers work out only one frame */
		if(ipu_priv_handle->split_mode) {
			ipu_priv_handle->left_stripe.input_column *= bytes_per_pixel(input->fmt);
			ipu_priv_handle->right_stripe.input_column *= bytes_per_pixel(input->fmt);
			ipu_priv_handle->left_stripe.output_column *= bytes_per_pixel(output->fmt);
			ipu_priv_handle->right_stripe.output_column *= bytes_per_pixel(output->fmt);

			_ipu_split_mode_set_stripe(ipu_priv_handle, ipu_priv_handle->i_minfo[0].paddr,
					ipu_priv_handle->output.o_minfo[0].paddr, LEFT_STRIPE, 0);
		}

		ipu_priv_handle->output.begin_chan =
			ipu_priv_handle->output.end_chan =
			ipu_priv_handle->output.ic_chan;
	}
	/*Only ROT*/
	else if (task_mode == ROT_MODE){
		dbg(DBG_INFO, "\tOnly ROT, begin & end chan:\n");

		if (ipu_priv_handle->output.ipu_task & ROT_ENC) {
			ipu_priv_handle->output.rot_chan = MEM_ROT_ENC_MEM;
			dbg(DBG_INFO, "\t\tMEM_ROT_ENC_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & ROT_VF) {
			ipu_priv_handle->output.rot_chan = MEM_ROT_VF_MEM;
			dbg(DBG_INFO, "\t\tMEM_ROT_VF_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & ROT_PP) {
			ipu_priv_handle->output.rot_chan = MEM_ROT_PP_MEM;
			dbg(DBG_INFO, "\t\tMEM_ROT_PP_MEM\n");
		}

		ret = ipu_init_channel(ipu_priv_handle->output.rot_chan, NULL);
		if (ret < 0) {
			goto done;
		}

		ipu_priv_handle->input_stride = input->width*bytes_per_pixel(input->fmt);
		ret = ipu_init_channel_buffer(ipu_priv_handle->output.rot_chan,
				IPU_INPUT_BUFFER,
				input->fmt,
				ipu_priv_handle->iwidth,
				ipu_priv_handle->iheight,
				ipu_priv_handle->input_stride,
				output->rot,
				ipu_priv_handle->i_minfo[0].paddr + ipu_priv_handle->i_off,
				ipu_priv_handle->mode & OP_STREAM_MODE ?
				ipu_priv_handle->i_minfo[1].paddr + ipu_priv_handle->i_off : 0,
				ipu_priv_handle->i_uoff, ipu_priv_handle->i_voff);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.rot_chan);
			goto done;
		}

		if (output->show_to_fb) {
			ipu_priv_handle->output.output_stride = ipu_priv_handle->output.fb_stride;
		} else
			ipu_priv_handle->output.output_stride = output->width*bytes_per_pixel(output->fmt);

		ret = ipu_init_channel_buffer(ipu_priv_handle->output.rot_chan,
				IPU_OUTPUT_BUFFER,
				output->fmt,
				ipu_priv_handle->output.owidth,
				ipu_priv_handle->output.oheight,
				ipu_priv_handle->output.output_stride,
				IPU_ROTATE_NONE,
				ipu_priv_handle->output.o_minfo[0].paddr +
				ipu_priv_handle->output.o_off,
				ipu_priv_handle->mode & OP_STREAM_MODE ?
				ipu_priv_handle->output.o_minfo[1].paddr +
				ipu_priv_handle->output.o_off : 0,
				ipu_priv_handle->output.o_uoff, ipu_priv_handle->output.o_voff);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.rot_chan);
			goto done;
		}

		ipu_priv_handle->output.begin_chan =
			ipu_priv_handle->output.end_chan =
			ipu_priv_handle->output.rot_chan;
	}
	/*IC ROT*/
	else if(task_mode == (IC_MODE | ROT_MODE)){
		dbg(DBG_INFO, "\tIC + ROT, begin chan:\n");

		if (ipu_priv_handle->output.ipu_task & IC_ENC) {
			ipu_priv_handle->output.ic_chan = MEM_PRP_ENC_MEM;
			dbg(DBG_INFO, "\t\tMEM_PRP_ENC_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & IC_VF) {
			ipu_priv_handle->output.ic_chan = MEM_PRP_VF_MEM;
			dbg(DBG_INFO, "\t\tMEM_PRP_VF_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & IC_PP) {
			ipu_priv_handle->output.ic_chan = MEM_PP_MEM;
			dbg(DBG_INFO, "\t\tMEM_PP_MEM\n");
		}

		dbg(DBG_INFO, "\tend chan:\n");

		if (ipu_priv_handle->output.ipu_task & ROT_ENC) {
			ipu_priv_handle->output.rot_chan = MEM_ROT_ENC_MEM;
			dbg(DBG_INFO, "\t\tMEM_ROT_ENC_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & ROT_VF) {
			ipu_priv_handle->output.rot_chan = MEM_ROT_VF_MEM;
			dbg(DBG_INFO, "\t\tMEM_ROT_VF_MEM\n");
		} else if (ipu_priv_handle->output.ipu_task & ROT_PP) {
			ipu_priv_handle->output.rot_chan = MEM_ROT_PP_MEM;
			dbg(DBG_INFO, "\t\tMEM_ROT_PP_MEM\n");
		}

		if(output->rot >= IPU_ROTATE_90_RIGHT){
			/*output swap*/
			tmp = ipu_priv_handle->output.owidth;
			ipu_priv_handle->output.owidth = ipu_priv_handle->output.oheight;
			ipu_priv_handle->output.oheight = tmp;
		}

		memset(&params, 0, sizeof (params));

		params.mem_prp_vf_mem.in_width = ipu_priv_handle->iwidth;
		params.mem_prp_vf_mem.in_height = ipu_priv_handle->iheight;
		params.mem_prp_vf_mem.in_pixel_fmt = input->fmt;

		params.mem_prp_vf_mem.out_width = ipu_priv_handle->output.owidth;
		params.mem_prp_vf_mem.out_height = ipu_priv_handle->output.oheight;
		params.mem_prp_vf_mem.out_pixel_fmt = output->fmt;

		if (overlay) {
			params.mem_prp_vf_mem.in_g_pixel_fmt = overlay->fmt;
			params.mem_prp_vf_mem.graphics_combine_en = 1;
			params.mem_prp_vf_mem.global_alpha_en = overlay->global_alpha_en;
			params.mem_prp_vf_mem.alpha = overlay->alpha;
			params.mem_prp_vf_mem.key_color_en = overlay->key_color_en;
			params.mem_prp_vf_mem.key_color = overlay->key_color;
			params.mem_prp_vf_mem.alpha_chan_en = overlay->local_alpha_en;
		}

		ret = ipu_init_channel(ipu_priv_handle->output.ic_chan, &params);
		if (ret < 0) {
			goto done;
		}

		ipu_priv_handle->input_stride = input->width*bytes_per_pixel(input->fmt);
		ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
				IPU_INPUT_BUFFER,
				input->fmt,
				ipu_priv_handle->iwidth,
				ipu_priv_handle->iheight,
				ipu_priv_handle->input_stride,
				IPU_ROTATE_NONE,
				ipu_priv_handle->i_minfo[0].paddr + ipu_priv_handle->i_off,
				ipu_priv_handle->mode & OP_STREAM_MODE ?
				ipu_priv_handle->i_minfo[1].paddr + ipu_priv_handle->i_off : 0,
				ipu_priv_handle->i_uoff, ipu_priv_handle->i_voff);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			goto done;
		}

		if (overlay) {
			ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
					IPU_GRAPH_IN_BUFFER,
					overlay->fmt,
					ipu_priv_handle->ovwidth,
					ipu_priv_handle->ovheight,
					overlay->width*bytes_per_pixel(overlay->fmt),
					IPU_ROTATE_NONE,
					ipu_priv_handle->ov_minfo[0].paddr + ipu_priv_handle->ov_off,
					ipu_priv_handle->mode & OP_STREAM_MODE ?
					ipu_priv_handle->ov_minfo[1].paddr + ipu_priv_handle->ov_off : 0,
					ipu_priv_handle->ov_uoff, ipu_priv_handle->ov_voff);
			if (ret < 0) {
				ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
				goto done;
			}

			if (overlay->local_alpha_en) {
				ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
						IPU_ALPHA_IN_BUFFER,
						IPU_PIX_FMT_GENERIC,
						ipu_priv_handle->ovwidth,
						ipu_priv_handle->ovheight,
						overlay->width,
						IPU_ROTATE_NONE,
						ipu_priv_handle->ov_alpha_minfo[0].paddr + ipu_priv_handle->ov_alpha_off,
						ipu_priv_handle->mode & OP_STREAM_MODE ?
						ipu_priv_handle->ov_alpha_minfo[1].paddr + ipu_priv_handle->ov_alpha_off : 0,
						0, 0);
				if (ret < 0) {
					ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
					goto done;
				}
			}
		}

		ret = ipu_init_channel_buffer(ipu_priv_handle->output.ic_chan,
				IPU_OUTPUT_BUFFER,
				output->fmt,
				ipu_priv_handle->output.owidth,
				ipu_priv_handle->output.oheight,
				ipu_priv_handle->output.owidth*bytes_per_pixel(output->fmt),
				IPU_ROTATE_NONE,
				ipu_priv_handle->output.r_minfo[0].paddr,
				ipu_priv_handle->mode & OP_STREAM_MODE ?
				ipu_priv_handle->output.r_minfo[1].paddr : 0,
				0, 0);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			goto done;
		}

		ret = ipu_init_channel(ipu_priv_handle->output.rot_chan, NULL);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			goto done;
		}

		ret = ipu_init_channel_buffer(ipu_priv_handle->output.rot_chan,
				IPU_INPUT_BUFFER,
				output->fmt,
				ipu_priv_handle->output.owidth,
				ipu_priv_handle->output.oheight,
				ipu_priv_handle->output.owidth*bytes_per_pixel(output->fmt),
				output->rot,
				ipu_priv_handle->output.r_minfo[0].paddr,
				ipu_priv_handle->mode & OP_STREAM_MODE ?
				ipu_priv_handle->output.r_minfo[1].paddr : 0,
				0, 0);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			ipu_uninit_channel(ipu_priv_handle->output.rot_chan);
			goto done;
		}

		if(output->rot >= IPU_ROTATE_90_RIGHT){
			/*output swap*/
			tmp = ipu_priv_handle->output.owidth;
			ipu_priv_handle->output.owidth = ipu_priv_handle->output.oheight;
			ipu_priv_handle->output.oheight = tmp;
		}

		if (output->show_to_fb) {
			ipu_priv_handle->output.output_stride = ipu_priv_handle->output.fb_stride;
		} else
			ipu_priv_handle->output.output_stride = output->width*bytes_per_pixel(output->fmt);

		ret = ipu_init_channel_buffer(ipu_priv_handle->output.rot_chan,
				IPU_OUTPUT_BUFFER,
				output->fmt,
				ipu_priv_handle->output.owidth,
				ipu_priv_handle->output.oheight,
				ipu_priv_handle->output.output_stride,
				IPU_ROTATE_NONE,
				ipu_priv_handle->output.o_minfo[0].paddr +
				ipu_priv_handle->output.o_off,
				ipu_priv_handle->mode & OP_STREAM_MODE ?
				ipu_priv_handle->output.o_minfo[1].paddr +
				ipu_priv_handle->output.o_off : 0,
				ipu_priv_handle->output.o_uoff, ipu_priv_handle->output.o_voff);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			ipu_uninit_channel(ipu_priv_handle->output.rot_chan);
			goto done;
		}

		ret = ipu_link_channels(ipu_priv_handle->output.ic_chan,
				ipu_priv_handle->output.rot_chan);
		if (ret < 0) {
			ipu_uninit_channel(ipu_priv_handle->output.ic_chan);
			ipu_uninit_channel(ipu_priv_handle->output.rot_chan);
			goto done;
		}

		ipu_priv_handle->output.begin_chan = ipu_priv_handle->output.ic_chan;
		ipu_priv_handle->output.end_chan = ipu_priv_handle->output.rot_chan;
	}

	if (output->show_to_fb) {
		dbg(DBG_INFO, "\tdisp chan:\n");
		if (ipu_priv_handle->output.fb_chan == MEM_BG_SYNC) {
			dbg(DBG_INFO, "\t\tMEM_BG_SYNC\n")
		}
		if (ipu_priv_handle->output.fb_chan == MEM_FG_SYNC) {
			dbg(DBG_INFO, "\t\tMEM_FG_SYNC\n")
		}
		if (ipu_priv_handle->output.fb_chan == MEM_DC_SYNC) {
			dbg(DBG_INFO, "\t\tMEM_DC_SYNC\n")
		}
	}

	switch (ipu_priv_handle->output.end_chan) {
		case MEM_ROT_ENC_MEM:
			ipu_priv_handle->irq = IPU_IRQ_PRP_ENC_ROT_OUT_EOF;
			break;
		case MEM_ROT_VF_MEM:
			ipu_priv_handle->irq = IPU_IRQ_PRP_VF_ROT_OUT_EOF;
			break;
		case MEM_ROT_PP_MEM:
			ipu_priv_handle->irq = IPU_IRQ_PP_ROT_OUT_EOF;
			break;
		case MEM_PRP_ENC_MEM:
			ipu_priv_handle->irq = IPU_IRQ_PRP_ENC_OUT_EOF;
			break;
		case MEM_PRP_VF_MEM:
			ipu_priv_handle->irq = IPU_IRQ_PRP_VF_OUT_EOF;
			break;
		case MEM_PP_MEM:
			ipu_priv_handle->irq = IPU_IRQ_PP_OUT_EOF;
			break;
		default:
			dbg(DBG_ERR, "Should not be here!\n");
	}
done:
	return ret;
}

static int _ipu_copy_setup(ipu_lib_input_param_t * input,
		ipu_lib_output_param_t * output,
		ipu_lib_handle_t * ipu_handle)
{
	int ret = 0, hope_task_mode, ipu_task_busy = 0;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;
	unsigned int task;

	hope_task_mode = ipu_priv_handle->mode & 0x07;
	if (ipu_priv_handle->mode & OP_STREAM_MODE) {
		if (!input->user_def_paddr[0] ||
			!input->user_def_paddr[1] ||
			!output->user_def_paddr[0] ||
			!output->user_def_paddr[1]) {
			dbg(DBG_ERR, "Should set both user def paddr for stream mode!\n");
			ret = -1;
			goto done;
		}
	}

	ipu_priv_handle->output.show_to_fb = 0;
	ipu_priv_handle->i_minfo[0].paddr = input->user_def_paddr[0];
	ipu_priv_handle->i_minfo[1].paddr = input->user_def_paddr[1];
	ipu_priv_handle->output.o_minfo[0].paddr = output->user_def_paddr[0];
	ipu_priv_handle->output.o_minfo[1].paddr = output->user_def_paddr[1];

	if (ipu_priv_handle->split_mode) {
		/* try IC-ENC first */
		ipu_priv_handle->output.ipu_task = IC_ENC;
		if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
				(hope_task_mode && ((hope_task_mode & TASK_ENC_MODE) == 0))) {
			/* try IC-PP */
			ipu_priv_handle->output.ipu_task = IC_PP;
			if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
					(hope_task_mode && ((hope_task_mode & TASK_PP_MODE) == 0))) {
				ipu_task_busy = 1;
				goto done;
			}
		}
	} else {
		/* try IC-ENC first */
		ipu_priv_handle->output.ipu_task = IC_ENC;
		if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
				(hope_task_mode && ((hope_task_mode & TASK_ENC_MODE) == 0))) {

			/* try ROT-ENC */
			ipu_priv_handle->output.ipu_task = ROT_ENC;
			if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
					(hope_task_mode && ((hope_task_mode & TASK_ENC_MODE) == 0))) {
				/* hope mode ENC task is busy ? */
				if (hope_task_mode && (hope_task_mode & TASK_ENC_MODE)) {
					ipu_task_busy = 1;
					goto done;
				}

				/* try IC-PP */
				ipu_priv_handle->output.ipu_task = IC_PP;
				if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
						(hope_task_mode && ((hope_task_mode & TASK_PP_MODE) == 0))) {

					/* try ROT-PP */
					ipu_priv_handle->output.ipu_task = ROT_PP;
					if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
							(hope_task_mode &&
							((hope_task_mode & TASK_PP_MODE) == 0))) {
						/* hope mode PP task is busy ? */
						if (hope_task_mode && (hope_task_mode & TASK_PP_MODE)) {
							ipu_task_busy = 1;
							goto done;
						}

						/* try ROT-VF */
						ipu_priv_handle->output.ipu_task = ROT_VF;
						if (_ipu_is_task_busy(ipu_priv_handle->output.ipu_task) ||
								(hope_task_mode &&
								((hope_task_mode & TASK_VF_MODE) == 0))) {
							ipu_task_busy = 1;
							goto done;
						}
					}
				}
			}
		}
	}
	task = ipu_priv_handle->output.ipu_task;
	dbg(DBG_INFO, "\033[0;34mCOPY mode will take ipu task\033[0m\n");
	if (task & IC_ENC)
		dbg(DBG_INFO, "\tIC_ENC\n");
	if (task & IC_PP)
		dbg(DBG_INFO, "\tIC_PP\n");
	if (task & ROT_ENC)
		dbg(DBG_INFO, "\tROT_ENC\n");
	if (task & ROT_VF)
		dbg(DBG_INFO, "\tROT_VF\n");
	if (task & ROT_PP)
		dbg(DBG_INFO, "\tROT_PP\n");

	if ((ipu_priv_handle->output.ipu_task == IC_ENC) ||
		(ipu_priv_handle->output.ipu_task == IC_PP))
		ipu_priv_handle->output.task_mode |= IC_MODE;
	else
		ipu_priv_handle->output.task_mode |= ROT_MODE;

	ret = _ipu_channel_setup(input, NULL, output, ipu_handle);
done:
	if (ipu_task_busy) {
		ret = -1;
		dbg(DBG_ERR, "ipu is busy\n");
		if (hope_task_mode)
			dbg(DBG_ERR, " for hope task mode 0x%x!\n", hope_task_mode);
	}
	return ret;
}

static int _ipu_task_setup(ipu_lib_input_param_t * input,
		ipu_lib_overlay_param_t * overlay,
		ipu_lib_output_param_t * output,
		ipu_lib_handle_t * ipu_handle)
{
	int ret = 0;

	if ((ret = _ipu_mem_alloc(input, overlay, output, ipu_handle)) < 0) {
		_ipu_mem_free(ipu_handle);
		return ret;
	}

	if ((ret = _ipu_channel_setup(input, overlay, output, ipu_handle)) < 0) {
		_ipu_mem_free(ipu_handle);
		return ret;
	}

	return ret;
}

void mxc_ipu_lib_lock(ipu_lib_handle_t * ipu_handle)
{
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;
	unsigned int task = ipu_priv_handle->output.ipu_task;

	if (task & (IC_ENC | ROT_ENC | IC_VF | ROT_VF))
		pthread_mutex_lock(&prp_mutex);
	if (task & (IC_PP | ROT_PP))
		pthread_mutex_lock(&pp_mutex);
}

void mxc_ipu_lib_unlock(ipu_lib_handle_t * ipu_handle)
{
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;
	unsigned int task = ipu_priv_handle->output.ipu_task;

	if (task & (IC_ENC | ROT_ENC | IC_VF | ROT_VF))
		pthread_mutex_unlock(&prp_mutex);
	if (task & (IC_PP | ROT_PP))
		pthread_mutex_unlock(&pp_mutex);
}

/*!
 * This function init the ipu task according to param setting.
 *
 * @param	input		Input parameter for ipu task.
 *
 * @param	overlay		Overlay parameter for ipu task.
 *
 * @param	output		The output paramter for ipu task.
 *
 * @param	mode		The ipu mode user can define, refer to
 * 				header file.
 *
 * @param	ipu_handle	User just allocate this structure for init.
 * 				this parameter will provide some necessary
 * 				info after task init function.
 *
 * @return	This function returns 0 on success or negative error code on
 * 		fail.
 */
int mxc_ipu_lib_task_init(ipu_lib_input_param_t * input,
		ipu_lib_overlay_param_t * overlay,
		ipu_lib_output_param_t * output,
		int mode, ipu_lib_handle_t * ipu_handle)
{
	int ret = 0;
	ipu_lib_priv_handle_t * ipu_priv_handle;

	dbg(DBG_INFO, "\033[0;34m*** mxc_ipu_lib_task_init ***\033[0m\n");

	if (ipu_handle == NULL) {
		dbg(DBG_ERR, "Pls allocate ipu_handle!\n");
		return -1;
	}

	memset(ipu_handle, 0, sizeof(ipu_lib_handle_t));

	ipu_priv_handle = (ipu_lib_priv_handle_t *)malloc(sizeof(ipu_lib_priv_handle_t));
	if (ipu_priv_handle == NULL) {
		dbg(DBG_ERR, "Can not malloc priv handle!\n");
		ret = -1;
		goto done;
	}
	ipu_handle->priv = ipu_priv_handle;

	memset(ipu_priv_handle, 0, sizeof(ipu_lib_priv_handle_t));

	ipu_priv_handle->mode = mode;

	if ((ret = ipu_priv_handle->fd_ipu  = ipu_open()) < 0)
		goto done;

	if ((ret = _ipu_task_check(input, overlay, output, ipu_handle)) < 0) {
		ipu_close();
		goto done;
	}

	mxc_ipu_lib_lock(ipu_handle);

	if (ipu_priv_handle->output.task_mode & COPY_MODE) {
		if ((ret = _ipu_copy_setup(input, output, ipu_handle)) < 0) {
			ipu_close();
			mxc_ipu_lib_unlock(ipu_handle);
			goto done;
		}
	} else {
		if ((ret = _ipu_task_setup(input, overlay, output, ipu_handle)) < 0) {
			ipu_close();
			mxc_ipu_lib_unlock(ipu_handle);
			goto done;
		}
	}

	g_task_in_use |= ipu_priv_handle->output.ipu_task;

	dbg(DBG_INFO, "g_task_in_use 0x%x\n", g_task_in_use);

	mxc_ipu_lib_unlock(ipu_handle);
done:

	return ret;
}

/*!
 * This function uninit the ipu task for special ipu handle.
 *
 * @param	ipu_handle	The ipu task handle need to un-init.
 *
 * @return	This function returns 0 on success or negative error code on
 * 		fail.
 */
void mxc_ipu_lib_task_uninit(ipu_lib_handle_t * ipu_handle)
{
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	dbg(DBG_INFO, "\033[0;34m*** mxc_ipu_lib_task_uninit ***\033[0m\n");

	/* if stream mode, wait for latest frame finish */
	if (ipu_priv_handle->mode & OP_STREAM_MODE) {
		if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
			dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
		} else
			ipu_priv_handle->output_fr_cnt++;
	}

	mxc_ipu_lib_lock(ipu_handle);

	if (ipu_priv_handle->output.show_to_fb) {
		if (ipu_priv_handle->output.fb_chan == MEM_FG_SYNC) {
			struct mxcfb_pos pos = {0};

			if ( ioctl(ipu_priv_handle->output.fd_fb, MXCFB_SET_OVERLAY_POS,
						&pos) < 0)
				dbg(DBG_ERR, "Set FB position failed!\n");
		}
	}

	_ipu_task_disable(ipu_handle);

	dbg(DBG_INFO, "total input frame cnt is %d\n", ipu_priv_handle->input_fr_cnt);
	dbg(DBG_INFO, "total output frame cnt is %d\n", ipu_priv_handle->output_fr_cnt);

	if((ipu_priv_handle->output.task_mode & ROT_MODE) &&
			(ipu_priv_handle->output.task_mode & IC_MODE))
		ipu_unlink_channels(ipu_priv_handle->output.ic_chan,
				ipu_priv_handle->output.rot_chan);

	if(ipu_priv_handle->output.task_mode & IC_MODE)
		ipu_uninit_channel(ipu_priv_handle->output.ic_chan);

	if(ipu_priv_handle->output.task_mode & ROT_MODE)
		ipu_uninit_channel(ipu_priv_handle->output.rot_chan);

	g_task_in_use &= ~(ipu_priv_handle->output.ipu_task);

	dbg(DBG_INFO, "g_task_in_use 0x%x\n", g_task_in_use);

	if (!(ipu_priv_handle->output.task_mode & COPY_MODE))
		_ipu_mem_free(ipu_handle);

	ipu_close();

	free((void *)ipu_priv_handle);

	mxc_ipu_lib_unlock(ipu_handle);
}

int _ipu_task_enable(ipu_lib_handle_t * ipu_handle)
{
	int ret = 0, bufcnt;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;
	unsigned int task_mode;

	if (ipu_priv_handle->mode & OP_STREAM_MODE)
		bufcnt = 2;
	else
		bufcnt = 1;

	/*setup irq*/
	ipu_clear_irq(ipu_priv_handle->irq);
	ret = ipu_register_generic_isr(ipu_priv_handle->irq, NULL);
	if (ret < 0) {
		dbg(DBG_ERR, "Ioctl IPU_REGISTER_GENERIC_ISR %d failed!\n", ipu_priv_handle->irq);
		goto done;
	}

	/* enable channels first*/
	if(ipu_priv_handle->output.task_mode & ROT_MODE)
		ipu_enable_channel(ipu_priv_handle->output.rot_chan);
	if(ipu_priv_handle->output.task_mode & IC_MODE)
		ipu_enable_channel(ipu_priv_handle->output.ic_chan);

	/* set channel buffer ready */
	task_mode = ipu_priv_handle->output.task_mode & ~(COPY_MODE);
	if(task_mode == IC_MODE){
		ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_INPUT_BUFFER, 0);
		if (ipu_priv_handle->overlay_en) {
			ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_GRAPH_IN_BUFFER, 0);
			if (ipu_priv_handle->overlay_local_alpha_en)
				ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_ALPHA_IN_BUFFER, 0);
		}
		ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_OUTPUT_BUFFER, 0);
		if (bufcnt == 2 && !ipu_priv_handle->split_mode) {
			ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_INPUT_BUFFER, 1);
			if (ipu_priv_handle->overlay_en) {
				ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_GRAPH_IN_BUFFER, 1);
				if (ipu_priv_handle->overlay_local_alpha_en)
					ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_ALPHA_IN_BUFFER, 1);
			}
			ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_OUTPUT_BUFFER, 1);
		}
	} else if (task_mode == ROT_MODE){
		ipu_select_buffer(ipu_priv_handle->output.rot_chan, IPU_INPUT_BUFFER, 0);
		ipu_select_buffer(ipu_priv_handle->output.rot_chan, IPU_OUTPUT_BUFFER, 0);
		if (bufcnt == 2) {
			ipu_select_buffer(ipu_priv_handle->output.rot_chan, IPU_INPUT_BUFFER, 1);
			ipu_select_buffer(ipu_priv_handle->output.rot_chan, IPU_OUTPUT_BUFFER, 1);
		}
	} else if(task_mode == (IC_MODE | ROT_MODE)){
		ipu_select_buffer(ipu_priv_handle->output.rot_chan, IPU_OUTPUT_BUFFER, 0);
		if (bufcnt == 2)
			ipu_select_buffer(ipu_priv_handle->output.rot_chan, IPU_OUTPUT_BUFFER, 1);

		ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_INPUT_BUFFER, 0);
		if (ipu_priv_handle->overlay_en) {
			ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_GRAPH_IN_BUFFER, 0);
			if (ipu_priv_handle->overlay_local_alpha_en)
				ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_ALPHA_IN_BUFFER, 0);
		}
		ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_OUTPUT_BUFFER, 0);
		if (bufcnt == 2) {
			ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_INPUT_BUFFER, 1);
			if (ipu_priv_handle->overlay_en) {
				ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_GRAPH_IN_BUFFER, 1);
				if (ipu_priv_handle->overlay_local_alpha_en)
					ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_ALPHA_IN_BUFFER, 1);
			}
			ipu_select_buffer(ipu_priv_handle->output.ic_chan, IPU_OUTPUT_BUFFER, 1);
		}
	}

done:
	return ret;
}

void _ipu_task_disable(ipu_lib_handle_t * ipu_handle)
{
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	ipu_free_irq(ipu_priv_handle->irq, NULL);

	if(ipu_priv_handle->output.task_mode & IC_MODE){
		if (ipu_priv_handle->output.ipu_task & IC_ENC) {
			ipu_clear_irq(IPU_IRQ_PRP_IN_EOF);
			ipu_clear_irq(IPU_IRQ_PRP_ENC_OUT_EOF);
		} else if (ipu_priv_handle->output.ipu_task & IC_VF) {
			ipu_clear_irq(IPU_IRQ_PRP_IN_EOF);
			ipu_clear_irq(IPU_IRQ_PRP_VF_OUT_EOF);
		} else if (ipu_priv_handle->output.ipu_task & IC_PP) {
			ipu_clear_irq(IPU_IRQ_PP_IN_EOF);
			ipu_clear_irq(IPU_IRQ_PP_OUT_EOF);
		}
		ipu_disable_channel(ipu_priv_handle->output.ic_chan, 1);
	}

	if(ipu_priv_handle->output.task_mode & ROT_MODE){
		if (ipu_priv_handle->output.ipu_task & ROT_ENC) {
			ipu_clear_irq(IPU_IRQ_PRP_ENC_ROT_IN_EOF);
			ipu_clear_irq(IPU_IRQ_PRP_ENC_ROT_OUT_EOF);
		} else if (ipu_priv_handle->output.ipu_task & ROT_VF) {
			ipu_clear_irq(IPU_IRQ_PRP_VF_ROT_IN_EOF);
			ipu_clear_irq(IPU_IRQ_PRP_VF_ROT_OUT_EOF);
		} else if (ipu_priv_handle->output.ipu_task & ROT_PP) {
			ipu_clear_irq(IPU_IRQ_PP_ROT_IN_EOF);
			ipu_clear_irq(IPU_IRQ_PP_ROT_OUT_EOF);
		}
		ipu_disable_channel(ipu_priv_handle->output.rot_chan, 1);
	}
}

int _ipu_wait_for_irq(int irq, int times)
{
	int wait = 0;
	ipu_event_info info;
	info.irq = irq;

	while (ipu_get_interrupt_event(&info) < 0) {
		dbg(DBG_INFO, "Can not get wait irq %d, try again!\n", irq);
		wait += 1;
		if (wait >= times)
			break;
	}

	if (wait < times)
		return 0;
	else
		return 1;
}

int pan_display(ipu_lib_priv_handle_t * ipu_priv_handle, int idx)
{
	struct fb_var_screeninfo fb_var;
	int ret = 0;

	if (ioctl(ipu_priv_handle->output.fd_fb, FBIOGET_VSCREENINFO, &fb_var) < 0) {
		dbg(DBG_ERR, "Get FB var info for output failed!\n");
		ret = -1;
		goto done;
	}

	if (idx == 0)
		fb_var.yoffset = fb_var.yres;
	else if (idx == 1)
		fb_var.yoffset = 0;
	else
		fb_var.yoffset = 2*fb_var.yres;

	if (ioctl(ipu_priv_handle->output.fd_fb, FBIOPAN_DISPLAY, &fb_var) < 0) {
		dbg(DBG_WARNING, "Set FB pan display failed!\n");
		ret = -1;
		goto done;
	}

done:
	return ret;
}

/*!
 * This function update the buffer for special ipu task, it must be run after
 * init function.
 * For OP_STREAM_MODE mode, ipu task will take double buffer method, this function
 * will return the next need-update buffer index number(0 or 1) on success, user
 * should update input buffer according to it.
 * Similar with it, output_callback's second parameter indicates the current output
 * buffer index number(0 or 1), user should read output data from exact buffer
 * according to it.
 * For OP_NORMAL_MODE mode, ipu task will take single buffer method, so this function
 * will always return 0 on success(next update buffer will keep on index 0), the same,
 * output_callback's second parameter will keep on 0 too.
 * How to update input buffer? If user has phys buffer themselves, please just update
 * the phys buffer address by parameter phyaddr; if not, user can fill the input data
 * to ipu_handle->inbuf_start[].
 *
 * @param	ipu_handle	The ipu task handle need to update buffer.
 *
 * @param	new_inbuf_paddr	User can set phyaddr to their own allocated
 * 				buffer addr, ipu lib will update the buffer
 * 				from this address for process. If user do not
 * 				want to use it, please let it be zero, and
 * 				fill the buffer according to inbuf_start
 * 				parameter in ipu_handle.
 *
 * @param	new_ovbuf_paddr User defined overlay physical buffer address.
 *
 * @param	output_callback	IPU lib will call output_callback funtion
 * 				when there is output data.
 *
 * @param	output_cb_arg	The argument will be passed to output_callback.
 *
 * @return	This function returns the next update buffer index number on success
 * 		or negative error code on fail.
 */
int mxc_ipu_lib_task_buf_update(ipu_lib_handle_t * ipu_handle,
	dma_addr_t new_inbuf_paddr, dma_addr_t new_ovbuf_paddr,
	dma_addr_t new_ovbuf_alpha_paddr, void (output_callback)(void *, int),
	void * output_cb_arg)
{
	int ret;
	ipu_lib_priv_handle_t * ipu_priv_handle = (ipu_lib_priv_handle_t *)ipu_handle->priv;

	if (ipu_priv_handle->enabled == 0) {
		/* first time will not update new buf, it just enable task and
		 * make sure first frame done
		 * init value:
		 * output_bufnum = 0;
		 * update_bufnum = 0;
		 * tri_output_bufnum = 2 or 1; (it manages output tripple buf)
		 */
		mxc_ipu_lib_lock(ipu_handle);

		if ((ret = _ipu_task_enable(ipu_handle)) < 0) {
			mxc_ipu_lib_unlock(ipu_handle);
			return ret;
		}

		mxc_ipu_lib_unlock(ipu_handle);

		dbg(DBG_INFO, "\033[0;34mipu task begin:\033[0m\n");

		if (ipu_priv_handle->mode & OP_STREAM_MODE)
			ipu_priv_handle->input_fr_cnt = 2;
		else
			ipu_priv_handle->input_fr_cnt = 1;

		if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
			dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
			return -1;
		}

		if (ipu_priv_handle->split_mode) {
			_ipu_split_mode_set_stripe(ipu_priv_handle, ipu_priv_handle->i_minfo[0].paddr,
						ipu_priv_handle->output.o_minfo[0].paddr, RIGHT_STRIPE, 1);

			if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
				dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
				return -1;
			}
			dbg(DBG_DEBUG, "split mode first frame done!\n");
		}

		if (output_callback)
			output_callback(output_cb_arg, ipu_priv_handle->output_bufnum);

		if (ipu_priv_handle->output.show_to_fb)
			pan_display(ipu_priv_handle, ipu_priv_handle->output_bufnum);

		if (ipu_priv_handle->split_mode && (ipu_priv_handle->mode & OP_STREAM_MODE)) {
			_ipu_split_mode_set_stripe(ipu_priv_handle, ipu_priv_handle->i_minfo[1].paddr,
						ipu_priv_handle->output.o_minfo[1].paddr, LEFT_STRIPE, 1);

			if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
				dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
				return -1;
			}

			_ipu_split_mode_set_stripe(ipu_priv_handle, ipu_priv_handle->i_minfo[1].paddr,
						ipu_priv_handle->output.o_minfo[1].paddr, RIGHT_STRIPE, 1);
		}

		if (ipu_priv_handle->mode & OP_STREAM_MODE)
			ipu_priv_handle->tri_output_bufnum = 2;
		else if (ipu_priv_handle->output.show_to_fb)
			ipu_priv_handle->tri_output_bufnum = 1;
		else
			ipu_priv_handle->tri_output_bufnum = 0;

		ipu_priv_handle->output_fr_cnt = 1;
		ipu_priv_handle->enabled = 1;
	} else {
		dbg(DBG_DEBUG, "update pingpang %d\n", ipu_priv_handle->update_bufnum);
		dbg(DBG_DEBUG, "output tripple %d\n", ipu_priv_handle->output_bufnum);
		dbg(DBG_DEBUG, "output update tripple %d\n", ipu_priv_handle->tri_output_bufnum);

		if (ipu_priv_handle->mode & OP_STREAM_MODE) {
			if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
				dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
				return -1;
			}

			if (output_callback)
				output_callback(output_cb_arg, ipu_priv_handle->output_bufnum);

			if (ipu_priv_handle->output.show_to_fb)
				pan_display(ipu_priv_handle, ipu_priv_handle->output_bufnum);
		}

		if (new_inbuf_paddr) {
			dbg(DBG_DEBUG, "update input with user defined buffer phy 0x%x\n", new_inbuf_paddr);
			if (!ipu_priv_handle->split_mode)
				ipu_update_channel_buffer(ipu_priv_handle->output.begin_chan, IPU_INPUT_BUFFER,
						ipu_priv_handle->update_bufnum,
						new_inbuf_paddr + ipu_priv_handle->i_off);
			else {
				_ipu_split_mode_set_stripe(ipu_priv_handle, new_inbuf_paddr,
						ipu_priv_handle->output.o_minfo[ipu_priv_handle->tri_output_bufnum].paddr,
						LEFT_STRIPE, 1);
				if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
					dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
					return -1;
				}
				_ipu_split_mode_set_stripe(ipu_priv_handle, new_inbuf_paddr,
						ipu_priv_handle->output.o_minfo[ipu_priv_handle->tri_output_bufnum].paddr,
						RIGHT_STRIPE, 1);
			}
		} else if (ipu_priv_handle->split_mode) {
				_ipu_split_mode_set_stripe(ipu_priv_handle,
						ipu_priv_handle->i_minfo[ipu_priv_handle->update_bufnum].paddr,
						ipu_priv_handle->output.o_minfo[ipu_priv_handle->tri_output_bufnum].paddr,
						LEFT_STRIPE, 1);
				if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
					dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
					return -1;
				}
				_ipu_split_mode_set_stripe(ipu_priv_handle,
						ipu_priv_handle->i_minfo[ipu_priv_handle->update_bufnum].paddr,
						ipu_priv_handle->output.o_minfo[ipu_priv_handle->tri_output_bufnum].paddr,
						RIGHT_STRIPE, 1);
		}

		if (new_ovbuf_paddr && ipu_priv_handle->overlay_en) {
			dbg(DBG_DEBUG, "update overlay with user defined buffer phy 0x%x\n", new_ovbuf_paddr);
			ipu_update_channel_buffer(ipu_priv_handle->output.begin_chan, IPU_GRAPH_IN_BUFFER,
					ipu_priv_handle->update_bufnum, new_ovbuf_paddr + ipu_priv_handle->ov_off);
		}
		if (new_ovbuf_alpha_paddr && ipu_priv_handle->overlay_en && ipu_priv_handle->overlay_local_alpha_en) {
			dbg(DBG_DEBUG, "update overlay local alpha blending with user defined buffer phy 0x%x\n", new_ovbuf_alpha_paddr);
			ipu_update_channel_buffer(ipu_priv_handle->output.begin_chan, IPU_ALPHA_IN_BUFFER,
				ipu_priv_handle->update_bufnum, new_ovbuf_alpha_paddr + ipu_priv_handle->ov_alpha_off);
		}
		if (ipu_priv_handle->overlay_en) {
			ipu_select_buffer(ipu_priv_handle->output.begin_chan, IPU_GRAPH_IN_BUFFER,
					ipu_priv_handle->update_bufnum);
			if (ipu_priv_handle->overlay_local_alpha_en)
				ipu_select_buffer(ipu_priv_handle->output.begin_chan, IPU_ALPHA_IN_BUFFER,
					ipu_priv_handle->update_bufnum);
		}

		if (!ipu_priv_handle->split_mode) {
			ipu_update_channel_buffer(ipu_priv_handle->output.end_chan,
					IPU_OUTPUT_BUFFER, ipu_priv_handle->update_bufnum,
					ipu_priv_handle->output.o_minfo[ipu_priv_handle->tri_output_bufnum].paddr
					+ ipu_priv_handle->output.o_off);
			ipu_select_buffer(ipu_priv_handle->output.end_chan,
					IPU_OUTPUT_BUFFER, ipu_priv_handle->update_bufnum);
			ipu_select_buffer(ipu_priv_handle->output.begin_chan, IPU_INPUT_BUFFER,
					ipu_priv_handle->update_bufnum);
		}

		if (ipu_priv_handle->mode & OP_STREAM_MODE)
			ipu_priv_handle->update_bufnum = ipu_priv_handle->update_bufnum ? 0 : 1;
		else {
			if (_ipu_wait_for_irq(ipu_priv_handle->irq, 1)) {
				dbg(DBG_ERR, "wait for irq %d time out!\n", ipu_priv_handle->irq);
				return -1;
			}

			if (output_callback)
				output_callback(output_cb_arg, ipu_priv_handle->output_bufnum);

			if (ipu_priv_handle->output.show_to_fb)
				pan_display(ipu_priv_handle, ipu_priv_handle->tri_output_bufnum);
		}

		if (ipu_priv_handle->mode & OP_STREAM_MODE || ipu_priv_handle->output.show_to_fb)
			ipu_priv_handle->tri_output_bufnum = (++ipu_priv_handle->tri_output_bufnum) % 3;

		ipu_priv_handle->input_fr_cnt++;
		ipu_priv_handle->output_fr_cnt++;
	}

	if (ipu_priv_handle->mode & OP_STREAM_MODE)
		ipu_priv_handle->output_bufnum = (++ipu_priv_handle->output_bufnum) % 3;

	return ipu_priv_handle->update_bufnum;
}

#ifdef __cplusplus
}
#endif
