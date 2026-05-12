#include <linux/module.h>
#include <linux/export-internal.h>
#include <linux/compiler.h>

MODULE_INFO(name, KBUILD_MODNAME);

__visible struct module __this_module
__section(".gnu.linkonce.this_module") = {
	.name = KBUILD_MODNAME,
	.init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
	.exit = cleanup_module,
#endif
	.arch = MODULE_ARCH_INIT,
};



static const struct modversion_info ____versions[]
__used __section("__versions") = {
	{ 0x31c0f9b0, "vb2_queue_init" },
	{ 0x3fdc3369, "usb_alloc_urb" },
	{ 0x08552149, "video_ioctl2" },
	{ 0x0c8e9a2d, "usb_free_urb" },
	{ 0x59700c43, "vb2_ioctl_streamoff" },
	{ 0xd710adbf, "__kmalloc_noprof" },
	{ 0x540509f0, "vb2_ioctl_expbuf" },
	{ 0x40a621c5, "snprintf" },
	{ 0x99f38a35, "vb2_ops_wait_finish" },
	{ 0xadb55ac9, "usb_register_driver" },
	{ 0xa53f4e29, "memcpy" },
	{ 0xcb8b6ec6, "kfree" },
	{ 0xdda4665c, "vb2_ioctl_querybuf" },
	{ 0x2c90d796, "v4l2_fh_open" },
	{ 0xe1e1f979, "_raw_spin_lock_irqsave" },
	{ 0xbe011736, "__dynamic_dev_dbg" },
	{ 0xd272d446, "__fentry__" },
	{ 0xe8213e80, "_printk" },
	{ 0xbd03ed67, "__ref_stack_chk_guard" },
	{ 0xff7fbdd1, "___ratelimit" },
	{ 0xa31fd687, "usb_put_dev" },
	{ 0xd272d446, "__stack_chk_fail" },
	{ 0xa1a295d0, "video_devdata" },
	{ 0xd710adbf, "__kmalloc_large_noprof" },
	{ 0x5501f767, "vb2_fop_release" },
	{ 0x37602407, "usb_get_dev" },
	{ 0x8c18c8e2, "usb_submit_urb" },
	{ 0x9b1de7cb, "_dev_info" },
	{ 0x90a48d82, "__ubsan_handle_out_of_bounds" },
	{ 0xdda4665c, "vb2_ioctl_dqbuf" },
	{ 0x9b1de7cb, "_dev_err" },
	{ 0xa9d55ea2, "vb2_ioctl_create_bufs" },
	{ 0xdda4665c, "vb2_ioctl_prepare_buf" },
	{ 0xbd03ed67, "random_kmalloc_seed" },
	{ 0x6c00f410, "usb_control_msg" },
	{ 0x7a7304c6, "vb2_buffer_done" },
	{ 0xc5823ea1, "vb2_plane_vaddr" },
	{ 0xc98698bb, "usb_set_interface" },
	{ 0xefea9c9e, "video_unregister_device" },
	{ 0xc1e6c71e, "__mutex_init" },
	{ 0xa8f96c6e, "usb_deregister" },
	{ 0xdda4665c, "vb2_ioctl_qbuf" },
	{ 0x2eca2015, "vb2_fop_mmap" },
	{ 0x81a1a811, "_raw_spin_unlock_irqrestore" },
	{ 0xd0dc4385, "vb2_vmalloc_memops" },
	{ 0xd272d446, "__x86_return_thunk" },
	{ 0xa2634152, "vb2_fop_read" },
	{ 0x385e7f25, "v4l2_device_register" },
	{ 0x2644068d, "__video_register_device" },
	{ 0x99f38a35, "vb2_ops_wait_prepare" },
	{ 0xc064623f, "__kmalloc_cache_noprof" },
	{ 0x0c8e9a2d, "usb_kill_urb" },
	{ 0x97acb853, "ktime_get" },
	{ 0x59700c43, "vb2_ioctl_streamon" },
	{ 0xa59ab014, "vb2_fop_poll" },
	{ 0x8400cfbf, "v4l2_device_unregister" },
	{ 0xe4de56b4, "__ubsan_handle_load_invalid_value" },
	{ 0xefea9c9e, "video_device_release_empty" },
	{ 0xfaabfe5e, "kmalloc_caches" },
	{ 0x4d023d2b, "vb2_ioctl_reqbufs" },
	{ 0xbebe66ff, "module_layout" },
};

