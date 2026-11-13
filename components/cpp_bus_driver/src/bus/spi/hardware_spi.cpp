/*
 * @Description: Hardware SPI with persistent DMA bounce buffer
 * @Author: LILYGO_L (DMA fix by Off by One / ADS-B Scope)
 * @Date: 2025-02-13 15:04:49
 * @LastEditTime: 2026-03-20 00:00:00
 * @License: GPL 3.0
 */
#include "hardware_spi.h"

#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
#include "esp_heap_caps.h"
#endif

namespace Cpp_Bus_Driver
{
    bool Hardware_Spi::begin(int32_t freq_hz, int32_t cs)
    {
#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        if ((_bus_init_flag == true) && (_device_init_flag == true))
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__, "hardware_spi has been initialized\n");
            return true;
        }

#endif

        if (freq_hz == DEFAULT_CPP_BUS_DRIVER_VALUE)
        {
            freq_hz = DEFAULT_CPP_BUS_DRIVER_SPI_FREQ_HZ;
        }

#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        if (_flags == DEFAULT_CPP_BUS_DRIVER_VALUE)
        {
            _flags = static_cast<uint32_t>(NULL);
        }
#endif

        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _mosi: %d\n", _mosi);
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _sclk: %d\n", _sclk);
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _miso: %d\n", _miso);
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config cs: %d\n", cs);

#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _port: %d\n", _port);
#elif defined DEVELOPMENT_FRAMEWORK_ARDUINO_NRF
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _port address: %#X\n", _port);
#endif

        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _mode: %d\n", _mode);

#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _clock_source: %d\n", _clock_source);
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _flags: %d\n", _flags);
#elif defined DEVELOPMENT_FRAMEWORK_ARDUINO_NRF
        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config _bit_order: %d\n", _bit_order);
#endif

        assert_log(Log_Level::INFO, __FILE__, __LINE__, "hardware_spi config freq_hz: %d hz\n", freq_hz);

#if defined DEVELOPMENT_FRAMEWORK_ESPIDF

        // ── Pre-allocate persistent DMA bounce buffers ────────────
        if (_dma_tx_buf == nullptr)
        {
            _dma_tx_buf = static_cast<uint8_t *>(
                heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (_dma_tx_buf == nullptr)
            {
                assert_log(Log_Level::BUS, __FILE__, __LINE__,
                    "DMA TX buffer alloc failed (%u bytes)\n", DMA_BUF_SIZE);
                return false;
            }
        }

        if (_dma_rx_buf == nullptr)
        {
            _dma_rx_buf = static_cast<uint8_t *>(
                heap_caps_malloc(DMA_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
            if (_dma_rx_buf == nullptr)
            {
                assert_log(Log_Level::BUS, __FILE__, __LINE__,
                    "DMA RX buffer alloc failed (%u bytes)\n", DMA_BUF_SIZE);
                heap_caps_free(_dma_tx_buf);
                _dma_tx_buf = nullptr;
                return false;
            }
        }

        assert_log(Log_Level::INFO, __FILE__, __LINE__,
            "SPI DMA bounce buffers pre-allocated (%u bytes each)\n", DMA_BUF_SIZE);

        if (_bus_init_flag == false)
        {
            const spi_bus_config_t bus_config =
                {
                    .mosi_io_num = _mosi,
                    .miso_io_num = _miso,
                    .sclk_io_num = _sclk,
                    .quadwp_io_num = -1,
                    .quadhd_io_num = -1,
                    .data4_io_num = -1,
                    .data5_io_num = -1,
                    .data6_io_num = -1,
                    .data7_io_num = -1,
                    .data_io_default_level = 0,
                    .max_transfer_sz = 0,
                    .flags = SPICOMMON_BUSFLAG_MASTER,
                    .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
                    .intr_flags = static_cast<uint32_t>(NULL),
                };

            esp_err_t assert = spi_bus_initialize(_port, &bus_config, SPI_DMA_CH_AUTO);
            if (assert != ESP_OK)
            {
                assert_log(Log_Level::BUS, __FILE__, __LINE__, "spi_bus_initialize fail (error code: %#X)\n", assert);
                return false;
            }

            _bus_init_flag = true;
        }

        if (_device_init_flag == false)
        {
            const spi_device_interface_config_t device_config =
                {
                    .command_bits = 0,
                    .address_bits = 0,
                    .dummy_bits = 0,
                    .mode = _mode,
                    .clock_source = _clock_source,
                    .duty_cycle_pos = 128,
                    .cs_ena_pretrans = 1,
                    .cs_ena_posttrans = 1,
                    .clock_speed_hz = freq_hz,
                    .input_delay_ns = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
                    .sample_point = spi_sampling_point_t::SPI_SAMPLING_POINT_PHASE_0,
#endif
                    .spics_io_num = cs,
                    .flags = _flags,
                    .queue_size = 1,
                    .pre_cb = NULL,
                    .post_cb = NULL,
                };
            esp_err_t assert = spi_bus_add_device(_port, &device_config, &_spi_device);
            if (assert != ESP_OK)
            {
                assert_log(Log_Level::BUS, __FILE__, __LINE__, "spi_bus_add_device fail (error code: %#X)\n", assert);
                return false;
            }

            _device_init_flag = true;
        }

#elif defined DEVELOPMENT_FRAMEWORK_ARDUINO_NRF
        _spi_handle = std::make_unique<SPIClass>(_port, static_cast<uint8_t>(_miso), static_cast<uint8_t>(_sclk), static_cast<uint8_t>(_mosi));
        pin_mode(cs, Pin_Mode::OUTPUT);
        _spi_settings = SPISettings(freq_hz, _bit_order, _mode);

        _spi_handle->begin();
#endif

        _freq_hz = freq_hz;
        _cs = cs;

        return true;
    }

    bool Hardware_Spi::write(const void *data, size_t byte)
    {
#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        if (byte > DMA_BUF_SIZE)
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__,
                "SPI write exceeds DMA buffer (%u > %u)\n", byte, DMA_BUF_SIZE);
            return false;
        }

        // Bounce through pre-allocated DMA buffer
        memcpy(_dma_tx_buf, data, byte);

        spi_transaction_t buffer =
            {
                .flags = static_cast<uint32_t>(NULL),
                .cmd = 0,
                .addr = 0,
                .length = byte * 8,
                .rxlength = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
                .override_freq_hz = 0,
#endif
                .user = (void *)0,
                .tx_buffer = _dma_tx_buf,
                .rx_buffer = NULL,
            };

        esp_err_t assert = spi_device_polling_transmit(_spi_device, &buffer);
        if (assert != ESP_OK)
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__, "spi_device_polling_transmit fail (error code: %#X)\n", assert);
            return false;
        }

        return true;

