// SPDX-License-Identifier: GPL-2.0+
/*
 * owgw-spi-master.c - IIO driver for OWGW SPI Master (STM32 1-Wire gateway)
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/workqueue.h>
#include <linux/crc8.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/string.h>
#include <linux/iio/temperature/owgw_iface.h>

#ifndef IIO_DMA_MINALIGN
#define IIO_DMA_MINALIGN ARCH_DMA_MINALIGN
#endif

#define OWGW_DRV_NAME			"owgw-spi-master"
#define OWGW_POLL_INTERVAL_DEFAULT	1000	/* ms */

#define OWGW_NUM_SENSORS		16
#define OWGW_MAX_PAYLOAD		(OWGW_NUM_SENSORS * 2)

#define OWGW_RESP_HEADER		2
#define OWGW_CRC_SIZE			1
#define OWGW_RX_LEN			(OWGW_RESP_HEADER + OWGW_MAX_PAYLOAD + OWGW_CRC_SIZE)
#define OWGW_TX_LEN			sizeof(OwSpiRequest)

#define OWGW_SPI_XFER_LEN		(OWGW_TX_LEN > OWGW_RX_LEN ? OWGW_TX_LEN : OWGW_RX_LEN)

DECLARE_CRC8_TABLE(owgw_crc8_table);

struct owgw_state {
	struct spi_device	*spi;
	struct iio_dev		*indio_dev;
	struct delayed_work	poll_work;
	struct mutex		lock;		/* protects sensor_values and spi transfers */
	s16			sensor_values[OWGW_NUM_SENSORS]; /* s16 для отрицательных температур */
	unsigned int		poll_interval_ms;
	bool			started;

	/* Буферы SPI ДОЛЖНЫ быть выровнены для DMA и находиться в конце структуры */
	u8			tx_buf[OWGW_SPI_XFER_LEN] __aligned(IIO_DMA_MINALIGN);
	u8			rx_buf[OWGW_SPI_XFER_LEN] __aligned(IIO_DMA_MINALIGN);
};

/* Публичная функция с поддержкой сдвига ответа на 1 такт (согласно протоколу) */
static int owgw_transfer(struct owgw_state *st, u8 cmd, const u8 *payload,
			 u8 payload_len, u8 *status, u8 *data_len, u8 *data)
{
	struct spi_transfer t = {0};
	struct spi_message m;
	OwSpiRequest *req = (OwSpiRequest *)st->tx_buf;
	OwSpiResponse *resp = (OwSpiResponse *)st->rx_buf;
	int ret;
	u8 crc, expected_crc;

	memset(st->tx_buf, 0, sizeof(st->tx_buf));
	memset(st->rx_buf, 0, sizeof(st->rx_buf));

	req->cmd = cmd;
	req->len = payload_len;
	if (payload && payload_len)
		memcpy(req->payload, payload, payload_len);

	t.tx_buf = st->tx_buf;
	t.rx_buf = st->rx_buf;
	t.len = OWGW_SPI_XFER_LEN;

	spi_message_init(&m);
	spi_message_add_tail(&t, &m);

	ret = spi_sync(st->spi, &m);
	if (ret)
		return ret;

	/* Если ответ нас реально интересует (т.е. запрашиваем payload) */
	if (status && data_len) {
		*status = resp->status;
		*data_len = resp->len;

		if (*data_len > OWGW_MAX_PAYLOAD) {
			dev_err(&st->spi->dev, "Invalid response length %u (max %u)\n",
				*data_len, OWGW_MAX_PAYLOAD);
			return -EIO;
		}

		expected_crc = st->rx_buf[OWGW_RESP_HEADER + *data_len];
		crc = crc8(owgw_crc8_table, st->rx_buf, OWGW_RESP_HEADER + *data_len, 0);
		if (crc != expected_crc) {
			dev_warn(&st->spi->dev, "CRC mismatch: calc=0x%02x, recv=0x%02x\n",
				 crc, expected_crc);
			return -EIO;
		}

		if (*status == OW_STATUS_OK && *data_len > 0 && data)
			memcpy(data, resp->payload, *data_len);
	}

	return 0;
}

static void owgw_poll_work(struct work_struct *work)
{
	struct owgw_state *st = container_of(work, struct owgw_state, poll_work.work);
	u8 status, len, data[OWGW_MAX_PAYLOAD];
	int i, ret;

	mutex_lock(&st->lock);

	if (!st->started) {
		mutex_unlock(&st->lock);
		return;
	}

	ret = owgw_transfer(st, OW_CMD_GET_DATA, NULL, 0, &status, &len, data);
	
	if (!ret && status == OW_STATUS_OK && len >= 2) {
		for (i = 0; i < OWGW_NUM_SENSORS && (i * 2 + 1) < len; i++) {
			/* Явное приведение к знаковому s16 для совместимости с IIO */
			st->sensor_values[i] = (s16)(data[i * 2] | ((u16)data[i * 2 + 1] << 8));
		}
	}

	mutex_unlock(&st->lock);
	schedule_delayed_work(&st->poll_work, msecs_to_jiffies(st->poll_interval_ms));
}

