/* Minimal static libudev stub for riscv64 cross-builds where no static
 * libudev.a exists (Debian/Ubuntu's libudev-dev only ships the .so).
 * mujina-miner links libudev for real USB ASIC board enumeration, which
 * this deployment never uses (only the virtual "nano3s" board, gated by
 * MUJINA_NANO3S_ENABLE) -- every function here returns libudev's own
 * documented failure value (NULL/-1/0), matching what a real udev
 * subsystem legitimately returns when unavailable (e.g. no /run/udev),
 * so any correctly-written binding already handles these paths without
 * crashing. Symbol list taken from libudev-sys 0.1.4's extern "C" block.
 * NOT a real udev implementation -- do not rely on this for any board
 * that actually needs USB enumeration. */

typedef void *udev_ptr;

#define PTR_STUB(name) udev_ptr name(void) { return (udev_ptr)0; }
#define INT_STUB(name) int name(void) { return -1; }
#define VOID_STUB(name) void name(void) { }
#define ULL_STUB(name) unsigned long long name(void) { return 0; }

/* struct udev * */
PTR_STUB(udev_ref)
VOID_STUB(udev_unref)
PTR_STUB(udev_new)
VOID_STUB(udev_set_userdata)
PTR_STUB(udev_get_userdata)

/* struct udev_list_entry * */
PTR_STUB(udev_list_entry_get_next)
PTR_STUB(udev_list_entry_get_by_name)
PTR_STUB(udev_list_entry_get_name)
PTR_STUB(udev_list_entry_get_value)

/* struct udev_device * */
PTR_STUB(udev_device_ref)
VOID_STUB(udev_device_unref)
PTR_STUB(udev_device_get_udev)
PTR_STUB(udev_device_new_from_syspath)
PTR_STUB(udev_device_new_from_devnum)
PTR_STUB(udev_device_new_from_device_id)
PTR_STUB(udev_device_new_from_subsystem_sysname)
PTR_STUB(udev_device_new_from_environment)
PTR_STUB(udev_device_get_parent)
PTR_STUB(udev_device_get_parent_with_subsystem_devtype)
PTR_STUB(udev_device_get_devpath)
PTR_STUB(udev_device_get_subsystem)
PTR_STUB(udev_device_get_devtype)
PTR_STUB(udev_device_get_syspath)
PTR_STUB(udev_device_get_sysname)
PTR_STUB(udev_device_get_sysnum)
PTR_STUB(udev_device_get_devnode)
INT_STUB(udev_device_get_is_initialized)
PTR_STUB(udev_device_get_devlinks_list_entry)
PTR_STUB(udev_device_get_properties_list_entry)
PTR_STUB(udev_device_get_property_value)
PTR_STUB(udev_device_get_tags_list_entry)
INT_STUB(udev_device_has_tag)
PTR_STUB(udev_device_get_sysattr_value)
INT_STUB(udev_device_set_sysattr_value)
PTR_STUB(udev_device_get_sysattr_list_entry)
ULL_STUB(udev_device_get_devnum)
PTR_STUB(udev_device_get_driver)
ULL_STUB(udev_device_get_seqnum)
ULL_STUB(udev_device_get_usec_since_initialized)
PTR_STUB(udev_device_get_action)

/* struct udev_enumerate * */
PTR_STUB(udev_enumerate_ref)
VOID_STUB(udev_enumerate_unref)
PTR_STUB(udev_enumerate_get_udev)
PTR_STUB(udev_enumerate_new)
INT_STUB(udev_enumerate_add_match_subsystem)
INT_STUB(udev_enumerate_add_nomatch_subsystem)
INT_STUB(udev_enumerate_add_match_sysattr)
INT_STUB(udev_enumerate_add_nomatch_sysattr)
INT_STUB(udev_enumerate_add_match_property)
INT_STUB(udev_enumerate_add_match_sysname)
INT_STUB(udev_enumerate_add_match_tag)
INT_STUB(udev_enumerate_add_match_parent)
INT_STUB(udev_enumerate_add_match_is_initialized)
INT_STUB(udev_enumerate_add_syspath)
INT_STUB(udev_enumerate_scan_devices)
INT_STUB(udev_enumerate_scan_subsystems)
PTR_STUB(udev_enumerate_get_list_entry)

/* struct udev_monitor * */
PTR_STUB(udev_monitor_ref)
VOID_STUB(udev_monitor_unref)
PTR_STUB(udev_monitor_get_udev)
PTR_STUB(udev_monitor_new_from_netlink)
INT_STUB(udev_monitor_enable_receiving)
INT_STUB(udev_monitor_set_receive_buffer_size)
INT_STUB(udev_monitor_get_fd)
PTR_STUB(udev_monitor_receive_device)
INT_STUB(udev_monitor_filter_add_match_subsystem_devtype)
INT_STUB(udev_monitor_filter_add_match_tag)
INT_STUB(udev_monitor_filter_update)
INT_STUB(udev_monitor_filter_remove)

/* struct udev_queue * */
PTR_STUB(udev_queue_ref)
VOID_STUB(udev_queue_unref)
PTR_STUB(udev_queue_get_udev)
PTR_STUB(udev_queue_new)
INT_STUB(udev_queue_get_udev_is_active)
INT_STUB(udev_queue_get_queue_is_empty)
INT_STUB(udev_queue_get_fd)
INT_STUB(udev_queue_flush)

/* misc */
INT_STUB(udev_util_encode_string)
