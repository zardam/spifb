/*
 *  linux/drivers/video/vfb.c -- Virtual frame buffer device
 *
 *      Copyright (C) 2002 James Simmons
 *
 *	Copyright (C) 1997 Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License. See the file COPYING in the main directory of this archive for
 *  more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>

#include <linux/fb.h>
#include <linux/init.h>
#include <linux/dma-mapping.h>
#include <linux/spi/spi.h>

#define WIDTH 320
#define HEIGHT 240
#define SPI_SPEED 70000000

static struct spi_device *spi_device;
static void *videomemory;
static u_long videomemorysize = WIDTH * HEIGHT * 2;

static struct fb_var_screeninfo spifb_var = {
	.xres		= WIDTH,
	.yres		= HEIGHT,
	.xres_virtual	= WIDTH,
	.yres_virtual	= HEIGHT,
	.width		= WIDTH,
	.height		= HEIGHT,
	.bits_per_pixel	= 16,
	.red		= {11, 5, 0},
	.green		= {5, 6, 0},
	.blue		= {0, 5, 0},
	.activate	= FB_ACTIVATE_NOW,
	.vmode		= FB_VMODE_NONINTERLACED,
};

static struct fb_fix_screeninfo spifb_fix = {
	.id =		"SPI FB",
	.type =		FB_TYPE_PACKED_PIXELS,
	.visual =	FB_VISUAL_TRUECOLOR,
	.accel =	FB_ACCEL_NONE,
	.line_length = WIDTH * 2,
};

uint16_t *txbuffer;
static void spifb_update(struct fb_info *info, struct list_head *pagelist) {
	int i;
	struct spi_transfer t1 = {
		.tx_buf = txbuffer,
		.len = 32768,
	};
	struct spi_transfer t2 = {
		.tx_buf = txbuffer + 16384,
		.len = 32768,
	};
	struct spi_transfer t3 = {
		.tx_buf = txbuffer + 16384 * 2,
		.len = 32768,
	};
	struct spi_transfer t4 = {
		.tx_buf = txbuffer + 16384 * 3,
		.len = 32768,
	};
	struct spi_transfer t5 = {
		.tx_buf = txbuffer + 16384 * 4,
		.len = videomemorysize - 32768*4,
	};
	struct spi_message m;

	for(i = 0; i < videomemorysize/2; i++) {
			txbuffer[i]=cpu_to_be16(((uint16_t *) videomemory)[i]);
	}

	spi_message_init(&m);
	spi_message_add_tail(&t1, &m);
	spi_message_add_tail(&t2, &m);
	spi_message_add_tail(&t3, &m);
	spi_message_add_tail(&t4, &m);
	spi_message_add_tail(&t5, &m);

	spi_sync(spi_device, &m);
}

static struct fb_deferred_io spifb_defio = {
        .delay          = HZ / 60,
        .deferred_io    = spifb_update,
};

static int spifb_setcolreg(u_int regno, u_int red, u_int green, u_int blue,
			 u_int transp, struct fb_info *info)
{
	if (regno >= 256)	/* no. of hw registers */
		return 1;

	/* grayscale works only partially under directcolor */
	if (info->var.grayscale) {
		/* grayscale = 0.30*R + 0.59*G + 0.11*B */
		red = green = blue =
		    (red * 77 + green * 151 + blue * 28) >> 8;
	}

#define CNVT_TOHW(val,width) ((((val)<<(width))+0x7FFF-(val))>>16)
	switch (info->fix.visual) {
	case FB_VISUAL_TRUECOLOR:
	case FB_VISUAL_PSEUDOCOLOR:
		red = CNVT_TOHW(red, info->var.red.length);
		green = CNVT_TOHW(green, info->var.green.length);
		blue = CNVT_TOHW(blue, info->var.blue.length);
		transp = CNVT_TOHW(transp, info->var.transp.length);
		break;
	case FB_VISUAL_DIRECTCOLOR:
		red = CNVT_TOHW(red, 8);	/* expect 8 bit DAC */
		green = CNVT_TOHW(green, 8);
		blue = CNVT_TOHW(blue, 8);
		/* hey, there is bug in transp handling... */
		transp = CNVT_TOHW(transp, 8);
		break;
	}
#undef CNVT_TOHW
	/* Truecolor has hardware independent palette */
	if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
		u32 v;

		if (regno >= 16)
			return 1;

		v = (red << info->var.red.offset) |
		    (green << info->var.green.offset) |
		    (blue << info->var.blue.offset) |
		    (transp << info->var.transp.offset);
		switch (info->var.bits_per_pixel) {
		case 8:
			break;
		case 16:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		case 24:
		case 32:
			((u32 *) (info->pseudo_palette))[regno] = v;
			break;
		}
		return 0;
	}

	return 0;
}

static void trigger_update(struct fb_info *info)
{
	struct fb_deferred_io *fbdefio = info->fbdefio;
	schedule_delayed_work(&info->deferred_work, fbdefio->delay);
}

