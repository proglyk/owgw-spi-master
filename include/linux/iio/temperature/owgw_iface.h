/**
  * @file   owgw_iface.h
  * @brief  Соглашение о командах и данных по обмену между master и slave
  * @author Ilia Proniashin, msg@proglyk.ru
  * @date   24-March-2026
  */

#ifndef OWGW_IFACE_H
#define OWGW_IFACE_H

#include <linux/types.h>

/*
 * Протокол SPI для моста 1-Wire (Linux master <-> MCU slave)
 *
 * Кадр запроса (master -> slave):
 *   [CMD:1] [LEN:1] [PAYLOAD:0..N]
 *
 * Кадр ответа (slave -> master), читается при следующей транзакции SPI:
 *   [STATUS:1] [LEN:1] [PAYLOAD:0..N]
 *
 * Все многобайтовые значения передаются в формате little-endian.
 *
 * Режим работы:
 *   1. Master отправляет OW_CMD_START — slave запускает непрерывный цикл опроса
 *   2. Slave автоматически опрашивает датчики каждые 1 сек (через SwTimer)
 *   3. Master постоянно читает данные через OW_CMD_GET_DATA
 *   4. Master отправляет OW_CMD_STOP для остановки цикла
 */

/* ---- Команды (master -> slave) ---- */
#define OW_CMD_NOP              0x00  /* Нет операции, чтение статуса и данных */
#define OW_CMD_START            0x01  /* Запустить непрерывный автоматический опрос (1 сек период) */
#define OW_CMD_STOP             0x02  /* Остановить автоматический опрос */
#define OW_CMD_GET_DEVICES      0x03  /* Получить список найденных устройств */
#define OW_CMD_GET_DATA         0x04  /* Получить данные с устройств (температуры) */
#define OW_CMD_GET_STATS        0x05  /* Получить статистику работы автомата */

/* ---- Режимы работы для OW_CMD_START (payload[0]) ---- */
#define OW_MODE_SEARCH_ROM      0x00  /* Искать все устройства (по умолчанию при len==0) */
#define OW_MODE_SKIP_ROM        0x01  /* Один датчик на шине, пропустить поиск ROM */

/* ---- Коды статуса (slave -> master) ---- */
#define OW_STATUS_OK            0x00
#define OW_STATUS_BUSY          0x01  /* Автомат запущен, операция в процессе */
#define OW_STATUS_ERROR         0x02  /* Ошибка 1-Wire (отсутствие presence, CRC и т.д.) */
#define OW_STATUS_NO_DEVICE     0x03  /* Устройства не найдены */
#define OW_STATUS_BAD_INDEX     0x04  /* Запрошенный индекс >= количества устройств */
#define OW_STATUS_UNKNOWN_CMD   0xFF
#define OW_STATUS_STOPPED       0xFE  /* Автомат остановлен */

/* ---- Ограничения ---- */
#define OW_MAX_DEVICES          16
#define OW_ROM_SIZE             8
#define OW_SCRATCHPAD_SIZE      9
#define OW_TEMPERATURE_DATA_SIZE 2  /* 2 байта на температуру (raw data) */

/* Исправление: максимальный размер данных, которые мы вытягиваем - 32 байта, а не 9 */
#define OW_MAX_PAYLOAD_SIZE     (OW_MAX_DEVICES * OW_TEMPERATURE_DATA_SIZE)

/* ---- Структуры кадров ---- */
typedef struct __attribute__((packed)) {
    u8 cmd;
    u8 len;
    u8 payload[OW_ROM_SIZE];  /* Максимальная полезная нагрузка запроса */
} OwSpiRequest;

typedef struct __attribute__((packed)) {
    u8 status;
    u8 len;
    u8 payload[OW_MAX_PAYLOAD_SIZE]; /* БЫЛО OW_SCRATCHPAD_SIZE (9), СТАЛО 32! */
} OwSpiResponse;

/* ---- Структура данных устройства ---- */
typedef struct {
    u8 rom[OW_ROM_SIZE];
    u16 temperature_raw;  /* Сырые данные температуры (2 байта) */
    u8 valid;            /* Флаг валидности данных */
    u8 reserved;
} OwDeviceData;

// Hardware Abstraction Layer Interface
// The user application must implement these functions and pass them to the library.
typedef struct {
    void (*setBaudRate)(int baudRate);
    u8 *pucRxBuf; // uartRxBuffer
    u8 *pucTxBuf; // uartTxBuffer
    void (*xmit_data)(u8* txData, u8* rxData, int length);
    void (*xmit_reset)(u8* txData, u8* rxData);
} OneWireHW;

#endif // OWGW_IFACE_H