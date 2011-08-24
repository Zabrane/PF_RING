#include <linux/module.h>
#include <linux/vermagic.h>
#include <linux/compiler.h>

MODULE_INFO(vermagic, VERMAGIC_STRING);

struct module __this_module
__attribute__((section(".gnu.linkonce.this_module"))) = {
 .name = KBUILD_MODNAME,
 .init = init_module,
#ifdef CONFIG_MODULE_UNLOAD
 .exit = cleanup_module,
#endif
 .arch = MODULE_ARCH_INIT,
};

static const struct modversion_info ____versions[]
__used
__attribute__((section("__versions"))) = {
	{ 0xca5bc4d1, "module_layout" },
	{ 0x12da5bb2, "__kmalloc" },
	{ 0xf9a482f9, "msleep" },
	{ 0x603a062a, "pci_enable_sriov" },
	{ 0x528c709d, "simple_read_from_buffer" },
	{ 0xc3222357, "generic_file_llseek" },
	{ 0xb04cbe4d, "debugfs_create_dir" },
	{ 0x64cd003a, "mem_map" },
	{ 0xd6ee688f, "vmalloc" },
	{ 0xb85f3bbe, "pv_lock_ops" },
	{ 0xcd2e1a61, "single_open" },
	{ 0xa90c928a, "param_ops_int" },
	{ 0x1b51953e, "page_address" },
	{ 0xba27cea9, "dev_set_drvdata" },
	{ 0x8c01daf6, "seq_open" },
	{ 0x496c1584, "seq_release_private" },
	{ 0x2d37342e, "cpu_online_mask" },
	{ 0xb59cd00a, "dma_set_mask" },
	{ 0xddb27d9a, "dev_printk" },
	{ 0xbb55dcca, "single_release" },
	{ 0x5e0c4105, "pci_get_slot" },
	{ 0x2c4b8b06, "napi_complete" },
	{ 0x14ecdc6f, "seq_puts" },
	{ 0x246fcd96, "pci_disable_device" },
	{ 0xf88a41ba, "pci_disable_msix" },
	{ 0xd553b02, "netif_carrier_on" },
	{ 0x4a3fd066, "pci_disable_sriov" },
	{ 0x20000329, "simple_strtoul" },
	{ 0xb6a5a19b, "seq_printf" },
	{ 0xa40a665d, "netif_carrier_off" },
	{ 0x9e1ce7b9, "remove_proc_entry" },
	{ 0x6ad6e329, "x86_dma_fallback_dev" },
	{ 0xeae3dfd6, "__const_udelay" },
	{ 0x61df2020, "pci_release_regions" },
	{ 0x677bb305, "param_ops_bool" },
	{ 0x9e1bdc28, "init_timer_key" },
	{ 0x6baae653, "cancel_delayed_work_sync" },
	{ 0x4df0270, "mutex_unlock" },
	{ 0x85df9b6c, "strsep" },
	{ 0x999e8297, "vfree" },
	{ 0x167e7f9d, "__get_user_1" },
	{ 0x9a10f02a, "pci_bus_write_config_word" },
	{ 0x10d122f3, "debugfs_create_file" },
	{ 0x47c7b0d2, "cpu_number" },
	{ 0xa51cdfe8, "__FIXADDR_TOP" },
	{ 0x3c2c5af5, "sprintf" },
	{ 0x2d9a1efa, "debugfs_remove_recursive" },
	{ 0x81a8ead9, "seq_read" },
	{ 0x17fad347, "__alloc_pages_nodemask" },
	{ 0x268e0664, "netif_napi_del" },
	{ 0x7d11c268, "jiffies" },
	{ 0xe2d5255a, "strcmp" },
	{ 0x817fcc7c, "pci_reset_function" },
	{ 0xf2b55a7f, "vlan_gro_frags" },
	{ 0x644f1abc, "pci_set_master" },
	{ 0xe1bc7ede, "del_timer_sync" },
	{ 0x2bc95bd4, "memset" },
	{ 0x1e17ab0b, "pci_enable_pcie_error_reporting" },
	{ 0x924fcf17, "proc_mkdir" },
	{ 0xe3e458fd, "pci_enable_msix" },
	{ 0x11089ac7, "_ctype" },
	{ 0xad9bd82c, "dev_err" },
	{ 0xaeb89fd, "__seq_open_private" },
	{ 0xfc65c85b, "__mutex_init" },
	{ 0xa2f5ca2a, "ethtool_op_get_link" },
	{ 0x50eedeb8, "printk" },
	{ 0x42224298, "sscanf" },
	{ 0xf609aa30, "_raw_spin_trylock" },
	{ 0x5152e605, "memcmp" },
	{ 0x23269a13, "strict_strtoul" },
	{ 0xdd61eb4, "free_netdev" },
	{ 0xfaef0ed, "__tasklet_schedule" },
	{ 0xa1c76e0a, "_cond_resched" },
	{ 0x2f287f0d, "copy_to_user" },
	{ 0xdaba1141, "register_netdev" },
	{ 0xb4390f9a, "mcount" },
	{ 0x4de15a60, "netif_receive_skb" },
	{ 0xa0d42ff7, "seq_putc" },
	{ 0x6c2e3320, "strncmp" },
	{ 0xae298814, "debugfs_remove" },
	{ 0x16305289, "warn_slowpath_null" },
	{ 0xe674acfc, "pci_bus_write_config_dword" },
	{ 0xc637b470, "mutex_lock" },
	{ 0x4c1182cb, "bitmap_scnprintf" },
	{ 0x9545af6d, "tasklet_init" },
	{ 0xce095088, "mod_timer" },
	{ 0xa7e255, "netif_napi_add" },
	{ 0xbfcacb59, "dma_release_from_coherent" },
	{ 0xfda85a7d, "request_threaded_irq" },
	{ 0x3c9d1211, "string_get_size" },
	{ 0x310917fe, "sort" },
	{ 0x1ed1de09, "dev_kfree_skb_any" },
	{ 0xdb7d2224, "contig_page_data" },
	{ 0x9fa25542, "dma_alloc_from_coherent" },
	{ 0x5c1b4e08, "pci_find_capability" },
	{ 0x82072614, "tasklet_kill" },
	{ 0x7dceceac, "capable" },
	{ 0x3ff62317, "local_bh_disable" },
	{ 0xd3fa7a74, "_dev_info" },
	{ 0xe5eec0e, "__free_pages" },
	{ 0x12c9bcd1, "__alloc_skb" },
	{ 0xc00918ed, "pci_bus_read_config_word" },
	{ 0xa9fc140a, "ethtool_op_set_sg" },
	{ 0x1b134b94, "__napi_schedule" },
	{ 0x617c151e, "pci_bus_read_config_dword" },
	{ 0xf0fdf6cb, "__stack_chk_fail" },
	{ 0x77edf722, "schedule_delayed_work" },
	{ 0x86a4889a, "kmalloc_order_trace" },
	{ 0xce5f7e19, "kfree_skb" },
	{ 0x8a7d1c31, "high_memory" },
	{ 0x86fb9b05, "bitmap_parse_user" },
	{ 0x799aca4, "local_bh_enable" },
	{ 0x6fc2a1f0, "eth_type_trans" },
	{ 0xcec9c2a4, "pci_unregister_driver" },
	{ 0x6443d74d, "_raw_spin_lock" },
	{ 0x152cbc7b, "pci_ioremap_bar" },
	{ 0x31b599ba, "proc_create_data" },
	{ 0xed95ea5d, "eth_validate_addr" },
	{ 0x39ed4574, "seq_lseek" },
	{ 0x37a0cba, "kfree" },
	{ 0x2e60bace, "memcpy" },
	{ 0xf0cc809, "pci_request_regions" },
	{ 0xd1329880, "param_array_ops" },
	{ 0x9ea3450f, "pci_disable_msi" },
	{ 0x4db7b6a8, "dma_supported" },
	{ 0xcdf7a17d, "dev_alert" },
	{ 0x6128b5fc, "__printk_ratelimit" },
	{ 0xedc03953, "iounmap" },
	{ 0xe726fb17, "__pci_register_driver" },
	{ 0xe3b22412, "put_page" },
	{ 0x374ed073, "scnprintf" },
	{ 0x50716588, "request_firmware" },
	{ 0x74c134b9, "__sw_hweight32" },
	{ 0x3566b87, "dev_warn" },
	{ 0x590f043e, "napi_get_frags" },
	{ 0xca212ed, "unregister_netdev" },
	{ 0xd315cf80, "pci_dev_put" },
	{ 0x701d0ebd, "snprintf" },
	{ 0x1bbc70f7, "pci_enable_msi_block" },
	{ 0x9b1714fd, "seq_release" },
	{ 0xdf58c688, "__netif_schedule" },
	{ 0x7b4d76b1, "consume_skb" },
	{ 0x77bc13a0, "strim" },
	{ 0xd6eb9777, "skb_put" },
	{ 0x45850bef, "pci_enable_device" },
	{ 0x362ef408, "_copy_from_user" },
	{ 0x13095525, "param_ops_uint" },
	{ 0xa11820cb, "skb_copy_bits" },
	{ 0x6678d8f4, "dev_get_drvdata" },
	{ 0xca3b9830, "release_firmware" },
	{ 0x9e7d6bd0, "__udelay" },
	{ 0x8605bb92, "ethtool_op_set_tx_ipv6_csum" },
	{ 0xbdb53c5a, "dma_ops" },
	{ 0x8fa83077, "napi_gro_frags" },
	{ 0xf20dabd8, "free_irq" },
	{ 0x5b45d324, "__vlan_hwaccel_rx" },
	{ 0xe914e41e, "strcpy" },
	{ 0x5ef6a0f0, "alloc_etherdev_mqs" },
};

static const char __module_depends[]
__used
__attribute__((section(".modinfo"))) =
"depends=";

MODULE_ALIAS("pci:v00001425d0000A000sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004400sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004401sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004402sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004403sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004404sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004405sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004406sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004407sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004408sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d00004409sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00001425d0000440Asv*sd*bc*sc*i*");

MODULE_INFO(srcversion, "D0D1E047135E5FC1501E8AB");
