/*
 * Copyright (C) 2020 Bruce MacKinnon KC1FSZ
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef _STM32_HAL_Interface_h
#define _STM32_HAL_Interface_h

#include <stdint.h>
// NOTE: Add the appropriate HAL include here:
#include "stm32f4xx_hal.h"

#include "I2CInterface.h"

// An STM32/HAL based implementation of the I2CInterface.
// 
// NOT IMPLEMENTED YET!!
//
class STM32_HAL_Interface : public I2CInterface {
public:

    STM32_HAL_Interface(I2C_HandleTypeDef* hi2c)
    :   _hi2c(hi2c),
        _errorCount(0),
        _timeoutMs(10) {
    }

    uint8_t check_address(uint8_t i2c_bus_addr) {
        HAL_StatusTypeDef status = HAL_I2C_IsDeviceReady(_hi2c, (uint16_t)(i2c_bus_addr << 1), 1, _timeoutMs);
        if (status == HAL_OK) {
           return 0;
        } else {
            return -1;
        }
    }
    
    uint8_t read(uint8_t i2c_bus_addr, uint8_t addr) {
        uint8_t reg_val = 0;
        // NOTE: PER HAL DOCUMENTATION, ADDRESS NEEDS TO BE SHIFTED LEFT
        HAL_StatusTypeDef status = HAL_I2C_Mem_Read(_hi2c, (uint16_t)(i2c_bus_addr << 1), (uint16_t)(addr), 1, 
            &reg_val, 1, _timeoutMs);
        if (status != HAL_OK) {
            _errorCount++;
        }
        return reg_val;        
    }
    
    uint8_t write(uint8_t i2c_bus_addr, uint8_t addr, uint8_t data) {
        // NOTE: PER HAL DOCUMENTATION, ADDRESS NEEDS TO BE SHIFTED LEFT
	    HAL_StatusTypeDef status = HAL_I2C_Mem_Write(_hi2c, (uint16_t)(i2c_bus_addr << 1), addr, 1, 
            &data, 1, _timeoutMs);
        if (status != HAL_OK) {
            _errorCount++;
        }
        return 1;
    }

    uint8_t write_bulk(uint8_t i2c_bus_addr, uint8_t addr, uint8_t bytes, uint8_t *data)  {
        // NOTE: PER HAL DOCUMENTATION, ADDRESS NEEDS TO BE SHIFTED LEFT
	    HAL_StatusTypeDef status = HAL_I2C_Mem_Write(_hi2c, (uint16_t)(i2c_bus_addr << 1), addr, 1, 
            data, bytes, _timeoutMs);
        if (status != HAL_OK) {
            _errorCount++;
        }
        return bytes;
    }

private:

    I2C_HandleTypeDef* _hi2c;
    int _errorCount;
    uint32_t _timeoutMs;
};

#endif
