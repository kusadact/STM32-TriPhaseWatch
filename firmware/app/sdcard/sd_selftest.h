#ifndef SD_SELFTEST_H
#define SD_SELFTEST_H

#include <stdint.h>

#include "ff.h"

#define SD_SELFTEST_MAX_LINE 200u

typedef enum {
  SD_SELFTEST_STAGE_NONE = 0u,
  SD_SELFTEST_STAGE_OPEN,
  SD_SELFTEST_STAGE_WRITE,
  SD_SELFTEST_STAGE_SYNC,
  SD_SELFTEST_STAGE_REOPEN,
  SD_SELFTEST_STAGE_READ,
  SD_SELFTEST_STAGE_COMPARE
} sd_selftest_stage_t;

typedef struct {
  FRESULT fr;
  uint8_t stage;
  uint8_t verified;
  uint32_t size_before;
  uint32_t size_after;
  uint16_t written;
  uint16_t read_back;
} sd_selftest_result_t;

/*
 * Appends one line to path and verifies it by reading the same byte range
 * back. Opening uses FA_OPEN_ALWAYS, so existing files are appended to and
 * never truncated; no other file or raw sector is touched.
 *
 * Returns 0 when the written bytes were read back identical, -1 otherwise
 * (details in result).
 */
int sd_selftest_append(const char *path, const char *line, sd_selftest_result_t *result);

#endif /* SD_SELFTEST_H */
