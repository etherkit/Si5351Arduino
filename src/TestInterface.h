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
#ifndef _Test_Interface_h
#define _Test_Interface_h

#include <stdint.h>
#include <stdio.h>
#include "I2CInterface.h"

// A dummy interface for testing.
// 
class TestInterface : public I2CInterface {
public:
    TestInterface() {
        printf("TestInterface initialized\n");
    }
    uint8_t check_address(uint8_t i2c_bus_addr) {
        printf("check_address\n");
        return 0;
    }
    uint8_t read(uint8_t i2c_bus_addr, uint8_t addr) {
        printf("read(%x,%x)\n", i2c_bus_addr, addr);
        return 0;
    }
    uint8_t write(uint8_t i2c_bus_addr, uint8_t addr, uint8_t data) {
        printf("write(%x, %x, %x)\n", i2c_bus_addr, addr, data);
        return 0;
    }
    uint8_t write_bulk(uint8_t i2c_bus_addr, uint8_t addr, uint8_t bytes, uint8_t *data)  {
        printf("write_bulk(%x, %x, ", i2c_bus_addr, addr);
        for (int i = 0; i < bytes; i++) {
            printf("%x ", data[i]);
        }
        printf(")\n");
        return 0;
    }
};

#endif
