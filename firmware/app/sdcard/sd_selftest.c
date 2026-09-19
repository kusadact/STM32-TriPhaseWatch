#include "sd_selftest.h"

#include <string.h>

int sd_selftest_append(const char *path, const char *line, sd_selftest_result_t *result)
{
  FIL file;
  FRESULT fr;
  UINT written = 0u;
  UINT read_back = 0u;
  size_t len;
  uint8_t buffer[SD_SELFTEST_MAX_LINE];

  if ((path == NULL) || (line == NULL) || (result == NULL)) {
    return -1;
  }

  memset(result, 0, sizeof(*result));
  result->fr = FR_INVALID_PARAMETER;

  len = strlen(line);
  if ((len == 0u) || (len > SD_SELFTEST_MAX_LINE)) {
    return -1;
  }

  fr = f_open(&file, path, FA_OPEN_ALWAYS | FA_WRITE);
  if (fr != FR_OK) {
    result->fr = fr;
    return -1;
  }
  result->stage = SD_SELFTEST_STAGE_OPEN;
  result->size_before = f_size(&file);

  fr = f_lseek(&file, result->size_before);
  if (fr != FR_OK) {
    result->fr = fr;
    (void)f_close(&file);
    return -1;
  }

  fr = f_write(&file, line, (UINT)len, &written);
  if ((fr != FR_OK) || (written != (UINT)len)) {
    result->fr = (fr != FR_OK) ? fr : FR_INT_ERR;
    (void)f_close(&file);
    return -1;
  }
  result->stage = SD_SELFTEST_STAGE_WRITE;
  result->written = (uint16_t)written;

  fr = f_sync(&file);
  if (fr != FR_OK) {
    result->fr = fr;
    (void)f_close(&file);
    return -1;
  }
  result->stage = SD_SELFTEST_STAGE_SYNC;
  result->size_after = f_size(&file);

  fr = f_close(&file);
  if (fr != FR_OK) {
    result->fr = fr;
    return -1;
  }

  fr = f_open(&file, path, FA_READ);
  if (fr != FR_OK) {
    result->fr = fr;
    return -1;
  }
  result->stage = SD_SELFTEST_STAGE_REOPEN;

  if (result->size_after < (DWORD)len) {
    result->fr = FR_INT_ERR;
    (void)f_close(&file);
    return -1;
  }

  fr = f_lseek(&file, result->size_after - (DWORD)len);
  if (fr != FR_OK) {
    result->fr = fr;
    (void)f_close(&file);
    return -1;
  }

  fr = f_read(&file, buffer, (UINT)len, &read_back);
  if ((fr != FR_OK) || (read_back != (UINT)len)) {
    result->fr = (fr != FR_OK) ? fr : FR_INT_ERR;
    (void)f_close(&file);
    return -1;
  }
  result->stage = SD_SELFTEST_STAGE_READ;
  result->read_back = (uint16_t)read_back;
  (void)f_close(&file);

  result->verified = (memcmp(buffer, line, len) == 0u) ? 1u : 0u;
  result->stage = SD_SELFTEST_STAGE_COMPARE;
  result->fr = FR_OK;

  return (result->verified != 0u) ? 0 : -1;
}
