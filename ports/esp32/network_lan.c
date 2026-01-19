/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 "Eric Poulsen" <eric@zyxod.com>
 * Copyright (c) 2021 "Tobias Eydam" <tobiaseydam@hotmail.com>
 *
 * Based on the ESP IDF example code which is Public Domain / CC0
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "py/runtime.h"
#include "py/mphal.h"

#if MICROPY_PY_NETWORK_LAN

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#if CONFIG_ETH_USE_SPI_ETHERNET
#include "driver/spi_master.h"
#endif

#include "modnetwork.h"
#include "extmod/modnetwork.h"

#if PHY_LAN867X_ENABLED
#include "esp_eth_phy_lan867x.h"
#endif

typedef struct _lan_if_obj_t {
    base_if_obj_t base;
    bool initialized;
    int8_t mdc_pin;
    int8_t mdio_pin;
    int8_t phy_reset_pin;
    int8_t phy_power_pin;
    int8_t phy_cs_pin;
    int8_t phy_int_pin;
    uint8_t phy_addr;
    uint8_t phy_type;
    #if CONFIG_ETH_USE_SPI_ETHERNET
    spi_host_device_t spi_host;  // Track which SPI bus we're using
    #endif
    esp_eth_phy_t *phy;
    esp_eth_handle_t eth_handle;
} lan_if_obj_t;

const mp_obj_type_t lan_if_type;
static lan_if_obj_t lan_obj = {{{&lan_if_type}, ESP_IF_ETH, NULL}, false, false};
static uint8_t eth_status = 0;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
    int32_t event_id, void *event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            eth_status = ETH_CONNECTED;
            ESP_LOGI("ethernet", "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            eth_status = ETH_DISCONNECTED;
            ESP_LOGI("ethernet", "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_START:
            eth_status = ETH_STARTED;
            ESP_LOGI("ethernet", "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            eth_status = ETH_STOPPED;
            ESP_LOGI("ethernet", "Ethernet Stopped");
            break;
        case IP_EVENT_ETH_GOT_IP:
            eth_status = ETH_GOT_IP;
            ESP_LOGI("ethernet", "Ethernet Got IP");
            break;
        default:
            break;
    }
}

static void set_mac_address(lan_if_obj_t *self, uint8_t *mac, size_t len) {
    if (len != 6) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid buffer length"));
    }
    if (((mac[0] & 0x01) != 0) ||
        (esp_eth_ioctl(self->eth_handle, ETH_CMD_S_MAC_ADDR, mac) != ESP_OK) ||
        (esp_netif_set_mac(self->base.netif, mac) != ESP_OK)) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("failed setting MAC address"));
    }
}

