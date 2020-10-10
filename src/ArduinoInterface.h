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
#ifndef _ArduinoInterface_h
#define _ArduinoInterface_h

#include "Arduino.h"
#include "Wire.h"
#include "I2CInterface.h"

/**
 * The standard Ardiuno way of talking to I2C
 * via the Wire.h library.
 */
class ArduinoInterface : public I2CInterface {
public:

    ArduinoInterface() {
        Wire.begin();
    }

    uint8_t check_address(uint8_t i2c_bus_addr) {
    	Wire.beginTransmission(i2c_bus_addr);
        return Wire.endTransmission();
    }

    uint8_t read(uint8_t i2c_bus_addr, uint8_t addr) {

        uint8_t reg_val = 0;

        Wire.beginTransmission(i2c_bus_addr);
        Wire.write(addr);
        Wire.endTransmission();

        Wire.requestFrom(i2c_bus_addr, (uint8_t)1, (uint8_t)false);

        while(Wire.available())
        {
            reg_val = Wire.read();
        }

        return reg_val;
    }

    uint8_t write(uint8_t i2c_bus_addr, uint8_t addr, uint8_t data) {
    	Wire.beginTransmission(i2c_bus_addr);
        Wire.write(addr);
        Wire.write(data);
        return Wire.endTransmission();
    }

    uint8_t write_bulk(uint8_t i2c_bus_addr, uint8_t addr, uint8_t bytes, uint8_t *data) {
        Wire.beginTransmission(i2c_bus_addr);
        Wire.write(addr);
        for(int i = 0; i < bytes; i++)
        {
            Wire.write(data[i]);
        }
        return Wire.endTransmission();
     }
};

#endif
