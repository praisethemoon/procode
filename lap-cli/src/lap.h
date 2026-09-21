/* lap - fine-grained edit recorder for AI agents.
 * Common definitions shared by every translation unit.
 */
#ifndef LAP_H
#define LAP_H

#if defined(_MSC_VER)
#define _CRT_SECURE_NO_WARNINGS 1
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LAP_VERSION "0.1.0"

#define LAP_DIR ".lap"
#define LAP_LOG_NAME "log.jsonl"
#define LAP_STATE_NAME "state.json"
#define LAP_SHADOW_NAME "shadow"
#define LAP_LOCK_NAME "lock"
#define LAP_IGNORE_NAME ".lapignore"

#define LAP_PATH_MAX 4096
#define LAP_MAX_FILE_SIZE (64u * 1024u * 1024u) /* refuse files larger than 64 MB */

/* Exit codes: 0 success, 1 user/repo error, 2 internal error (OOM, I/O corruption). */
#define LAP_EXIT_OK 0
#define LAP_EXIT_ERR 1
#define LAP_EXIT_FATAL 2

#endif /* LAP_H */