static int owgw_read_raw(struct iio_dev *indio_dev,
			 const struct iio_chan_spec *chan,
			 int *val, int *val2, long mask)
{
	struct owgw_state *st = iio_priv(indio_dev);

	if (chan->type != IIO_TEMP)
		return -EINVAL;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (chan->channel >= OWGW_NUM_SENSORS)
			return -EINVAL;
		mutex_lock(&st->lock);
		*val = st->sensor_values[chan->channel];
		mutex_unlock(&st->lock);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		*val = 1;
		*val2 = 0;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static const struct iio_chan_spec owgw_channels[] = {
#define OWGW_TEMP_CHAN(idx) {						\
	.type = IIO_TEMP,						\
	.indexed = 1,							\
	.channel = (idx),						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.scan_type = {							\
		.sign = 's',						\
		.realbits = 16,						\
		.storagebits = 16,					\
		.endianness = IIO_LE,					\
	},								\
}
	OWGW_TEMP_CHAN(0),  OWGW_TEMP_CHAN(1),  OWGW_TEMP_CHAN(2),  OWGW_TEMP_CHAN(3),
	OWGW_TEMP_CHAN(4),  OWGW_TEMP_CHAN(5),  OWGW_TEMP_CHAN(6),  OWGW_TEMP_CHAN(7),
	OWGW_TEMP_CHAN(8),  OWGW_TEMP_CHAN(9),  OWGW_TEMP_CHAN(10), OWGW_TEMP_CHAN(11),
	OWGW_TEMP_CHAN(12), OWGW_TEMP_CHAN(13), OWGW_TEMP_CHAN(14), OWGW_TEMP_CHAN(15),
	IIO_CHAN_SOFT_TIMESTAMP(16),
};

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct owgw_state *st = iio_priv(indio_dev);
	return sysfs_emit(buf, "%s\n", st->started ? "running" : "stopped");
}
static IIO_DEVICE_ATTR_RO(status, 0);

static ssize_t poll_interval_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct owgw_state *st = iio_priv(indio_dev);
	return sysfs_emit(buf, "%u\n", st->poll_interval_ms);
}

static ssize_t poll_interval_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct owgw_state *st = iio_priv(indio_dev);
	unsigned int val;

	if (kstrtouint(buf, 10, &val) || val < 10 || val > 60000)
		return -EINVAL;

	mutex_lock(&st->lock);
	st->poll_interval_ms = val;
	mutex_unlock(&st->lock);
	return len;
}
static IIO_DEVICE_ATTR_RW(poll_interval, 0);

static struct attribute *owgw_attrs[] = {
	&iio_dev_attr_status.dev_attr.attr,
	&iio_dev_attr_poll_interval.dev_attr.attr,
	NULL,
};

static const struct attribute_group owgw_attr_group = {
	.attrs = owgw_attrs,
};

static const struct iio_info owgw_info = {
	.read_raw = owgw_read_raw,
	.attrs = &owgw_attr_group, /* Правильный способ назначения атрибутов */
};

static int owgw_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct owgw_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	st->poll_interval_ms = OWGW_POLL_INTERVAL_DEFAULT;
	mutex_init(&st->lock);
	INIT_DELAYED_WORK(&st->poll_work, owgw_poll_work);

	crc8_populate_msb(owgw_crc8_table, 0x07);

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	ret = spi_setup(spi);
	if (ret < 0)
		return ret;

	spi_set_drvdata(spi, indio_dev);
	st->indio_dev = indio_dev;

	indio_dev->info = &owgw_info;
	indio_dev->name = OWGW_DRV_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = owgw_channels;
	indio_dev->num_channels = ARRAY_SIZE(owgw_channels);

	ret = devm_iio_device_register(&spi->dev, indio_dev);
	if (ret)
		return ret;

	/* Передаем START с защитой мьютексом */
	mutex_lock(&st->lock);
	ret = owgw_transfer(st, OW_CMD_START, NULL, 0, NULL, NULL, NULL);
	st->started = (ret == 0);
	mutex_unlock(&st->lock);

	if (ret) {
		dev_err(&spi->dev, "Failed to send START: %d\n", ret);
		return ret;
	}

	schedule_delayed_work(&st->poll_work, msecs_to_jiffies(st->poll_interval_ms));
	dev_info(&spi->dev, "OWGW SPI Master IIO driver initialized (16 channels)\n");
	
	return 0;
}

static int owgw_remove(struct spi_device *spi)
{
	struct iio_dev *indio_dev = spi_get_drvdata(spi);
	struct owgw_state *st = iio_priv(indio_dev);

	cancel_delayed_work_sync(&st->poll_work);

	/* Останавливаем автомат на STM32 при выгрузке драйвера */
	mutex_lock(&st->lock);
	if (st->started) {
		owgw_transfer(st, OW_CMD_STOP, NULL, 0, NULL, NULL, NULL);
		st->started = false;
	}
	mutex_unlock(&st->lock);

	dev_info(&spi->dev, "OWGW SPI Master driver removed\n");
	return 0;
}

static const struct of_device_id owgw_of_match[] = {
	{ .compatible = "proglyk,owgw-spi-master" },
	{ }
};
MODULE_DEVICE_TABLE(of, owgw_of_match);

static const struct spi_device_id owgw_id_table[] = {
	{ "owgw-spi-master", 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, owgw_id_table);

static struct spi_driver owgw_spi_driver = {
	.driver = {
		.name           = OWGW_DRV_NAME,
		.of_match_table = owgw_of_match,
	},
	.id_table       = owgw_id_table,
	.probe          = owgw_probe,
	.remove         = owgw_remove,
};

module_spi_driver(owgw_spi_driver);

MODULE_AUTHOR("Ilya Pronyashin <msg@proglyk.ru>");
MODULE_DESCRIPTION("OWGW SPI Master IIO driver (16 temperature channels)");
MODULE_LICENSE("GPL v2");