#elif defined DEVELOPMENT_FRAMEWORK_ARDUINO_NRF
        _spi_handle->beginTransaction(_spi_settings);
        pin_write(_cs, 0);
        _spi_handle->transfer(const_cast<void *>(data), byte);
        pin_write(_cs, 1);
        _spi_handle->endTransaction();

        return true;
#else
        return false;
#endif
    }

    bool Hardware_Spi::read(void *data, size_t byte)
    {
#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        if (byte > DMA_BUF_SIZE)
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__,
                "SPI read exceeds DMA buffer (%u > %u)\n", byte, DMA_BUF_SIZE);
            return false;
        }

        spi_transaction_t buffer =
            {
                .flags = static_cast<uint32_t>(NULL),
                .cmd = 0,
                .addr = 0,
                .length = byte * 8,
                .rxlength = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
                .override_freq_hz = 0,
#endif
                .user = (void *)0,
                .tx_buffer = NULL,
                .rx_buffer = _dma_rx_buf,
            };

        esp_err_t assert = spi_device_polling_transmit(_spi_device, &buffer);
        if (assert != ESP_OK)
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__, "spi_device_polling_transmit fail (error code: %#X)\n", assert);
            return false;
        }

        // Copy out from DMA buffer to caller
        memcpy(data, _dma_rx_buf, byte);

        return true;
#elif defined DEVELOPMENT_FRAMEWORK_ARDUINO_NRF
        uint8_t buffer[byte] = {0};

        _spi_handle->beginTransaction(_spi_settings);
        pin_write(_cs, 0);
        _spi_handle->transfer(buffer, data, byte);
        pin_write(_cs, 1);
        _spi_handle->endTransaction();

        return true;
#else
        return false;
#endif
    }

    bool Hardware_Spi::write_read(const void *write_data, void *read_data, size_t data_byte)
    {
#if defined DEVELOPMENT_FRAMEWORK_ESPIDF
        if (data_byte > DMA_BUF_SIZE)
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__,
                "SPI write_read exceeds DMA buffer (%u > %u)\n", data_byte, DMA_BUF_SIZE);
            return false;
        }

        // Bounce TX through pre-allocated DMA buffer
        memcpy(_dma_tx_buf, write_data, data_byte);

        spi_transaction_t buffer =
            {
                .flags = static_cast<uint32_t>(NULL),
                .cmd = 0,
                .addr = 0,
                .length = data_byte * 8,
                .rxlength = 0,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)
                .override_freq_hz = 0,
#endif
                .user = (void *)0,
                .tx_buffer = _dma_tx_buf,
                .rx_buffer = _dma_rx_buf,
            };

        esp_err_t assert = spi_device_polling_transmit(_spi_device, &buffer);
        if (assert != ESP_OK)
        {
            assert_log(Log_Level::BUS, __FILE__, __LINE__, "spi_device_polling_transmit fail (error code: %#X)\n", assert);
            return false;
        }

        // Copy RX out from DMA buffer to caller
        memcpy(read_data, _dma_rx_buf, data_byte);

        return true;
#elif defined DEVELOPMENT_FRAMEWORK_ARDUINO_NRF
        _spi_handle->beginTransaction(_spi_settings);
        pin_write(_cs, 0);
        _spi_handle->transfer(write_data, read_data, data_byte);
        pin_write(_cs, 1);
        _spi_handle->endTransaction();

        return true;
#else
        return false;
#endif
    }
}
