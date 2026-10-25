/*
 * scePad API Hooks
 * Intercepts PS4 controller API calls to inject Xbox controller input
 */

#ifndef HOOKS_H
#define HOOKS_H

/*
 * Configure current process identity (from sys_sdk_proc_info)
 */
void hooks_set_process_info(int pid, int is_shellui);

/*
 * Initialize USB & IPC subsystem for Xbox controller
 * Call ONCE from plugin_load, before hooks_install
 * @return 0 on success, negative on error (non-fatal)
 */
int hooks_init_usb(void);

/*
 * Install all scePad hooks
 * @return 0 on success, negative on error
 */
int hooks_install(void);

/*
 * Remove all scePad hooks
 */
void hooks_remove(void);

/*
 * Check if a handle is a virtual Xbox controller
 * @param handle    scePad handle value
 * @return 1 if virtual, 0 if physical
 */
int hooks_is_virtual_handle(int handle);

/*
 * Get Xbox controller index for a virtual handle
 * @param handle    Virtual scePad handle
 * @return Controller index (0-3), or -1 if invalid
 */
int hooks_handle_to_index(int handle);

#endif // HOOKS_H