static mp_obj_t get_lan(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    lan_if_obj_t *self = &lan_obj;

    ESP_LOGI("lan", "get_lan() called - initialized=%d eth_handle=%p netif=%p phy=%p active=%d",
             self->initialized, self->eth_handle, self->base.netif, self->phy, self->base.active);
    // DIAGNOSTIC: Log state on every call
    mp_printf(&mp_plat_print, "[DEBUG] get_lan() called - initialized=%d eth_handle=%p netif=%p phy=%p active=%d\n",
              self->initialized, self->eth_handle, self->base.netif, self->phy, self->base.active);

    if (self->initialized) {
        // Verify handles are still valid
        if (self->eth_handle != NULL && self->base.netif != NULL) {
            ESP_LOGI("lan", "Returning existing LAN instance");
            return MP_OBJ_FROM_PTR(&lan_obj);
        }
        // Handles invalid - force cleanup
        ESP_LOGW("lan", "LAN marked initialized but handles invalid, forcing cleanup");
    }
    
    // Cleanup if ANY resources detected (handles aborted init)
    if (self->eth_handle != NULL || self->base.netif != NULL || 
        self->phy != NULL || self->base.active) {
        
        ESP_LOGW("lan", "Detected leaked resources, forcing cleanup");
        ESP_LOGI("lan", "  eth_handle=%p netif=%p phy=%p active=%d initialized=%d",
                 self->eth_handle, self->base.netif, self->phy, 
                 self->base.active, self->initialized);
        mp_printf(&mp_plat_print, "[WARN] Leaked resources detected!\n");
        mp_printf(&mp_plat_print, "  eth_handle=%p netif=%p phy=%p active=%d init=%d\n",
                  self->eth_handle, self->base.netif, self->phy, 
                  self->base.active, self->initialized);
        
        mp_printf(&mp_plat_print, "  Unregistering event handlers...\n");
        
        // Unregister event handlers - be aggressive, try multiple times
        for (int i = 0; i < 3; i++) {
            esp_err_t ret1 = esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
            esp_err_t ret2 = esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
            if (ret1 == ESP_ERR_INVALID_STATE && ret2 == ESP_ERR_INVALID_STATE) {
                // Both handlers not registered - good
                break;
            }
            ESP_LOGW("lan", "Event handler unregister attempt %d: ETH=%d IP=%d", i+1, ret1, ret2);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        vTaskDelay(pdMS_TO_TICKS(50));  // Let unregister complete

        // Stop and uninstall driver
        if (self->eth_handle != NULL) {
            esp_eth_stop(self->eth_handle);
            vTaskDelay(pdMS_TO_TICKS(100));
            
            mp_printf(&mp_plat_print, "  Calling esp_eth_driver_uninstall...\n");
            esp_err_t ret = esp_eth_driver_uninstall(self->eth_handle);
            mp_printf(&mp_plat_print, "  Result: %d (ESP_OK=0)\n", ret);
            if (ret != ESP_OK) {
                ESP_LOGE("lan", "Failed to uninstall driver: %d - ABORTING", ret);
                mp_raise_msg(&mp_type_OSError, 
                    MP_ERROR_TEXT("Failed to cleanup previous LAN instance - reboot required"));
            }
            self->eth_handle = NULL;
            self->phy = NULL;
        }
        
        // Destroy netif
        if (self->base.netif != NULL) {
            mp_printf(&mp_plat_print, "  Calling esp_netif_destroy...\n");
            esp_netif_destroy(self->base.netif);
            self->base.netif = NULL;
        }
        
        // Reset state
        self->base.active = false;
        self->initialized = false;
        eth_status = 0;
        
        // Wait for ESP-IDF cleanup
        vTaskDelay(pdMS_TO_TICKS(500));
        ESP_LOGI("lan", "Cleanup complete, proceeding with fresh init");
        #if CONFIG_ETH_USE_SPI_ETHERNET
        // CRITICAL: Try to free SPI bus if it was used for W5500/DM9051/KSZ8851
        if (self->spi_host != 0) {
            mp_printf(&mp_plat_print, "  WARNING: SPI bus %d may have leaked device handles\n", self->spi_host);
            mp_printf(&mp_plat_print, "  Attempting spi_bus_free() to force cleanup...\n");
            esp_err_t ret = spi_bus_free(self->spi_host);
            mp_printf(&mp_plat_print, "  spi_bus_free() returned: %d\n", ret);
            self->spi_host = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        #endif
    }

    enum { ARG_id, ARG_mdc, ARG_mdio, ARG_reset, ARG_power, ARG_phy_addr, ARG_phy_type,
           ARG_spi, ARG_cs, ARG_int, ARG_ref_clk_mode, ARG_ref_clk };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_id,           MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_mdc,          MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_mdio,         MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_reset,        MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_power,        MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_phy_addr,     MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_phy_type,     MP_ARG_KW_ONLY | MP_ARG_REQUIRED | MP_ARG_INT },
        { MP_QSTR_spi,          MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_cs,           MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_int,          MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
        { MP_QSTR_ref_clk_mode, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_ref_clk,      MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_obj = mp_const_none} },
    };

    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    if (args[ARG_id].u_obj != mp_const_none) {
        if (mp_obj_get_int(args[ARG_id].u_obj) != 0) {
            mp_raise_ValueError(MP_ERROR_TEXT("invalid LAN interface identifier"));
        }
    }

    #define GET_PIN(XXX) args[XXX].u_obj == mp_const_none ? -1 : machine_pin_get_id(args[XXX].u_obj);

    self->mdc_pin = GET_PIN(ARG_mdc);
    self->mdio_pin = GET_PIN(ARG_mdio);
    self->phy_reset_pin = GET_PIN(ARG_reset);
    self->phy_power_pin = GET_PIN(ARG_power);
    self->phy_cs_pin = GET_PIN(ARG_cs);
    self->phy_int_pin = GET_PIN(ARG_int);

    if (args[ARG_phy_addr].u_int < 0x00 || args[ARG_phy_addr].u_int > 0x1f) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid phy address"));
    }
    self->phy_addr = args[ARG_phy_addr].u_int;

    if (args[ARG_phy_type].u_int != PHY_LAN8710 &&
        args[ARG_phy_type].u_int != PHY_LAN8720 &&
        args[ARG_phy_type].u_int != PHY_IP101 &&
        args[ARG_phy_type].u_int != PHY_RTL8201 &&
        args[ARG_phy_type].u_int != PHY_KSZ8041 &&
        args[ARG_phy_type].u_int != PHY_KSZ8081 &&
        #if PHY_LAN867X_ENABLED
        args[ARG_phy_type].u_int != PHY_LAN8670 &&
        #endif
        #if PHY_GENERIC_ENABLED
        args[ARG_phy_type].u_int != PHY_GENERIC &&
        #endif
        #if CONFIG_ETH_USE_SPI_ETHERNET
        #if CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
        args[ARG_phy_type].u_int != PHY_KSZ8851SNL &&
        #endif
        #if CONFIG_ETH_SPI_ETHERNET_DM9051
        args[ARG_phy_type].u_int != PHY_DM9051 &&
        #endif
        #if CONFIG_ETH_SPI_ETHERNET_W5500
        args[ARG_phy_type].u_int != PHY_W5500 &&
        #endif
        #endif
        args[ARG_phy_type].u_int != PHY_DP83848) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid phy type"));
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32P4
    eth_esp32_emac_config_t esp32_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    #endif

    esp_eth_mac_t *mac = NULL;

    #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32P4
    // Dynamic ref_clk configuration.
    if (args[ARG_ref_clk_mode].u_int != -1) {
        // Map the GPIO_MODE constants to EMAC_CLK constants.
        esp32_config.clock_config.rmii.clock_mode =
            args[ARG_ref_clk_mode].u_int == GPIO_MODE_INPUT ? EMAC_CLK_EXT_IN : EMAC_CLK_OUT;
    }
    if (args[ARG_ref_clk].u_obj != mp_const_none) {
        esp32_config.clock_config.rmii.clock_gpio = machine_pin_get_id(args[ARG_ref_clk].u_obj);
    }
    #endif

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = self->phy_addr;
    phy_config.reset_gpio_num = self->phy_reset_pin;
    self->phy = NULL;
    // Switch on the power before PHY is reset
    if (self->phy_power_pin >= 0) {
        mp_hal_pin_output(self->phy_power_pin);
        mp_hal_pin_write(self->phy_power_pin, 1);
        // let the power settle
        mp_hal_delay_ms(100);
    }
    #if CONFIG_ETH_USE_SPI_ETHERNET
    spi_device_interface_config_t devcfg = {
        .mode = 0,
        .clock_speed_hz = MICROPY_PY_NETWORK_LAN_SPI_CLOCK_SPEED_MZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = self->phy_cs_pin,
        .command_bits = 0, // Can both be set to 0, as the respective
        .address_bits = 0, // driver fills in proper default values.
    };
    #endif

    // Store SPI device handle for cleanup (W5500/DM9051/KSZ8851)
    #if CONFIG_ETH_USE_SPI_ETHERNET
    spi_device_handle_t spi_device_handle = NULL;
    #endif

    switch (args[ARG_phy_type].u_int) {
        #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32P4
        case PHY_LAN8710:
        case PHY_LAN8720:
            self->phy = esp_eth_phy_new_lan87xx(&phy_config);
            break;
        case PHY_IP101:
            self->phy = esp_eth_phy_new_ip101(&phy_config);
            break;
        case PHY_RTL8201:
            self->phy = esp_eth_phy_new_rtl8201(&phy_config);
            break;
        case PHY_DP83848:
            self->phy = esp_eth_phy_new_dp83848(&phy_config);
            break;
        case PHY_KSZ8041:
        case PHY_KSZ8081:
            self->phy = esp_eth_phy_new_ksz80xx(&phy_config);
            break;
        #if PHY_LAN867X_ENABLED
        case PHY_LAN8670:
            self->phy = esp_eth_phy_new_lan867x(&phy_config);
            break;
        #endif
        #if PHY_GENERIC_ENABLED
        case PHY_GENERIC:
            self->phy = esp_eth_phy_new_generic(&phy_config);
            break;
        #endif
        #endif // CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32P4
        #if CONFIG_ETH_USE_SPI_ETHERNET
        #if CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
        case PHY_KSZ8851SNL: {
            spi_host_device_t host = machine_hw_spi_get_host(args[ARG_spi].u_obj);
            eth_ksz8851snl_config_t chip_config = ETH_KSZ8851SNL_DEFAULT_CONFIG(host, &devcfg);
            chip_config.int_gpio_num = self->phy_int_pin;
            mac = esp_eth_mac_new_ksz8851snl(&chip_config, &mac_config);
            self->phy = esp_eth_phy_new_ksz8851snl(&phy_config);
            break;
        }
        #endif
        #if CONFIG_ETH_SPI_ETHERNET_DM9051
        case PHY_DM9051: {
            spi_host_device_t host = machine_hw_spi_get_host(args[ARG_spi].u_obj);
            eth_dm9051_config_t chip_config = ETH_DM9051_DEFAULT_CONFIG(host, &devcfg);
            chip_config.int_gpio_num = self->phy_int_pin;
            mac = esp_eth_mac_new_dm9051(&chip_config, &mac_config);
            self->phy = esp_eth_phy_new_dm9051(&phy_config);
            break;
        }
        #endif
        #if CONFIG_ETH_SPI_ETHERNET_W5500
        case PHY_W5500: {
            spi_host_device_t host = machine_hw_spi_get_host(args[ARG_spi].u_obj);

            mp_printf(&mp_plat_print, "[DEBUG] Creating W5500 with SPI host=%d cs=%d int=%d rst=%d\n",
                      host, self->phy_cs_pin, self->phy_int_pin, self->phy_reset_pin);
            mp_printf(&mp_plat_print, "[DEBUG] devcfg: mode=%d speed=%d queue=%d cs=%d\n",
                      devcfg.mode, devcfg.clock_speed_hz, devcfg.queue_size, devcfg.spics_io_num);

            eth_w5500_config_t chip_config = ETH_W5500_DEFAULT_CONFIG(host, &devcfg);
            chip_config.int_gpio_num = self->phy_int_pin;

            mp_printf(&mp_plat_print, "[DEBUG] About to create W5500 MAC...\n");

            mac = esp_eth_mac_new_w5500(&chip_config, &mac_config);

            mp_printf(&mp_plat_print, "[DEBUG] MAC created: %p\n", mac);

            if (mac == NULL) {
                mp_printf(&mp_plat_print, "[ERROR] esp_eth_mac_new_w5500() returned NULL!\n");
                mp_printf(&mp_plat_print, "  This usually means spi_bus_add_device() failed inside the macro.\n");
                mp_printf(&mp_plat_print, "  SPI bus may have leaked device handles from previous runs.\n");
                
                // Try to free any leaked SPI devices on this bus
                // Unfortunately we can't access them directly...
                mp_raise_msg(&mp_type_OSError, 
                    MP_ERROR_TEXT("W5500 MAC creation failed - SPI device leak, reboot required"));
            }

            mp_printf(&mp_plat_print, "[DEBUG] About to create W5500 PHY...\n");

            self->phy = esp_eth_phy_new_w5500(&phy_config);

            mp_printf(&mp_plat_print, "[DEBUG] esp_eth_phy_new_w5500() returned: %p\n", self->phy);
                
            if (self->phy == NULL) {
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("Failed to create W5500 PHY"));
            }
            break;
        }
        #endif
        #endif
    }

    #if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32P4
    if (!IS_SPI_PHY(args[ARG_phy_type].u_int)) {
        if (self->mdc_pin == -1 || self->mdio_pin == -1) {
            mp_raise_ValueError(MP_ERROR_TEXT("mdc and mdio must be specified"));
        }
        esp32_config.smi_mdc_gpio_num = self->mdc_pin;
        esp32_config.smi_mdio_gpio_num = self->mdio_pin;
        mac = esp_eth_mac_new_esp32(&esp32_config, &mac_config);
    }
    #endif

    if (esp_netif_init() != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_netif_init failed"));
    }

    // Check if netif already exists and destroy it first
    if (self->base.netif != NULL) {
        ESP_LOGW("lan", "netif still exists before creation, destroying it");
        esp_netif_destroy(self->base.netif);
        self->base.netif = NULL;
    }

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    self->base.netif = esp_netif_new(&cfg);
    ESP_LOGI("lan", "Created new netif: %p", self->base.netif);

    if (esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL) != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_event_handler_register failed"));
    }

    if (esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL) != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_event_handler_register failed"));
    }
    // DIAGNOSTIC: Verify MAC and PHY are valid before driver install
    mp_printf(&mp_plat_print, "[DEBUG] About to install driver:\n");
    mp_printf(&mp_plat_print, "  MAC: %p\n", mac);
    mp_printf(&mp_plat_print, "  PHY: %p (type=%d addr=%d)\n", 
              self->phy, self->phy_type, self->phy_addr);
    if (self->phy_type == PHY_W5500) {
        mp_printf(&mp_plat_print, "  W5500 config: spi=XX cs=%d int=%d reset=%d\n",
                  self->phy_cs_pin, self->phy_int_pin, self->phy_reset_pin);
    }
    
    // Verify MAC has required functions
    if (mac == NULL) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("MAC is NULL!"));
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, self->phy);

    esp_err_t esp_err = esp_eth_driver_install(&config, &self->eth_handle);
    ESP_LOGI("lan", "esp_eth_driver_install returned: %d (0x%x), handle=%p", 
             esp_err, esp_err, self->eth_handle);
    mp_printf(&mp_plat_print, "[DEBUG] esp_eth_driver_install returned: %d, handle=%p\n",
              esp_err, self->eth_handle);

    if (esp_err == ESP_OK) {
        self->base.active = false;
        self->initialized = true;
    } else {
        // CRITICAL FIX: Cleanup on failure!
        mp_printf(&mp_plat_print, "[ERROR] Driver install failed! Cleaning up leaked resources...\n");
        
        // Unregister event handlers we just registered
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        
        // Destroy netif we just created
        if (self->base.netif != NULL) {
            mp_printf(&mp_plat_print, "  Destroying leaked netif %p\n", self->base.netif);
            esp_netif_destroy(self->base.netif);
            self->base.netif = NULL;
        }
        
        // Clear PHY pointer (it was passed to driver_install, which may have partially used it)
        self->phy = NULL;
        
        mp_printf(&mp_plat_print, "  Cleanup complete\n");
        

        if (esp_err == ESP_ERR_INVALID_ARG) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_eth_driver_install failed with invalid argument"));
        } else if (esp_err == ESP_ERR_NO_MEM) {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_eth_driver_install failed with no memory for driver"));
        } else {
            mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_eth_driver_install failed"));
        }
    }

    if (esp_netif_attach(self->base.netif, esp_eth_new_netif_glue(self->eth_handle)) != ESP_OK) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("esp_netif_attach failed"));
    }

    // If MAC address is unset, set it to the address reserved for the ESP32 ETH interface
    uint8_t mac_addr[6];
    esp_eth_ioctl(self->eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
    if ((mac_addr[0] | mac_addr[1] | mac_addr[2] | mac_addr[3] | mac_addr[4] | mac_addr[5]) == 0) {
        esp_read_mac(mac_addr, ESP_MAC_ETH);  // Get ESP32 MAC address for ETH iface
        set_mac_address(self, mac_addr, sizeof(mac_addr));
    }

    eth_status = ETH_INITIALIZED;

    return MP_OBJ_FROM_PTR(&lan_obj);
}
MP_DEFINE_CONST_FUN_OBJ_KW(esp_network_get_lan_obj, 0, get_lan);

