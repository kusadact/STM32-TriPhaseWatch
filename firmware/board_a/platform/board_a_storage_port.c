#include "board_a_storage_port.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "ff.h"
#include "sd_spi.h"
#include "stm32f4xx.h"
#include "task.h"

static FATFS g_storage_fs;
static FIL g_storage_file;
static uint8_t g_storage_mounted;

static uint32_t storage_port_now_ms(void *context)
{
  (void)context;
  return (uint32_t)(TIM2->CNT / 1000U);
}

static void storage_port_deadline_begin(uint32_t deadline_ms)
{
  sd_spi_set_deadline(storage_port_now_ms, NULL, deadline_ms);
}

static void storage_port_unmount(void)
{
  (void)f_mount(NULL, "0:", 0U);
  g_storage_mounted = 0U;
}

static board_a_storage_error_t storage_port_map_fresult(FRESULT result)
{
  switch (result) {
    case FR_OK:
      return BOARD_A_STORAGE_ERROR_NONE;
    case FR_NOT_READY:
    case FR_NOT_ENABLED:
    case FR_NO_FILESYSTEM:
    case FR_DISK_ERR:
      return BOARD_A_STORAGE_ERROR_MOUNT;
    case FR_DENIED:
      return BOARD_A_STORAGE_ERROR_FULL;
    case FR_TIMEOUT:
      return BOARD_A_STORAGE_ERROR_TIMEOUT;
    default:
      return BOARD_A_STORAGE_ERROR_CREATE;
  }
}

static board_a_storage_io_result_t storage_port_map_error(
    board_a_storage_error_t error)
{
  switch (error) {
    case BOARD_A_STORAGE_ERROR_MOUNT:
      return BOARD_A_STORAGE_IO_NOT_READY;
    case BOARD_A_STORAGE_ERROR_FULL:
      return BOARD_A_STORAGE_IO_FULL;
    case BOARD_A_STORAGE_ERROR_TIMEOUT:
      return BOARD_A_STORAGE_IO_TIMEOUT;
    default:
      return BOARD_A_STORAGE_IO_OTHER;
  }
}

