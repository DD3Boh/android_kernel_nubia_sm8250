/*
 * nubia_disp_preference.c - nubia lcd display color enhancement and temperature setting
 *	      Linux kernel modules for mdss
 *
 * Copyright (c) 2015 nubia <nubia@nubia.com.cn>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/*
 * Supports NUBIA lcd display color enhancement and color temperature setting
 */

/*------------------------------ header file --------------------------------*/
#include "nubia_disp_preference.h"
#include "nubia_dp_preference.h"
#include "dp_debug.h"
#include <linux/delay.h>

#define CONFIG_NUBIA_HDMI_NODE_FEATURE

static struct dsi_display *nubia_display=NULL;
/*------------------------------- variables ---------------------------------*/
static struct kobject *enhance_kobj = NULL;
struct nubia_disp_type nubia_disp_val = {
	.en_hbm_mode = 1,
	.hbm_mode = HBM_OFF,
	.panel_type = DFPS_120
};

int fps_store = 90;
int fps_temp = 90;
extern bool enable_flag;
extern bool is_66451_panel;
#ifdef CONFIG_NUBIA_HDMI_NODE_FEATURE
extern struct _select_sde_edid_info select_sde_edid_info;
extern char edid_mode_best_info[32];
extern struct dp_debug_private *debug_node;
#endif

static ssize_t hbm_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
		if (nubia_display == NULL){
				NUBIA_DISP_ERROR("no nubia_display node!\n");
				return -EINVAL;
		}

		return snprintf(buf, PAGE_SIZE, "%d\n", nubia_disp_val.hbm_mode);
}

static ssize_t hbm_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t size)
{
	uint32_t val = 0;
	int ret = 0;
	if (nubia_display == NULL) {
		NUBIA_DISP_ERROR("no hbm_mode\n");
		return size;
	}
	sscanf(buf, "%d", &val);
	if ((val != HBM_OFF) && (val != HBM_ON)) {
		NUBIA_DISP_ERROR("invalid hbm_mode val = %d\n", val);
		return size;
	}
	if (!is_66451_panel) {
		NUBIA_DISP_INFO("r66455 un-support hbm mode \n");
		return size;
	}
	NUBIA_DISP_INFO("hbm_mode value = %d\n", val);
	
	ret = nubia_dsi_panel_hbm(nubia_display->panel, val);
	if (ret == 0) {
		nubia_disp_val.hbm_mode = val;
		NUBIA_DISP_INFO("success to set hbm_mode as = %d\n", val);
	}

	return size;
}

#ifdef CONFIG_NUBIA_HDMI_NODE_FEATURE
static ssize_t dp_debug_hpd_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return 0;
}

static ssize_t dp_debug_hpd_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t size)
{
	int const hpd_data_mask = 0x7;
	int hpd = 0;

	if (!debug_node)
		return -ENODEV;

    sscanf(buf, "%d", &hpd);
	printk("%s:  hpd = %d \n", __func__, hpd);
	
	hpd &= hpd_data_mask;
	debug_node->hotplug = !!(hpd & BIT(0));

	debug_node->dp_debug.psm_enabled = !!(hpd & BIT(1));

	/*
	 * print hotplug value as this code is executed
	 * only while running in debug mode which is manually
	 * triggered by a tester or a script.
	 */
	DP_INFO("%s\n", debug_node->hotplug ? "[CONNECT]" : "[DISCONNECT]");
	if(hpd == 0)
	{
		select_sde_edid_info.edid_hot_plug = true;
	}

	debug_node->hpd->simulate_connect(debug_node->hpd, debug_node->hotplug);

	return size;
}

static ssize_t edid_modes_show(struct kobject *kobj,
		 struct kobj_attribute *attr, char *buf)
{
		char *buf_edid;
		u32 len = 0, ret = 0, max_size = SZ_4K;
		int rc = 0;
	
		buf_edid = kzalloc(SZ_4K, GFP_KERNEL);
		if (ZERO_OR_NULL_PTR(buf_edid)) {
			rc = -ENOMEM;
			goto error;
		}
	
		ret = snprintf(buf_edid, max_size, "%s", edid_mode_best_info);
		len = snprintf(buf_edid + ret, max_size, "%s", select_sde_edid_info.edid_mode_info);
		