static mp_obj_t lan_active(size_t n_args, const mp_obj_t *args) {
    lan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (n_args > 1) {
        bool make_active = mp_obj_is_true(args[1]);
        if (make_active && !self->base.active) {
            esp_netif_set_hostname(self->base.netif, mod_network_hostname_data);
            self->base.active = (esp_eth_start(self->eth_handle) == ESP_OK);
            if (!self->base.active) {
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("ethernet enable failed"));
            }
        } else if (!make_active && self->base.active) {
            self->base.active = !(esp_eth_stop(self->eth_handle) == ESP_OK);
            if (self->base.active) {
                mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("ethernet disable failed"));
            }
        }
    }

    return mp_obj_new_bool(self->base.active);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(lan_active_obj, 1, 2, lan_active);

static mp_obj_t lan_status(mp_obj_t self_in) {
    return MP_OBJ_NEW_SMALL_INT(eth_status);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lan_status_obj, lan_status);

static mp_obj_t lan_isconnected(mp_obj_t self_in) {
    lan_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_bool(self->base.active && (eth_status == ETH_GOT_IP));
}
static MP_DEFINE_CONST_FUN_OBJ_1(lan_isconnected_obj, lan_isconnected);

static mp_obj_t lan_config(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    if (n_args != 1 && kwargs->used != 0) {
        mp_raise_TypeError(MP_ERROR_TEXT("either pos or kw args are allowed"));
    }
    lan_if_obj_t *self = MP_OBJ_TO_PTR(args[0]);

    if (kwargs->used != 0) {

        for (size_t i = 0; i < kwargs->alloc; i++) {
            if (mp_map_slot_is_filled(kwargs, i)) {
                switch (mp_obj_str_get_qstr(kwargs->table[i].key)) {
                    case MP_QSTR_mac: {
                        mp_buffer_info_t bufinfo;
                        mp_get_buffer_raise(kwargs->table[i].value, &bufinfo, MP_BUFFER_READ);
                        set_mac_address(self, bufinfo.buf, bufinfo.len);
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        return mp_const_none;
    }

    if (n_args != 2) {
        mp_raise_TypeError(MP_ERROR_TEXT("can query only one param"));
    }

    mp_obj_t val = mp_const_none;

    switch (mp_obj_str_get_qstr(args[1])) {
        case MP_QSTR_mac: {
            uint8_t mac[6];
            esp_eth_ioctl(self->eth_handle, ETH_CMD_G_MAC_ADDR, mac);
            return mp_obj_new_bytes(mac, sizeof(mac));
        }
        case MP_QSTR_ifname: {
            val = esp_ifname(self->base.netif);
            break;
        }
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("unknown config param"));
    }

    return val;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(lan_config_obj, 1, lan_config);

static mp_obj_t lan_deinit(mp_obj_t self_in) {
    lan_if_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // DIAGNOSTIC
    mp_printf(&mp_plat_print, "[DEBUG] lan_deinit() called - initialized=%d eth_handle=%p netif=%p\n",
              self->initialized, self->eth_handle, self->base.netif);


    if (!self->initialized && self->eth_handle == NULL && self->base.netif == NULL) {
        // Nothing to cleanup
        return mp_const_none;
    }
    
    ESP_LOGI("lan", "Deinitializing LAN interface");
    
    // Unregister event handlers first - try multiple times to be sure
    for (int i = 0; i < 3; i++) {
        esp_err_t ret1 = esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        esp_err_t ret2 = esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler);
        if (ret1 == ESP_ERR_INVALID_STATE && ret2 == ESP_ERR_INVALID_STATE) {
            break;  // Not registered anymore
        }
        ESP_LOGI("lan", "Event unregister attempt %d: ETH=%d IP=%d", i+1, ret1, ret2);
        vTaskDelay(pdMS_TO_TICKS(50));
    }    

    // Stop ethernet if active
    if (self->base.active && self->eth_handle != NULL) {
        ESP_LOGI("lan", "Stopping ethernet");
        esp_eth_stop(self->eth_handle);
        vTaskDelay(pdMS_TO_TICKS(100));
        self->base.active = false;
    }
    
    // Destroy netif before driver
    if (self->base.netif != NULL) {
        ESP_LOGI("lan", "Destroying netif");
        mp_printf(&mp_plat_print, "  Destroying netif %p...\n", self->base.netif);
         esp_netif_destroy(self->base.netif);
        mp_printf(&mp_plat_print, "  Netif destroyed\n");
        self->base.netif = NULL;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    // Uninstall driver (frees MAC, PHY, SPI)
    if (self->eth_handle != NULL) {
        ESP_LOGI("lan", "Uninstalling ethernet driver");
        mp_printf(&mp_plat_print, "  Uninstalling driver %p...\n", self->eth_handle);
        esp_err_t ret = esp_eth_driver_uninstall(self->eth_handle);
        mp_printf(&mp_plat_print, "  Result: %d (ESP_OK=0)\n", ret);
        self->eth_handle = NULL;
        self->phy = NULL;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    // Reset state
    self->initialized = false;
    eth_status = 0;
    
    ESP_LOGI("lan", "Deinitialization complete");
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(lan_deinit_obj, lan_deinit);

static const mp_rom_map_elem_t lan_if_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_active), MP_ROM_PTR(&lan_active_obj) },
    { MP_ROM_QSTR(MP_QSTR_isconnected), MP_ROM_PTR(&lan_isconnected_obj) },
    { MP_ROM_QSTR(MP_QSTR_status), MP_ROM_PTR(&lan_status_obj) },
    { MP_ROM_QSTR(MP_QSTR_config), MP_ROM_PTR(&lan_config_obj) },
    { MP_ROM_QSTR(MP_QSTR_ifconfig), MP_ROM_PTR(&esp_network_ifconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_ipconfig), MP_ROM_PTR(&esp_nic_ipconfig_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&lan_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&lan_deinit_obj) },
};

static MP_DEFINE_CONST_DICT(lan_if_locals_dict, lan_if_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    lan_if_type,
    MP_QSTR_LAN,
    MP_TYPE_FLAG_NONE,
    locals_dict, &lan_if_locals_dict
    );

#endif