static void spifb_fillrect(struct fb_info *p, const struct fb_fillrect *rect)
{
	sys_fillrect(p, rect);
	trigger_update(p);
}

static void spifb_imageblit(struct fb_info *p, const struct fb_image *image)
{
	sys_imageblit(p, image);
	trigger_update(p);
}

static void spifb_copyarea(struct fb_info *p, const struct fb_copyarea *area)
{
	sys_copyarea(p, area);
	trigger_update(p);
}

static ssize_t spifb_write(struct fb_info *p, const char __user *buf,
				size_t count, loff_t *ppos)
{
	ssize_t res;
	res = fb_sys_write(p, buf, count, ppos);
	trigger_update(p);
	return res;
}

static struct fb_ops spifb_ops = {
	.owner		    = THIS_MODULE,
	.fb_read      = fb_sys_read,
	.fb_write     = spifb_write,
	.fb_setcolreg	= spifb_setcolreg,
	.fb_fillrect	= spifb_fillrect,
	.fb_copyarea	= spifb_copyarea,
	.fb_imageblit	= spifb_imageblit,
};

dma_addr_t dev_addr;
unsigned int size;
static int spifb_probe(struct platform_device *dev)
{
	struct fb_info *info;
	int retval = -ENOMEM;

	size = PAGE_ALIGN(videomemorysize);

	if (!(txbuffer = dma_alloc_coherent(NULL, size, &dev_addr, GFP_KERNEL)))
		return retval;

	if (!(videomemory = vmalloc_32_user(size)))
		goto err;

	info = framebuffer_alloc(sizeof(u32) * 256, &dev->dev);
	if (!info)
		goto err0;

	info->screen_base = (char __iomem *) videomemory;
	info->fbops = &spifb_ops;

	spifb_fix.smem_start = (unsigned long) videomemory;
	spifb_fix.smem_len = videomemorysize;
	info->fix = spifb_fix;
	info->var = spifb_var;
	info->pseudo_palette = info->par;
	info->par = NULL;
	info->flags = FBINFO_FLAG_DEFAULT;

	retval = fb_alloc_cmap(&info->cmap, 256, 0);
	if (retval < 0)
		goto err1;

	info->fbdefio = &spifb_defio;
	fb_deferred_io_init(info);

	retval = register_framebuffer(info);
	if (retval < 0)
		goto err2;
	platform_set_drvdata(dev, info);

	fb_info(info, "SPI frame buffer device, using %ldK of video memory\n",
		videomemorysize >> 10);

	return 0;
err2:
	fb_dealloc_cmap(&info->cmap);
err1:
	framebuffer_release(info);
err0:
	vfree(videomemory);
err:
	dma_free_coherent(NULL, size, txbuffer, dev_addr);
	return retval;
}

static int spifb_remove(struct platform_device *dev)
{
	struct fb_info *info = platform_get_drvdata(dev);

	if (info) {
		fb_deferred_io_cleanup(info);
		unregister_framebuffer(info);
		vfree(videomemory);
		dma_free_coherent(NULL, size, txbuffer, dev_addr);
		fb_dealloc_cmap(&info->cmap);
		framebuffer_release(info);
	}
	return 0;
}

static struct platform_driver spifb_driver = {
	.probe	= spifb_probe,
	.remove = spifb_remove,
	.driver = {
		.name	= "spifb",
	},
};

static struct spi_board_info spi __initconst = {
	.max_speed_hz	= SPI_SPEED,
	.bus_num	= 0,
	.chip_select	= 0,
	.mode		= SPI_MODE_0,
};

static int spi_device_register(void) {
	struct spi_master *master;
	struct device *dev;
	char str[32];

	master = spi_busnum_to_master(spi.bus_num);
	if (!master) {
		pr_err("spi_busnum_to_master(%d) returned NULL\n",
		       spi.bus_num);
		return -EINVAL;
	}

  snprintf(str, sizeof(str), "%s.%u", dev_name(&master->dev), 0);
	dev = bus_find_device_by_name(&spi_bus_type, NULL, str);
	device_del(dev);

	spi_device = spi_new_device(master, &spi);
	put_device(&master->dev);
	if (!spi_device) {
		dev_err(&master->dev, "spi_new_device() returned NULL\n");
		return -EPERM;
	}
	return 0;
}

static struct platform_device *spifb_device;

static int __init spifb_init(void)
{
	int ret = 0;

	spi_device_register(); // TODO: verify result ! & release

	ret = platform_driver_register(&spifb_driver);

	if (!ret) {
		spifb_device = platform_device_alloc("spifb", 0);

		if (spifb_device)
			ret = platform_device_add(spifb_device);
		else
			ret = -ENOMEM;

		if (ret) {
			platform_device_put(spifb_device);
			platform_driver_unregister(&spifb_driver);
		}
	}

	return ret;
}

module_init(spifb_init);

static void __exit spifb_exit(void)
{
	platform_device_unregister(spifb_device);
	platform_driver_unregister(&spifb_driver);
}

module_exit(spifb_exit);

MODULE_LICENSE("GPL");
