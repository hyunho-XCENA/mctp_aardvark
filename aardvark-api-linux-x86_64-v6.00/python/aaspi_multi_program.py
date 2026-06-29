#!/usr/bin/env python3
#==========================================================================
# (c) 2004-2020  Total Phase, Inc.
#--------------------------------------------------------------------------
# Project : Multiple SPI EEPROM programmer
# File    : aaspi_multi_program.py
#--------------------------------------------------------------------------
# Program multiple SPI EEPROM devices using an Intel Hex format file.
#--------------------------------------------------------------------------
# Redistribution and use of this file in source and binary forms, with
# or without modification, are permitted.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
# FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
# COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
# INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
# BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
# LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
# LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
# ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
# POSSIBILITY OF SUCH DAMAGE.
#==========================================================================

#==========================================================================
# IMPORTS
#==========================================================================
from __future__ import division, with_statement, print_function

import sys
import threading

from aardvark_py import *


#==========================================================================
# CONSTANTS
#==========================================================================
SPI_BITRATE = 4000

# A mapping of devices to their (total memory size, page size)
DEVICES = {
    'AT25080' : (1024, 32),
    'AT25256' : (32768, 64),
}


#==========================================================================
# FUNCTIONS
#==========================================================================
def load_hex_file (filename, device):
    # Try to open file
    try:
        fp = open(filename, 'r')
    except:
        print('Unable to open file:', filename)
        sys.exit(1)

    # Create a 64k array of 0xFF
    data = array('B', [ 0xff for i in range(65535) ])

    # Read each line of the hex file and verify
    print('Reading file: %s' % filename)

    line_num = 1
    while True:
        line = fp.readline()

        # If empty, then end of file
        if len(line) == 0:
            break

        # Strip newline, linefeed and whitespace
        line = line.strip()

        # If empty, after strip, then it is simply a newline.
        if len(line) == 0:
            continue

        # Strip colon
        line = line[1:]

        # Parse line length
        line_length = int(line[0:2], 16)

        # Verify line length
        if len(line) != line_length*2 + 2 + 4 + 2 + 2:
            print('Error in line %d: Length mismatch' % line_num)
            sys.exit()

        # Verify line checksum
        line_check = 0
        for x in range(len(line)//2):
            line_check += int(line[x*2:x*2+2], 16)

        if line_check & 0xff != 0:
            print('Error in line %d: Line Checksum Error' % line_num)
            sys.exit()

        line_addr = int(line[2:6], 16)
        line_type = int(line[6:8], 16)

        # Verify type
        if line_type > 1:
            print('Error in line %d: Unsupported hex-record type' % line_num)
            sys.exit()

        line_data = line[8:-2]

        # Populate the data array
        if line_type == 0:
            for x in range(line_length):
                data[line_addr + x] = int(line_data[x*2:x*2+2], 16)

        # Increment iterator
        line_num += 1

    # Truncate the data to the maximum size for the EEPROM
    data = data[:DEVICES[device][0]]

    # Generate the checksum
    checksum = sum(data)

    return data, checksum

def get_serial_number (handle):
    serial = aa_unique_id(handle)
    return '%4d-%06d' % (serial // 1000000, serial % 1000000)

def write_memory (handle, device, data):
    # Get the serial number
    serial = get_serial_number(handle)

    # Determine the max size and page size of eeprom
    max_size, page_size = DEVICES[device]

    n = 0
    while n < len(data):
        # Write the write enable instruction
        data_out = array('B', [ 0x06 ])
        count, data_in = aa_spi_write(handle, data_out, 0)

        if count < 0:
            print('[%s] error: %s\n' % (serial, aa_status_string(count)))
            return

        if count != 1:
            print('[%s] error: read %d bytes (expected %d)' %
                  (serial, count, 1))

        # Assemble the write command and address
        data_out = array('B', [ 0 for i in range(3 + page_size) ] )
        data_out[0] = 0x02
        data_out[1] = (n >> 8) & 0xff
        data_out[2] = (n >> 0) & 0xff

        # Assemble a page of data
        data_out[3:] = data[n:n+page_size]

        n += len(data_out) - 3

        # Write the transaction
        count, data_in = aa_spi_write(handle, data_out, 0)

        if count < 0:
            print('[%s] error: %s\n' % (serial, aa_status_string(count)))
            return

        if count != len(data_out):
            print('[%s] error: read %d bytes (expected %d)' %
                  (serial, count-3, len(data_out)))

        aa_sleep_ms(10)

def read_memory (handle, addr, length):
    # Get the serial number
    serial = get_serial_number(handle)

    # Set up the command and read buffer
    data_out = array('B', [ 0 for i in range(3+length) ])
    data_in  = array_u08(3+length)

    # Assemble the read command and address
    data_out[0] = 0x03
    data_out[1] = (addr >> 8) & 0xff
    data_out[2] = (addr >> 0) & 0xff

    # Write the transaction
    count, data_in = aa_spi_write(handle, data_out, data_in)

    if count < 0:
        print('[%s] error: %s\n' % (serial, aa_status_string(count)))
        return None

    if count != length+3:
        print('[%s] error: read %d bytes (expected %d)' %
              (serial, count-3, length))

    return data_in[3:]

def write_thread (handle, program_data, device, max_size):
    serial = get_serial_number(handle)

    print('[%s] Writing EEPROM...' % serial)
    write_memory(handle, device, program_data)

    print('[%s] Reading EEPROM... pass 1' % serial)
    test1 = read_memory(handle, 0, max_size)

    if program_data == test1:
        print('[%s] ...PASSED' % serial)
    else:
        print('[%s] ...FAILED' % serial)

    print('[%s] Reading EEPROM... pass 2' % serial)
    test2 = read_memory(handle, 0, max_size)

    if program_data == test2:
        print('[%s] ...PASSED' % serial)
    else:
        print('[%s] ...FAILED' % serial)


#==========================================================================
# MAIN PROGRAM
#==========================================================================
def main (args):
    if len(args) != 4:
        print('usage: aaspi_multi_program DEVICE MODE FILENAME')
        print('  DEVICE    is the EEPROM device type')
        print('             - AT25080')
        print('             - AT25256')
        print('  MODE      is the SPI Mode')
        print('             - mode 0 : pol = 0, phase = 0')
        print('             - mode 1 : pol = 0, phase = 1')
        print('             - modes 2 and 3 are not supported')
        print('  FILENAME  is the Intel Hex Record file that')
        print('            contains the data to be sent to the')
        print('            SPI EEPROM')
        return 1

    device   = args[1]
    mode     = int(args[2])
    filename = args[3]

    # Test for valid mode
    if mode not in [ 0, 1 ]:
        print('Mode %d is not supported' % mode)
        return 1

    # Test for valid device
    if device not in DEVICES:
        print('%s is not a supported device' % device)
        return 1

    max_size, page_size = DEVICES[device]

    program_data, checksum = load_hex_file(filename, device)
    print('Checksum: 0x%x' % checksum)

    # Find up to 16 Aardvarks
    num_aardvarks, ports, unique_ids = aa_find_devices_ext(16, 16)
    print('Detected %s adapters' % num_aardvarks)

    # Open, configure, and start a thread for each Aardvark
    threads = []

    for num in range(num_aardvarks):
        # Open the Aardvark adapter
        port   = ports[num]
        handle = aa_open(port)

        if handle <= 0:
            print('Unable to open Aardvark device on port %d' % port)
            print('Error code = %d' % handle)
            return 1

        # Get the serial number
        serial = get_serial_number(handle)

        # Ensure that the SPI subsystem is enabled
        aa_configure(handle, AA_CONFIG_SPI_I2C)

        # Power the EEPROM using the Aardvark adapter's power supply.
        # This command is only effective on v2.0 hardware or greater.
        # The power pins on the v1.02 hardware are not enabled by default.
        aa_target_power(handle, AA_TARGET_POWER_BOTH)

        # Setup the clock phase
        aa_spi_configure(handle, mode >> 1, mode & 1, AA_SPI_BITORDER_MSB)

        # Set the bitrate
        bitrate = aa_spi_bitrate(handle, SPI_BITRATE)
        print('[%s] Bitrate set to %d kHz' % (serial, bitrate))

        thread = threading.Thread(
            target = write_thread,
            kwargs = {
                'handle':       handle,
                'program_data': program_data,
                'device':       device,
                'max_size':     max_size,
            }
        )

        threads.append((thread, handle))
        thread.start()

    # Wait for the threads to finish and close the Aardvarks
    for thread, handle in threads:
        thread.join()
        aa_close(handle)

    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
