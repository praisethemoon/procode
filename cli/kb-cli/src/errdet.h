/* The `details` object of an error (index-api §11).
 *
 * The code says what went wrong; the details say what a caller needs in order
 * to act on it without parsing the message: how many documents a collection
 * still holds, which process holds the lock, which structure is stale. The
 * message stays prose for a person to read.
 *
 * ONE PENDING OBJECT PER PROCESS. kb reports at most one error and exits, and
 * the layer that knows a detail is often below the one that reports: the lock
 * holder is known in store_open, the error is printed by the command. So the
 * lower layer adds fields here and err_out prints them. Fields are
 * built into a fixed buffer; one that does not fit is dropped whole, so the
 * object is always valid JSON.
 *
 * THE FIELDS BELONG TO A CODE. A lower layer can record details for an error
 * its caller then recovers from, and the next error reported would carry
 * fields about something else. So errdet_begin names the code, and err_out
 * prints the fields only under that code.
 */
#ifndef KB_ERRDET_H
#define KB_ERRDET_H

#include <stddef.h>
#include <stdint.h>

/* Discards whatever was pending and starts the details of `code`. */
void errdet_begin(const char *code);
void errdet_str(const char *key, const char *value);
void errdet_int(const char *key, int64_t value);
void errdet_strs(const char *key, const char *const *values, size_t n);
/* A value that is already JSON — an object the caller built. Taken as is. */
void errdet_raw(const char *key, const char *json);

/* "{...}" with the fields added since errdet_begin(code), or NULL when there
 * are none for that code. Valid until the next errdet_* call. */
const char *errdet_json(const char *code);

#endif