static board_a_storage_io_result_t storage_port_ensure_dirs(
    uint32_t file_date)
{
  FRESULT result;

  result = f_mkdir("0:/LOG");
  if ((result != FR_OK) && (result != FR_EXIST)) {
    return storage_port_map_error(storage_port_map_fresult(result));
  }

  if (file_date != 0U) {
    char path[24] = "0:/LOG/00000000";

    path[7] = (char)('0' + ((file_date / 10000000U) % 10U));
    path[8] = (char)('0' + ((file_date / 1000000U) % 10U));
    path[9] = (char)('0' + ((file_date / 100000U) % 10U));
    path[10] = (char)('0' + ((file_date / 10000U) % 10U));
    path[11] = (char)('0' + ((file_date / 1000U) % 10U));
    path[12] = (char)('0' + ((file_date / 100U) % 10U));
    path[13] = (char)('0' + ((file_date / 10U) % 10U));
    path[14] = (char)('0' + (file_date % 10U));
    result = f_mkdir(path);
    if ((result != FR_OK) && (result != FR_EXIST)) {
      return storage_port_map_error(storage_port_map_fresult(result));
    }
  } else {
    result = f_mkdir("0:/LOG/UNSET");
    if ((result != FR_OK) && (result != FR_EXIST)) {
      return storage_port_map_error(storage_port_map_fresult(result));
    }
  }

  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_port_probe(
    void *context, uint32_t deadline_ms, uint32_t *raw_error)
{
  FRESULT result;

  (void)context;
  if (raw_error == NULL) {
    return BOARD_A_STORAGE_IO_OTHER;
  }
  *raw_error = 0U;
  storage_port_deadline_begin(deadline_ms);
  if (g_storage_mounted == 0U) {
    memset(&g_storage_file, 0, sizeof(g_storage_file));
    result = f_mount(&g_storage_fs, "0:", 1U);
    *raw_error = (uint32_t)result;
    if (result != FR_OK) {
      return storage_port_map_error(storage_port_map_fresult(result));
    }
    g_storage_mounted = 1U;
  }

  result = f_mkdir("0:/LOG");
  *raw_error = (uint32_t)result;
  if ((result != FR_OK) && (result != FR_EXIST)) {
    board_a_storage_io_result_t mapped =
        storage_port_map_error(storage_port_map_fresult(result));
    storage_port_unmount();
    return mapped;
  }
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_port_open_new(
    void *context, uint32_t file_date, uint32_t file_id,
    uint32_t deadline_ms, void **handle, uint32_t *raw_error)
{
  board_a_storage_io_result_t ensure_result;
  char path[BOARD_A_RECORD_PATH_MAX_BYTES];
  FRESULT result;

  (void)context;
  if ((handle == NULL) || (raw_error == NULL)) {
    return BOARD_A_STORAGE_IO_OTHER;
  }
  *handle = NULL;
  *raw_error = 0U;
  storage_port_deadline_begin(deadline_ms);

  if (g_storage_mounted == 0U) {
    result = f_mount(&g_storage_fs, "0:", 1U);
    *raw_error = (uint32_t)result;
    if (result != FR_OK) {
      return storage_port_map_error(storage_port_map_fresult(result));
    }
    g_storage_mounted = 1U;
  }

  ensure_result = storage_port_ensure_dirs(file_date);
  if (ensure_result != BOARD_A_STORAGE_IO_OK) {
    if (ensure_result == BOARD_A_STORAGE_IO_NOT_READY) {
      storage_port_unmount();
    }
    return ensure_result;
  }

  if (!board_a_record_format_make_path(path, sizeof(path), file_date,
                                       file_id)) {
    return BOARD_A_STORAGE_IO_OTHER;
  }

  memset(&g_storage_file, 0, sizeof(g_storage_file));
  result = f_open(&g_storage_file, path, FA_CREATE_NEW | FA_WRITE);
  *raw_error = (uint32_t)result;
  if (result == FR_EXIST) {
    return BOARD_A_STORAGE_IO_EXISTS;
  }
  if (result != FR_OK) {
    board_a_storage_io_result_t mapped =
        storage_port_map_error(storage_port_map_fresult(result));
    storage_port_unmount();
    return mapped;
  }

  *handle = &g_storage_file;
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_port_write(
    void *context, void *handle, const uint8_t *data, uint16_t length,
    uint32_t deadline_ms, uint16_t *written, uint32_t *raw_error)
{
  UINT write_count = 0U;
  FRESULT result;

  (void)context;
  if ((handle == NULL) || (data == NULL) || (written == NULL) ||
      (raw_error == NULL)) {
    return BOARD_A_STORAGE_IO_OTHER;
  }
  storage_port_deadline_begin(deadline_ms);
  result = f_write((FIL *)handle, data, length, &write_count);
  *raw_error = (uint32_t)result;
  *written = (uint16_t)write_count;
  if (result != FR_OK) {
    g_storage_mounted = 0U;
    if (result == FR_DENIED) {
      return BOARD_A_STORAGE_IO_FULL;
    }
    if (result == FR_TIMEOUT) {
      return BOARD_A_STORAGE_IO_TIMEOUT;
    }
    return BOARD_A_STORAGE_IO_WRITE;
  }
  if (write_count != length) {
    g_storage_mounted = 0U;
    *raw_error = (uint32_t)FR_INT_ERR;
    return BOARD_A_STORAGE_IO_WRITE;
  }
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_port_sync(
    void *context, void *handle, uint32_t deadline_ms, uint32_t *raw_error)
{
  FRESULT result;

  (void)context;
  if ((handle == NULL) || (raw_error == NULL)) {
    return BOARD_A_STORAGE_IO_OTHER;
  }
  storage_port_deadline_begin(deadline_ms);
  result = f_sync((FIL *)handle);
  *raw_error = (uint32_t)result;
  if (result != FR_OK) {
    g_storage_mounted = 0U;
    return (result == FR_TIMEOUT) ? BOARD_A_STORAGE_IO_TIMEOUT :
        BOARD_A_STORAGE_IO_SYNC;
  }
  return BOARD_A_STORAGE_IO_OK;
}

static board_a_storage_io_result_t storage_port_close(
    void *context, void *handle, uint32_t deadline_ms, uint32_t *raw_error)
{
  FRESULT result;

  (void)context;
  if ((handle == NULL) || (raw_error == NULL)) {
    return BOARD_A_STORAGE_IO_OTHER;
  }
  storage_port_deadline_begin(deadline_ms);
  result = f_close((FIL *)handle);
  *raw_error = (uint32_t)result;
  if (result != FR_OK) {
    g_storage_mounted = 0U;
    return (result == FR_TIMEOUT) ? BOARD_A_STORAGE_IO_TIMEOUT :
        BOARD_A_STORAGE_IO_CLOSE;
  }
  return BOARD_A_STORAGE_IO_OK;
}

static void storage_port_yield(void *context)
{
  (void)context;
  if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
    taskYIELD();
  }
}

static const board_a_storage_io_ops_t g_storage_ops = {
  .context = NULL,
  .probe = storage_port_probe,
  .open_new = storage_port_open_new,
  .write = storage_port_write,
  .sync = storage_port_sync,
  .close = storage_port_close,
  .now_ms = storage_port_now_ms,
  .yield = storage_port_yield
};

const board_a_storage_io_ops_t *board_a_storage_port_ops(void)
{
  return &g_storage_ops;
}

void board_a_storage_port_reset(void)
{
  g_storage_mounted = 0U;
  memset(&g_storage_file, 0, sizeof(g_storage_file));
  sd_spi_clear_deadline();
}
