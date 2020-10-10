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
#include "I2CInterface.h"

// An STM32/HAL based implementation of the I2CInterface.
// 
// NOT IMPLEMENTED YET!!
//
class STM32_HAL_Interface : public I2C_Interface {
public:
    // TODO: WILL PASS THE HAL I2C STRUCTURE HERE
    STM32_HAL_Interface() {
    }
    uint8_t check_address(uint8_t i2c_bus_addr) {
        return 0;
    }
    uint8_t read(uint8_t i2c_bus_addr, uint8_t addr) {
        return 0;
    }
    uint8_t write(uint8_t i2c_bus_addr, uint8_t addr, uint8_t data) {
        return 0;
    }
    uint8_t write_bulk(uint8_t i2c_bus_addr, uint8_t addr, uint8_t bytes, uint8_t *data)  {
        return 0;
    }
};

#endif