		NUBIA_DISP_INFO("--- len = %d, edid_mode_best_info = %s	 select_sde_edid_info.edid_mode_info = %s\n", 
			len, edid_mode_best_info, select_sde_edid_info.edid_mode_info);
				
		len = sprintf(buf, "%s", buf_edid);
 		kfree(buf_edid);
	
		return len;
	error:
		return rc;

}

static ssize_t edid_modes_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t size)
{
	int hdisplay = 0, vdisplay = 0, vrefresh = 0, aspect_ratio;

	if (!debug_node)
		return -ENODEV;

	if (sscanf(buf, "%d %d %d %d", &hdisplay, &vdisplay, &vrefresh,
				&aspect_ratio) != 4)
		goto clear;
	NUBIA_DISP_INFO("hdisplay = %d, vdisplay = %d, vrefresh = %d, aspect_ratio = %d\n", 
		hdisplay, vdisplay, vrefresh, aspect_ratio);
		
	if (!hdisplay || !vdisplay || !vrefresh)
		goto clear;
	select_sde_edid_info.node_control = true;
	debug_node->dp_debug.debug_en = true;
	debug_node->dp_debug.hdisplay = hdisplay;
	debug_node->dp_debug.vdisplay = vdisplay;
	debug_node->dp_debug.vrefresh = vrefresh;
	debug_node->dp_debug.aspect_ratio = aspect_ratio;
	/*store the select fps and resulation of edid_mode_info*/
	memset(edid_mode_best_info, 0x00, 32);
	snprintf(edid_mode_best_info, 32,"%dx%d %d %d\n",hdisplay, vdisplay,vrefresh, aspect_ratio);
	
	select_sde_edid_info.edid_mode_store = true;
	goto end;
clear:
	NUBIA_DISP_INFO("clearing debug modes\n");
	debug_node->dp_debug.debug_en = false;
end:
	return size;
}
#endif

static struct kobj_attribute lcd_disp_attrs[] = {
	__ATTR(hbm_mode,        0664, hbm_show,       hbm_store),
#ifdef CONFIG_NUBIA_HDMI_NODE_FEATURE
    __ATTR(edid_modes,        0664, edid_modes_show,       edid_modes_store),
    __ATTR(hpd,        0664,        dp_debug_hpd_show,     dp_debug_hpd_store), 
#endif

};

void nubia_set_dsi_ctrl(struct dsi_display *display)
{
	NUBIA_DISP_INFO("start\n");
	nubia_display = display;
}

static int __init nubia_disp_preference_init(void)
{
	int retval = 0;
	int attr_count = 0;

	NUBIA_DISP_INFO("start\n");

	enhance_kobj = kobject_create_and_add("lcd_enhance", kernel_kobj);

	if (!enhance_kobj) {
		NUBIA_DISP_ERROR("failed to create and add kobject\n");
		return -ENOMEM;
	}

	/* Create attribute files associated with this kobject */
	for (attr_count = 0; attr_count < ARRAY_SIZE(lcd_disp_attrs); attr_count++) {
		retval = sysfs_create_file(enhance_kobj, &lcd_disp_attrs[attr_count].attr);
		if (retval < 0) {
			NUBIA_DISP_ERROR("failed to create sysfs attributes\n");
			goto err_sys_creat;
		}
	}
	NUBIA_DISP_INFO("success\n");

	return retval;

err_sys_creat:
	for (--attr_count; attr_count >= 0; attr_count--)
		sysfs_remove_file(enhance_kobj, &lcd_disp_attrs[attr_count].attr);

	kobject_put(enhance_kobj);
	return retval;
}

static void __exit nubia_disp_preference_exit(void)
{
	int attr_count = 0;

	for (attr_count = 0; attr_count < ARRAY_SIZE(lcd_disp_attrs); attr_count++)
		sysfs_remove_file(enhance_kobj, &lcd_disp_attrs[attr_count].attr);
	kobject_put(enhance_kobj);
}

MODULE_AUTHOR("NUBIA LCD Driver Team Software");
MODULE_DESCRIPTION("NUBIA LCD DISPLAY Color Saturation and Temperature Setting");
MODULE_LICENSE("GPL");
module_init(nubia_disp_preference_init);
module_exit(nubia_disp_preference_exit);