static const u32 ____version_ext_crcs[]
__used __section("__version_ext_crcs") = {
	0x31c0f9b0,
	0x3fdc3369,
	0x08552149,
	0x0c8e9a2d,
	0x59700c43,
	0xd710adbf,
	0x540509f0,
	0x40a621c5,
	0x99f38a35,
	0xadb55ac9,
	0xa53f4e29,
	0xcb8b6ec6,
	0xdda4665c,
	0x2c90d796,
	0xe1e1f979,
	0xbe011736,
	0xd272d446,
	0xe8213e80,
	0xbd03ed67,
	0xff7fbdd1,
	0xa31fd687,
	0xd272d446,
	0xa1a295d0,
	0xd710adbf,
	0x5501f767,
	0x37602407,
	0x8c18c8e2,
	0x9b1de7cb,
	0x90a48d82,
	0xdda4665c,
	0x9b1de7cb,
	0xa9d55ea2,
	0xdda4665c,
	0xbd03ed67,
	0x6c00f410,
	0x7a7304c6,
	0xc5823ea1,
	0xc98698bb,
	0xefea9c9e,
	0xc1e6c71e,
	0xa8f96c6e,
	0xdda4665c,
	0x2eca2015,
	0x81a1a811,
	0xd0dc4385,
	0xd272d446,
	0xa2634152,
	0x385e7f25,
	0x2644068d,
	0x99f38a35,
	0xc064623f,
	0x0c8e9a2d,
	0x97acb853,
	0x59700c43,
	0xa59ab014,
	0x8400cfbf,
	0xe4de56b4,
	0xefea9c9e,
	0xfaabfe5e,
	0x4d023d2b,
	0xbebe66ff,
};
static const char ____version_ext_names[]
__used __section("__version_ext_names") =
	"vb2_queue_init\0"
	"usb_alloc_urb\0"
	"video_ioctl2\0"
	"usb_free_urb\0"
	"vb2_ioctl_streamoff\0"
	"__kmalloc_noprof\0"
	"vb2_ioctl_expbuf\0"
	"snprintf\0"
	"vb2_ops_wait_finish\0"
	"usb_register_driver\0"
	"memcpy\0"
	"kfree\0"
	"vb2_ioctl_querybuf\0"
	"v4l2_fh_open\0"
	"_raw_spin_lock_irqsave\0"
	"__dynamic_dev_dbg\0"
	"__fentry__\0"
	"_printk\0"
	"__ref_stack_chk_guard\0"
	"___ratelimit\0"
	"usb_put_dev\0"
	"__stack_chk_fail\0"
	"video_devdata\0"
	"__kmalloc_large_noprof\0"
	"vb2_fop_release\0"
	"usb_get_dev\0"
	"usb_submit_urb\0"
	"_dev_info\0"
	"__ubsan_handle_out_of_bounds\0"
	"vb2_ioctl_dqbuf\0"
	"_dev_err\0"
	"vb2_ioctl_create_bufs\0"
	"vb2_ioctl_prepare_buf\0"
	"random_kmalloc_seed\0"
	"usb_control_msg\0"
	"vb2_buffer_done\0"
	"vb2_plane_vaddr\0"
	"usb_set_interface\0"
	"video_unregister_device\0"
	"__mutex_init\0"
	"usb_deregister\0"
	"vb2_ioctl_qbuf\0"
	"vb2_fop_mmap\0"
	"_raw_spin_unlock_irqrestore\0"
	"vb2_vmalloc_memops\0"
	"__x86_return_thunk\0"
	"vb2_fop_read\0"
	"v4l2_device_register\0"
	"__video_register_device\0"
	"vb2_ops_wait_prepare\0"
	"__kmalloc_cache_noprof\0"
	"usb_kill_urb\0"
	"ktime_get\0"
	"vb2_ioctl_streamon\0"
	"vb2_fop_poll\0"
	"v4l2_device_unregister\0"
	"__ubsan_handle_load_invalid_value\0"
	"video_device_release_empty\0"
	"kmalloc_caches\0"
	"vb2_ioctl_reqbufs\0"
	"module_layout\0"
;

MODULE_INFO(depends, "videobuf2-v4l2,videodev,videobuf2-common,videobuf2-vmalloc");

MODULE_ALIAS("usb:v046Dp0825d*dc*dsc*dp*ic*isc*ip*in*");

MODULE_INFO(srcversion, "2E78AB3A4FD4031C185BB55");
