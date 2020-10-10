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
#ifndef _I2CInterface_h
#define _I2CInterface_h

#include <stdint.h>

/**
 * A generic interface for interacting with an I2C bus
 */
class I2CInterface {
public:
    
    /**
     * Determmines whether a device is connected at the specified address.
     * @return 0 if things are good, -1 if there is a problem.
     */
    virtual uint8_t check_address(uint8_t i2c_bus_addr) = 0;

    /**
     * Standard read operation.
     * @return The received byte
     */
    virtual uint8_t read(uint8_t i2c_bus_addr, uint8_t addr) = 0;

    /** 
     * Standard write operation.
     * @return Then number of bytes written
     */
    virtual uint8_t write(uint8_t i2c_bus_addr, uint8_t addr, uint8_t data) = 0;

    /**
     * Multi-byte write operation
     * @return The number of bytes written
     */
    virtual uint8_t write_bulk(uint8_t i2c_bus_addr, uint8_t addr, uint8_t bytes, uint8_t *data) = 0;
};

#endif
