#ifndef PROTONOS_CONFIG_H
#define PROTONOS_CONFIG_H

#include <stdint.h>

/* System settings - what the machine remembers about itself
 *
 * Everything the system knew about itself was until now written into
 * it at build time. The name of a DNS server could be changed with a
 * command, and the change lived exactly as long as the machine did.
 * Turning it off was enough to forget.
 *
 * A setting is a line in a file on the disk: name=value, and nothing
 * more. Plain text on purpose - it can be read with 'cat', changed
 * with 'write', and it is obvious what happened to it. The kernel
 * reads that file once, at boot, right after the file system comes up.
 *
 * The set of settings is fixed and explicit, and that is deliberate:
 * every one of them has somewhere in the system a place that reads it
 * and does something with it. A name nobody reads is not a setting,
 * it is a line in a file.
 */

/* The file on the disk. Text, one setting per line. */
#define CONFIG_FILE "system.cfg"

#define CONFIG_VAL_MAX 32

/* Reads the settings from the disk. Whatever is not there keeps its
   default, so a machine with no disk works exactly as before. Called
   after fs_init. */
void config_load(void);

/* The value of a setting, or its default - never NULL for a known
   name, always NULL for an unknown one. */
const char *config_get(const char *key);

/* The same as a number (a decimal, or an IPv4 address in the form
   a.b.c.d when the setting is one). 'fallback' when there is no such
   setting. */
uint32_t config_get_num(const char *key, uint32_t fallback);

/* Changes a setting and writes ALL of them back to the disk. Returns 0
   on success, -1 for an unknown name, a value that is too long, or a
   disk that will not take it.

   The whole file is written, not one line: the settings are a handful
   of short strings, and rewriting them whole means there is no way to
   end up with a file half from before and half from now. */
int config_set(const char *key, const char *value);

/* Walking the settings, for whoever wants to show them all. Returns 0
   past the last one. */
int config_count(void);
const char *config_name(int i);
const char *config_value(int i);
const char *config_help(int i);

#endif
