Attribute VB_Name = "AARDVARK"
'==========================================================================
' Aardvark Interface Library
'--------------------------------------------------------------------------
' Copyright (c) 2003-2024 Total Phase, Inc.
' All rights reserved.
' www.totalphase.com
'
' Redistribution and use of this file in source and binary forms, with
' or without modification, are permitted provided that the following
' conditions are met:
'
' - Redistributions of source code must retain the above copyright
'   notice, this list of conditions, and the following disclaimer.
'
' - Redistributions in binary form must reproduce the above copyright
'   notice, this list of conditions, and the following disclaimer in the
'   documentation or other materials provided with the distribution.
'
' - This file must only be used to interface with Total Phase products.
'   The names of Total Phase and its contributors must not be used to
'   endorse or promote products derived from this software.
'
' THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
' "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING BUT NOT
' LIMITED TO THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
' FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.  IN NO EVENT WILL THE
' COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
' INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING
' BUT NOT LIMITED TO PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
' LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
' CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
' LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
' ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
' POSSIBILITY OF SUCH DAMAGE.
'--------------------------------------------------------------------------
' To access Total Phase Aardvark devices through the API:
'
' 1) Use one of the following shared objects:
'      aardvark.so       --  Linux or macOS shared object
'      aardvark.dll      --  Windows dynamic link library
'
' 2) Along with one of the following language modules:
'      aardvark.c/h      --  C/C++ API header file and interface module
'      aardvark_py.py    --  Python API
'      aardvark.cs       --  C# .NET source
'      aardvark_net.dll  --  Compiled .NET binding
'      aardvark.bas      --  Visual Basic 6 API
'==========================================================================

' Data types:
'    Handles are Long
'
'    Fixed bit-width types
'      8-bit => Byte
'     16-bit => Integer
'     32-bit => Long
'     64-bit => LongLong

Public Type LongLong
    LoValue As Long
    HiValue As Long
End Type

Public Enum AardvarkStatus
    ' General codes (0 to -99)
    AA_OK                       =    0
    AA_UNABLE_TO_LOAD_LIBRARY   =   -1
    AA_UNABLE_TO_LOAD_DRIVER    =   -2
    AA_UNABLE_TO_LOAD_FUNCTION  =   -3
    AA_INCOMPATIBLE_LIBRARY     =   -4
    AA_INCOMPATIBLE_DEVICE      =   -5
    AA_COMMUNICATION_ERROR      =   -6
    AA_UNABLE_TO_OPEN           =   -7
    AA_UNABLE_TO_CLOSE          =   -8
    AA_INVALID_HANDLE           =   -9
    AA_CONFIG_ERROR             =  -10

    ' I2C codes (-100 to -199)
    AA_I2C_NOT_AVAILABLE        = -100
    AA_I2C_NOT_ENABLED          = -101
    AA_I2C_READ_ERROR           = -102
    AA_I2C_WRITE_ERROR          = -103
    AA_I2C_SLAVE_BAD_CONFIG     = -104
    AA_I2C_SLAVE_READ_ERROR     = -105
    AA_I2C_SLAVE_TIMEOUT        = -106
    AA_I2C_DROPPED_EXCESS_BYTES = -107
    AA_I2C_BUS_ALREADY_FREE     = -108

    ' SPI codes (-200 to -299)
    AA_SPI_NOT_AVAILABLE        = -200
    AA_SPI_NOT_ENABLED          = -201
    AA_SPI_WRITE_ERROR          = -202
    AA_SPI_SLAVE_READ_ERROR     = -203
    AA_SPI_SLAVE_TIMEOUT        = -204
    AA_SPI_DROPPED_EXCESS_BYTES = -205

    ' GPIO codes (-400 to -499)
    AA_GPIO_NOT_AVAILABLE       = -400
End Enum

Public Type AardvarkVersion
    ' Software, firmware, and hardware versions.
    software        as Integer
    firmware        as Integer
    hardware        as Integer

    ' Firmware requires that software must be >= this version.
    sw_req_by_fw    as Integer

    ' Software requires that firmware must be >= this version.
    fw_req_by_sw    as Integer

    ' Software requires that the API interface must be >= this version.
    api_req_by_sw   as Integer
End Type

Public Const AA_PORT_NOT_FREE = &H8000

Public Type AardvarkExt
    ' Version matrix
    version    as AardvarkVersion

    ' Features of this device.
    features   as Long
End Type

Public Const AA_FEATURE_SPI = &H00000001
Public Const AA_FEATURE_I2C = &H00000002
Public Const AA_FEATURE_GPIO = &H00000008

Public Const AA_LOG_STDOUT = 1
Public Const AA_LOG_STDERR = 2

Public Enum AardvarkConfig
    AA_CONFIG_GPIO_ONLY = &H00
    AA_CONFIG_SPI_GPIO  = &H01
    AA_CONFIG_GPIO_I2C  = &H02
    AA_CONFIG_SPI_I2C   = &H03
    AA_CONFIG_QUERY     = &H80
End Enum

Public Const AA_CONFIG_SPI_MASK = &H00000001
Public Const AA_CONFIG_I2C_MASK = &H00000002

Public Const AA_TARGET_POWER_NONE = &H00
Public Const AA_TARGET_POWER_BOTH = &H03
Public Const AA_TARGET_POWER_QUERY = &H80

Public Const AA_ASYNC_NO_DATA = &H00000000
Public Const AA_ASYNC_I2C_READ = &H00000001
Public Const AA_ASYNC_I2C_WRITE = &H00000002
Public Const AA_ASYNC_SPI = &H00000004

Public Enum AardvarkI2cFlags
    AA_I2C_NO_FLAGS          = &H00
    AA_I2C_10_BIT_ADDR       = &H01
    AA_I2C_COMBINED_FMT      = &H02
    AA_I2C_NO_STOP           = &H04
    AA_I2C_SIZED_READ        = &H10
    AA_I2C_SIZED_READ_EXTRA1 = &H20
End Enum

Public Enum AardvarkI2cStatus
    AA_I2C_STATUS_OK            = 0
    AA_I2C_STATUS_BUS_ERROR     = 1
    AA_I2C_STATUS_SLA_ACK       = 2
    AA_I2C_STATUS_SLA_NACK      = 3
    AA_I2C_STATUS_DATA_NACK     = 4
    AA_I2C_STATUS_ARB_LOST      = 5
    AA_I2C_STATUS_BUS_LOCKED    = 6
    AA_I2C_STATUS_LAST_DATA_ACK = 7
End Enum

Public Const AA_I2C_PULLUP_NONE = &H00
Public Const AA_I2C_PULLUP_BOTH = &H03
Public Const AA_I2C_PULLUP_QUERY = &H80

Public Enum AardvarkSpiPolarity
    AA_SPI_POL_RISING_FALLING = 0
    AA_SPI_POL_FALLING_RISING = 1
End Enum

Public Enum AardvarkSpiPhase
    AA_SPI_PHASE_SAMPLE_SETUP = 0
    AA_SPI_PHASE_SETUP_SAMPLE = 1
End Enum

Public Enum AardvarkSpiBitorder
    AA_SPI_BITORDER_MSB = 0
    AA_SPI_BITORDER_LSB = 1
End Enum

Public Enum AardvarkSpiSSPolarity
    AA_SPI_SS_ACTIVE_LOW  = 0
    AA_SPI_SS_ACTIVE_HIGH = 1
End Enum

Public Enum AardvarkGpioBits
    AA_GPIO_SCL  = &H01
    AA_GPIO_SDA  = &H02
    AA_GPIO_MISO = &H04
    AA_GPIO_SCK  = &H08
    AA_GPIO_MOSI = &H10
    AA_GPIO_SS   = &H20
End Enum

Public Const AA_GPIO_DIR_INPUT = 0
Public Const AA_GPIO_DIR_OUTPUT = 1

Public Const AA_GPIO_PULLUP_OFF = 0
Public Const AA_GPIO_PULLUP_ON = 1

Private Declare Function vb6_aa_find_devices Lib "aardvark.dll" (ByVal num_devices As Long, ByRef devices As Integer) as Long
Private Declare Function vb6_aa_find_devices_ext Lib "aardvark.dll" (ByVal num_devices As Long, ByRef devices As Integer, ByVal num_ids As Long, ByRef unique_ids As Long) as Long
Private Declare Function vb6_aa_open Lib "aardvark.dll" (ByVal port_number As Long) as Long
Private Declare Function vb6_aa_open_ext Lib "aardvark.dll" (ByVal port_number As Long, ByRef aa_ext As AardvarkExt) as Long
Private Declare Function vb6_aa_close Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_port Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_features Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_unique_id Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_status_string Lib "aardvark.dll" (ByVal status As Long) as String
Private Declare Function vb6_aa_log Lib "aardvark.dll" (ByVal aardvark As Long, ByVal level As Long, ByVal handle As Long) as Long
Private Declare Function vb6_aa_version Lib "aardvark.dll" (ByVal aardvark As Long, ByRef version As AardvarkVersion) as Long
Private Declare Function vb6_aa_configure Lib "aardvark.dll" (ByVal aardvark As Long, ByVal config As Long) as Long
Private Declare Function vb6_aa_target_power Lib "aardvark.dll" (ByVal aardvark As Long, ByVal power_mask As Byte) as Long
Private Declare Function vb6_aa_sleep_ms Lib "aardvark.dll" (ByVal milliseconds As Long) as Long
Private Declare Function vb6_aa_async_poll Lib "aardvark.dll" (ByVal aardvark As Long, ByVal timeout As Long) as Long
Private Declare Function vb6_aa_i2c_free_bus Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_i2c_bitrate Lib "aardvark.dll" (ByVal aardvark As Long, ByVal bitrate_khz As Long) as Long
Private Declare Function vb6_aa_i2c_bus_timeout Lib "aardvark.dll" (ByVal aardvark As Long, ByVal timeout_ms As Integer) as Long
Private Declare Function vb6_aa_i2c_read Lib "aardvark.dll" (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_in As Byte) as Long
Private Declare Function vb6_aa_i2c_read_ext Lib "aardvark.dll" (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_in As Byte, ByRef num_read As Integer) as Long
Private Declare Function vb6_aa_i2c_write Lib "aardvark.dll" (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_out As Byte) as Long
Private Declare Function vb6_aa_i2c_write_ext Lib "aardvark.dll" (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_out As Byte, ByRef num_written As Integer) as Long
Private Declare Function vb6_aa_i2c_write_read Lib "aardvark.dll" (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal out_num_bytes As Integer, ByRef out_data As Byte, ByRef num_written As Integer, ByVal in_num_bytes As Integer, ByRef in_data As Byte, ByRef num_read As Integer) as Long
Private Declare Function vb6_aa_i2c_slave_enable Lib "aardvark.dll" (ByVal aardvark As Long, ByVal addr As Byte, ByVal maxTxBytes As Integer, ByVal maxRxBytes As Integer) as Long
Private Declare Function vb6_aa_i2c_slave_disable Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_i2c_slave_set_response Lib "aardvark.dll" (ByVal aardvark As Long, ByVal num_bytes As Byte, ByRef data_out As Byte) as Long
Private Declare Function vb6_aa_i2c_slave_write_stats Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_i2c_slave_read Lib "aardvark.dll" (ByVal aardvark As Long, ByRef addr As Byte, ByVal num_bytes As Integer, ByRef data_in As Byte) as Long
Private Declare Function vb6_aa_i2c_slave_write_stats_ext Lib "aardvark.dll" (ByVal aardvark As Long, ByRef num_written As Integer) as Long
Private Declare Function vb6_aa_i2c_slave_read_ext Lib "aardvark.dll" (ByVal aardvark As Long, ByRef addr As Byte, ByVal num_bytes As Integer, ByRef data_in As Byte, ByRef num_read As Integer) as Long
Private Declare Function vb6_aa_i2c_pullup Lib "aardvark.dll" (ByVal aardvark As Long, ByVal pullup_mask As Byte) as Long
Private Declare Function vb6_aa_spi_bitrate Lib "aardvark.dll" (ByVal aardvark As Long, ByVal bitrate_khz As Long) as Long
Private Declare Function vb6_aa_spi_configure Lib "aardvark.dll" (ByVal aardvark As Long, ByVal polarity As Long, ByVal phase As Long, ByVal bitorder As Long) as Long
Private Declare Function vb6_aa_spi_write Lib "aardvark.dll" (ByVal aardvark As Long, ByVal out_num_bytes As Integer, ByRef data_out As Byte, ByVal in_num_bytes As Integer, ByRef data_in As Byte) as Long
Private Declare Function vb6_aa_spi_slave_enable Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_spi_slave_disable Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_spi_slave_set_response Lib "aardvark.dll" (ByVal aardvark As Long, ByVal num_bytes As Byte, ByRef data_out As Byte) as Long
Private Declare Function vb6_aa_spi_slave_read Lib "aardvark.dll" (ByVal aardvark As Long, ByVal num_bytes As Integer, ByRef data_in As Byte) as Long
Private Declare Function vb6_aa_spi_master_ss_polarity Lib "aardvark.dll" (ByVal aardvark As Long, ByVal polarity As Long) as Long
Private Declare Function vb6_aa_gpio_direction Lib "aardvark.dll" (ByVal aardvark As Long, ByVal direction_mask As Byte) as Long
Private Declare Function vb6_aa_gpio_pullup Lib "aardvark.dll" (ByVal aardvark As Long, ByVal pullup_mask As Byte) as Long
Private Declare Function vb6_aa_gpio_get Lib "aardvark.dll" (ByVal aardvark As Long) as Long
Private Declare Function vb6_aa_gpio_set Lib "aardvark.dll" (ByVal aardvark As Long, ByVal value As Byte) as Long
Private Declare Function vb6_aa_gpio_change Lib "aardvark.dll" (ByVal aardvark As Long, ByVal timeout As Integer) as Long


'==========================================================================
' VERSION
'==========================================================================
Public Const AA_API_VERSION    = &H0600   ' v6.00
Public Const AA_REQ_SW_VERSION = &H050a   ' v5.10

Private Declare Function aa_std_version Lib "aardvark.dll" () as Long
Dim AA_VERSION_OK as Boolean

Private Function check_version () as Boolean
    If AA_VERSION_OK Then
        check_version = True
    Else
        Dim sw_version As Long
        Dim req_api_version As Long
        sw_version = aa_std_version() And &Hffff
        req_api_version = (aa_std_version() / 65536) And &Hffff
        AA_VERSION_OK = ((sw_version >= AA_REQ_SW_VERSION) And (AA_API_VERSION >= req_api_version))
        check_version = AA_VERSION_OK
    End If
End Function

'==========================================================================
' HELPER FUNCTIONS
'==========================================================================
Public Function tp_min (ByVal x As Variant, ByVal y As Variant) as Variant
    If x > y Then
        tp_min = y
    Else
        tp_min = x
    End If
End Function


'==========================================================================
' STATUS CODES
'==========================================================================
' All API functions return an integer which is the result of the
' transaction, or a status code if negative.  The status codes are
' defined as follows:

'==========================================================================
' GENERAL TYPE DEFINITIONS
'==========================================================================
' Aardvark handle type definition
' typedef Aardvark => Long

' Deprecated type definitions.
'
' These are only for use with legacy code and
' should not be used for new development.
' typedef aa_u08 => Byte

' typedef aa_u16 => Integer

' typedef aa_u32 => Long

' typedef aa_s08 => s08

' typedef aa_s16 => s16

' typedef aa_s32 => s32

' Aardvark version matrix.
'
' This matrix describes the various version dependencies
' of Aardvark components.  It can be used to determine
' which component caused an incompatibility error.
'
' All version numbers are of the format:
'   (major << 8) | minor
'
' ex. v1.20 would be encoded as:  0x0114

'==========================================================================
' GENERAL API
'==========================================================================
' Get a list of ports to which Aardvark devices are attached.
'
' nelem   = maximum number of elements to return
' devices = array into which the port numbers are returned
'
' Each element of the array is written with the port number.
' Devices that are in-use are ORed with AA_PORT_NOT_FREE (0x8000).
'
' ex.  devices are attached to ports 0, 1, 2
'      ports 0 and 2 are available, and port 1 is in-use.
'      array => 0x0000, 0x8001, 0x0002
'
' If the array is NULL, it is not filled with any values.
' If there are more devices than the array size, only the
' first nmemb port numbers will be written into the array.
'
' Returns the number of devices found, regardless of the
' array size.

Public Function aa_find_devices (ByVal num_devices As Long, ByRef devices() As Integer) as Long
    If check_version() Then
        Dim devices_num_devices As Long
        devices_num_devices = tp_min(num_devices, UBound(devices) - LBound(devices) + 1)
        aa_find_devices = vb6_aa_find_devices(devices_num_devices, devices(0))
    Else
        aa_find_devices = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Get a list of ports to which Aardvark devices are attached.
'
' This function is the same as aa_find_devices() except that
' it returns the unique IDs of each Aardvark device.  The IDs
' are guaranteed to be non-zero if valid.
'
' The IDs are the unsigned integer representation of the 10-digit
' serial numbers.

Public Function aa_find_devices_ext (ByVal num_devices As Long, ByRef devices() As Integer, ByVal num_ids As Long, ByRef unique_ids() As Long) as Long
    If check_version() Then
        Dim devices_num_devices As Long
        devices_num_devices = tp_min(num_devices, UBound(devices) - LBound(devices) + 1)
        Dim unique_ids_num_ids As Long
        unique_ids_num_ids = tp_min(num_ids, UBound(unique_ids) - LBound(unique_ids) + 1)
        aa_find_devices_ext = vb6_aa_find_devices_ext(devices_num_devices, devices(0), unique_ids_num_ids, unique_ids(0))
    Else
        aa_find_devices_ext = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Open the Aardvark port.
'
' The port number is a zero-indexed integer.
'
' The port number is the same as that obtained from the
' aa_find_devices() function above.
'
' Returns an Aardvark handle, which is guaranteed to be
' greater than zero if it is valid.
'
' This function is recommended for use in simple applications
' where extended information is not required.  For more complex
' applications, the use of aa_open_ext() is recommended.

Public Function aa_open (ByVal port_number As Long) as Long
    If check_version() Then
        aa_open = vb6_aa_open(port_number)
    Else
        aa_open = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Open the Aardvark port, returning extended information
' in the supplied structure.  Behavior is otherwise identical
' to aa_open() above.  If 0 is passed as the pointer to the
' structure, this function is exactly equivalent to aa_open().
'
' The structure is zeroed before the open is attempted.
' It is filled with whatever information is available.
'
' For example, if the firmware version is not filled, then
' the device could not be queried for its version number.
'
' This function is recommended for use in complex applications
' where extended information is required.  For more simple
' applications, the use of aa_open() is recommended.

Public Function aa_open_ext (ByVal port_number As Long, ByRef aa_ext As AardvarkExt) as Long
    If check_version() Then
        aa_open_ext = vb6_aa_open_ext(port_number, aa_ext)
    Else
        aa_open_ext = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Close the Aardvark port.

Public Function aa_close (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_close = vb6_aa_close(aardvark)
    Else
        aa_close = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Return the port for this Aardvark handle.
'
' The port number is a zero-indexed integer.

Public Function aa_port (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_port = vb6_aa_port(aardvark)
    Else
        aa_port = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Return the device features as a bit-mask of values, or
' an error code if the handle is not valid.

Public Function aa_features (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_features = vb6_aa_features(aardvark)
    Else
        aa_features = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Return the unique ID for this Aardvark adapter.
' IDs are guaranteed to be non-zero if valid.
' The ID is the unsigned integer representation of the
' 10-digit serial number.

Public Function aa_unique_id (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_unique_id = vb6_aa_unique_id(aardvark)
    Else
        aa_unique_id = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Return the status string for the given status code.
' If the code is not valid or the library function cannot
' be loaded, return a NULL string.

Public Function aa_status_string (ByVal status As Long) as String
    If check_version() Then
        aa_status_string = vb6_aa_status_string(status)
    Else
        aa_status_string = vbNullString
    End If
End Function


' Enable logging to a file.  The handle must be standard file
' descriptor.  In C, a file descriptor can be obtained by using
' the ANSI C function "open" or by using the function "fileno"
' on a FILE* stream.  A FILE* stream can be obtained using "fopen"
' or can correspond to the common "stdout" or "stderr" --
' available when including stdlib.h

Public Function aa_log (ByVal aardvark As Long, ByVal level As Long, ByVal handle As Long) as Long
    If check_version() Then
        aa_log = vb6_aa_log(aardvark, level, handle)
    Else
        aa_log = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Return the version matrix for the device attached to the
' given handle.  If the handle is 0 or invalid, only the
' software and required api versions are set.

Public Function aa_version (ByVal aardvark As Long, ByRef version As AardvarkVersion) as Long
    If check_version() Then
        aa_version = vb6_aa_version(aardvark, version)
    Else
        aa_version = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Configure the device by enabling/disabling I2C, SPI, and
' GPIO functions.

Public Function aa_configure (ByVal aardvark As Long, ByVal config As Long) as Long
    If check_version() Then
        aa_configure = vb6_aa_configure(aardvark, config)
    Else
        aa_configure = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Configure the target power pins.
' This is only supported on hardware versions >= 2.00

Public Function aa_target_power (ByVal aardvark As Long, ByVal power_mask As Byte) as Long
    If check_version() Then
        aa_target_power = vb6_aa_target_power(aardvark, power_mask)
    Else
        aa_target_power = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Sleep for the specified number of milliseconds
' Accuracy depends on the operating system scheduler
' Returns the number of milliseconds slept

Public Function aa_sleep_ms (ByVal milliseconds As Long) as Long
    If check_version() Then
        aa_sleep_ms = vb6_aa_sleep_ms(milliseconds)
    Else
        aa_sleep_ms = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



'==========================================================================
' ASYNC MESSAGE POLLING
'==========================================================================
' Polling function to check if there are any asynchronous
' messages pending for processing. The function takes a timeout
' value in units of milliseconds.  If the timeout is < 0, the
' function will block until data is received.  If the timeout is 0,
' the function will perform a non-blocking check.

Public Function aa_async_poll (ByVal aardvark As Long, ByVal timeout As Long) as Long
    If check_version() Then
        aa_async_poll = vb6_aa_async_poll(aardvark, timeout)
    Else
        aa_async_poll = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



'==========================================================================
' I2C API
'==========================================================================
' Free the I2C bus.

Public Function aa_i2c_free_bus (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_i2c_free_bus = vb6_aa_i2c_free_bus(aardvark)
    Else
        aa_i2c_free_bus = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Set the I2C bit rate in kilohertz.  If a zero is passed as the
' bitrate, the bitrate is unchanged and the current bitrate is
' returned.

Public Function aa_i2c_bitrate (ByVal aardvark As Long, ByVal bitrate_khz As Long) as Long
    If check_version() Then
        aa_i2c_bitrate = vb6_aa_i2c_bitrate(aardvark, bitrate_khz)
    Else
        aa_i2c_bitrate = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Set the bus lock timeout.  If a zero is passed as the timeout,
' the timeout is unchanged and the current timeout is returned.

Public Function aa_i2c_bus_timeout (ByVal aardvark As Long, ByVal timeout_ms As Integer) as Long
    If check_version() Then
        aa_i2c_bus_timeout = vb6_aa_i2c_bus_timeout(aardvark, timeout_ms)
    Else
        aa_i2c_bus_timeout = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Read a stream of bytes from the I2C slave device.

Public Function aa_i2c_read (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_in() As Byte) as Long
    If check_version() Then
        Dim data_in_num_bytes As Integer
        data_in_num_bytes = tp_min(num_bytes, UBound(data_in) - LBound(data_in) + 1)
        aa_i2c_read = vb6_aa_i2c_read(aardvark, slave_addr, flags, data_in_num_bytes, data_in(0))
    Else
        aa_i2c_read = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Read a stream of bytes from the I2C slave device.
' This API function returns the number of bytes read into
' the num_read variable.  The return value of the function
' is a status code.

Public Function aa_i2c_read_ext (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_in() As Byte, ByRef num_read As Integer) as Long
    If check_version() Then
        Dim data_in_num_bytes As Integer
        data_in_num_bytes = tp_min(num_bytes, UBound(data_in) - LBound(data_in) + 1)
        aa_i2c_read_ext = vb6_aa_i2c_read_ext(aardvark, slave_addr, flags, data_in_num_bytes, data_in(0), num_read)
    Else
        aa_i2c_read_ext = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Write a stream of bytes to the I2C slave device.

Public Function aa_i2c_write (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_out() As Byte) as Long
    If check_version() Then
        Dim data_out_num_bytes As Integer
        data_out_num_bytes = tp_min(num_bytes, UBound(data_out) - LBound(data_out) + 1)
        aa_i2c_write = vb6_aa_i2c_write(aardvark, slave_addr, flags, data_out_num_bytes, data_out(0))
    Else
        aa_i2c_write = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Write a stream of bytes to the I2C slave device.
' This API function returns the number of bytes written into
' the num_written variable.  The return value of the function
' is a status code.

Public Function aa_i2c_write_ext (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal num_bytes As Integer, ByRef data_out() As Byte, ByRef num_written As Integer) as Long
    If check_version() Then
        Dim data_out_num_bytes As Integer
        data_out_num_bytes = tp_min(num_bytes, UBound(data_out) - LBound(data_out) + 1)
        aa_i2c_write_ext = vb6_aa_i2c_write_ext(aardvark, slave_addr, flags, data_out_num_bytes, data_out(0), num_written)
    Else
        aa_i2c_write_ext = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Do an atomic write+read to an I2C slave device by first
' writing a stream of bytes to the I2C slave device and then
' reading a stream of bytes back from the same slave device.
' This API function returns the number of bytes written into
' the num_written variable and the number of bytes read into
' the num_read variable.  The return value of the function is
' the status given as (read_status << 8) | (write_status).

Public Function aa_i2c_write_read (ByVal aardvark As Long, ByVal slave_addr As Integer, ByVal flags As Long, ByVal out_num_bytes As Integer, ByRef out_data() As Byte, ByRef num_written As Integer, ByVal in_num_bytes As Integer, ByRef in_data() As Byte, ByRef num_read As Integer) as Long
    If check_version() Then
        Dim out_data_out_num_bytes As Integer
        out_data_out_num_bytes = tp_min(out_num_bytes, UBound(out_data) - LBound(out_data) + 1)
        Dim in_data_in_num_bytes As Integer
        in_data_in_num_bytes = tp_min(in_num_bytes, UBound(in_data) - LBound(in_data) + 1)
        aa_i2c_write_read = vb6_aa_i2c_write_read(aardvark, slave_addr, flags, out_data_out_num_bytes, out_data(0), num_written, in_data_in_num_bytes, in_data(0), num_read)
    Else
        aa_i2c_write_read = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Enable/Disable the Aardvark as an I2C slave device

Public Function aa_i2c_slave_enable (ByVal aardvark As Long, ByVal addr As Byte, ByVal maxTxBytes As Integer, ByVal maxRxBytes As Integer) as Long
    If check_version() Then
        aa_i2c_slave_enable = vb6_aa_i2c_slave_enable(aardvark, addr, maxTxBytes, maxRxBytes)
    Else
        aa_i2c_slave_enable = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



Public Function aa_i2c_slave_disable (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_i2c_slave_disable = vb6_aa_i2c_slave_disable(aardvark)
    Else
        aa_i2c_slave_disable = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Set the slave response in the event the Aardvark is put
' into slave mode and contacted by a Master.

Public Function aa_i2c_slave_set_response (ByVal aardvark As Long, ByVal num_bytes As Byte, ByRef data_out() As Byte) as Long
    If check_version() Then
        Dim data_out_num_bytes As Byte
        data_out_num_bytes = tp_min(num_bytes, UBound(data_out) - LBound(data_out) + 1)
        aa_i2c_slave_set_response = vb6_aa_i2c_slave_set_response(aardvark, data_out_num_bytes, data_out(0))
    Else
        aa_i2c_slave_set_response = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Return number of bytes written from a previous
' Aardvark->I2C_master transmission.  Since the transmission is
' happening asynchronously with respect to the PC host
' software, there could be responses queued up from many
' previous write transactions.

Public Function aa_i2c_slave_write_stats (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_i2c_slave_write_stats = vb6_aa_i2c_slave_write_stats(aardvark)
    Else
        aa_i2c_slave_write_stats = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Read the bytes from an I2C slave reception

Public Function aa_i2c_slave_read (ByVal aardvark As Long, ByRef addr As Byte, ByVal num_bytes As Integer, ByRef data_in() As Byte) as Long
    If check_version() Then
        Dim data_in_num_bytes As Integer
        data_in_num_bytes = tp_min(num_bytes, UBound(data_in) - LBound(data_in) + 1)
        aa_i2c_slave_read = vb6_aa_i2c_slave_read(aardvark, addr, data_in_num_bytes, data_in(0))
    Else
        aa_i2c_slave_read = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Extended functions that return status code

Public Function aa_i2c_slave_write_stats_ext (ByVal aardvark As Long, ByRef num_written As Integer) as Long
    If check_version() Then
        aa_i2c_slave_write_stats_ext = vb6_aa_i2c_slave_write_stats_ext(aardvark, num_written)
    Else
        aa_i2c_slave_write_stats_ext = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



Public Function aa_i2c_slave_read_ext (ByVal aardvark As Long, ByRef addr As Byte, ByVal num_bytes As Integer, ByRef data_in() As Byte, ByRef num_read As Integer) as Long
    If check_version() Then
        Dim data_in_num_bytes As Integer
        data_in_num_bytes = tp_min(num_bytes, UBound(data_in) - LBound(data_in) + 1)
        aa_i2c_slave_read_ext = vb6_aa_i2c_slave_read_ext(aardvark, addr, data_in_num_bytes, data_in(0), num_read)
    Else
        aa_i2c_slave_read_ext = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Configure the I2C pullup resistors.
' This is only supported on hardware versions >= 2.00

Public Function aa_i2c_pullup (ByVal aardvark As Long, ByVal pullup_mask As Byte) as Long
    If check_version() Then
        aa_i2c_pullup = vb6_aa_i2c_pullup(aardvark, pullup_mask)
    Else
        aa_i2c_pullup = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



'==========================================================================
' SPI API
'==========================================================================
' Set the SPI bit rate in kilohertz.  If a zero is passed as the
' bitrate, the bitrate is unchanged and the current bitrate is
' returned.

Public Function aa_spi_bitrate (ByVal aardvark As Long, ByVal bitrate_khz As Long) as Long
    If check_version() Then
        aa_spi_bitrate = vb6_aa_spi_bitrate(aardvark, bitrate_khz)
    Else
        aa_spi_bitrate = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' These configuration parameters specify how to clock the
' bits that are sent and received on the Aardvark SPI
' interface.
'
'   The polarity option specifies which transition
'   constitutes the leading edge and which transition is the
'   falling edge.  For example, AA_SPI_POL_RISING_FALLING
'   would configure the SPI to idle the SCK clock line low.
'   The clock would then transition low-to-high on the
'   leading edge and high-to-low on the trailing edge.
'
'   The phase option determines whether to sample or setup on
'   the leading edge.  For example, AA_SPI_PHASE_SAMPLE_SETUP
'   would configure the SPI to sample on the leading edge and
'   setup on the trailing edge.
'
'   The bitorder option is used to indicate whether LSB or
'   MSB is shifted first.
'
' See the diagrams in the Aardvark datasheet for
' more details.
' Configure the SPI master or slave interface

Public Function aa_spi_configure (ByVal aardvark As Long, ByVal polarity As Long, ByVal phase As Long, ByVal bitorder As Long) as Long
    If check_version() Then
        aa_spi_configure = vb6_aa_spi_configure(aardvark, polarity, phase, bitorder)
    Else
        aa_spi_configure = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Write a stream of bytes to the downstream SPI slave device.

Public Function aa_spi_write (ByVal aardvark As Long, ByVal out_num_bytes As Integer, ByRef data_out() As Byte, ByVal in_num_bytes As Integer, ByRef data_in() As Byte) as Long
    If check_version() Then
        Dim data_out_out_num_bytes As Integer
        data_out_out_num_bytes = tp_min(out_num_bytes, UBound(data_out) - LBound(data_out) + 1)
        Dim data_in_in_num_bytes As Integer
        data_in_in_num_bytes = tp_min(in_num_bytes, UBound(data_in) - LBound(data_in) + 1)
        aa_spi_write = vb6_aa_spi_write(aardvark, data_out_out_num_bytes, data_out(0), data_in_in_num_bytes, data_in(0))
    Else
        aa_spi_write = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Enable/Disable the Aardvark as an SPI slave device

Public Function aa_spi_slave_enable (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_spi_slave_enable = vb6_aa_spi_slave_enable(aardvark)
    Else
        aa_spi_slave_enable = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



Public Function aa_spi_slave_disable (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_spi_slave_disable = vb6_aa_spi_slave_disable(aardvark)
    Else
        aa_spi_slave_disable = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Set the slave response in the event the Aardvark is put
' into slave mode and contacted by a Master.

Public Function aa_spi_slave_set_response (ByVal aardvark As Long, ByVal num_bytes As Byte, ByRef data_out() As Byte) as Long
    If check_version() Then
        Dim data_out_num_bytes As Byte
        data_out_num_bytes = tp_min(num_bytes, UBound(data_out) - LBound(data_out) + 1)
        aa_spi_slave_set_response = vb6_aa_spi_slave_set_response(aardvark, data_out_num_bytes, data_out(0))
    Else
        aa_spi_slave_set_response = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Read the bytes from an SPI slave reception

Public Function aa_spi_slave_read (ByVal aardvark As Long, ByVal num_bytes As Integer, ByRef data_in() As Byte) as Long
    If check_version() Then
        Dim data_in_num_bytes As Integer
        data_in_num_bytes = tp_min(num_bytes, UBound(data_in) - LBound(data_in) + 1)
        aa_spi_slave_read = vb6_aa_spi_slave_read(aardvark, data_in_num_bytes, data_in(0))
    Else
        aa_spi_slave_read = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Change the output polarity on the SS line.
'
' Note: When configured as an SPI slave, the Aardvark will
' always be setup with SS as active low.  Hence this function
' only affects the SPI master functions on the Aardvark.

Public Function aa_spi_master_ss_polarity (ByVal aardvark As Long, ByVal polarity As Long) as Long
    If check_version() Then
        aa_spi_master_ss_polarity = vb6_aa_spi_master_ss_polarity(aardvark, polarity)
    Else
        aa_spi_master_ss_polarity = AA_INCOMPATIBLE_LIBRARY
    End If
End Function



'==========================================================================
' GPIO API
'==========================================================================
' The following enumerated type maps the named lines on the
' Aardvark I2C/SPI line to bit positions in the GPIO API.
' All GPIO API functions will index these lines through an
' 8-bit masked value.  Thus, each bit position in the mask
' can be referred back its corresponding line through the
' enumerated type.
' Configure the GPIO, specifying the direction of each bit.
'
' A call to this function will not change the value of the pullup
' mask in the Aardvark.  This is illustrated by the following
' example:
'   (1) Direction mask is first set to 0x00
'   (2) Pullup is set to 0x01
'   (3) Direction mask is set to 0x01
'   (4) Direction mask is later set back to 0x00.
'
' The pullup will be active after (4).
'
' On Aardvark power-up, the default value of the direction
' mask is 0x00.

Public Function aa_gpio_direction (ByVal aardvark As Long, ByVal direction_mask As Byte) as Long
    If check_version() Then
        aa_gpio_direction = vb6_aa_gpio_direction(aardvark, direction_mask)
    Else
        aa_gpio_direction = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Enable an internal pullup on any of the GPIO input lines.
'
' Note: If a line is configured as an output, the pullup bit
' for that line will be ignored, though that pullup bit will
' be cached in case the line is later configured as an input.
'
' By default the pullup mask is 0x00.

Public Function aa_gpio_pullup (ByVal aardvark As Long, ByVal pullup_mask As Byte) as Long
    If check_version() Then
        aa_gpio_pullup = vb6_aa_gpio_pullup(aardvark, pullup_mask)
    Else
        aa_gpio_pullup = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Read the current digital values on the GPIO input lines.
'
' The bits will be ordered as described by AA_GPIO_BITS.  If a
' line is configured as an output, its corresponding bit
' position in the mask will be undefined.

Public Function aa_gpio_get (ByVal aardvark As Long) as Long
    If check_version() Then
        aa_gpio_get = vb6_aa_gpio_get(aardvark)
    Else
        aa_gpio_get = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Set the outputs on the GPIO lines.
'
' Note: If a line is configured as an input, it will not be
' affected by this call, but the output value for that line
' will be cached in the event that the line is later
' configured as an output.

Public Function aa_gpio_set (ByVal aardvark As Long, ByVal value As Byte) as Long
    If check_version() Then
        aa_gpio_set = vb6_aa_gpio_set(aardvark, value)
    Else
        aa_gpio_set = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


' Block until there is a change on the GPIO input lines.
' Pins configured as outputs will be ignored.
'
' The function will return either when a change has occurred or
' the timeout expires.  The timeout, specified in millisecods, has
' a precision of ~16 ms. The maximum allowable timeout is
' approximately 4 seconds. If the timeout expires, this function
' will return the current state of the GPIO lines.
'
' This function will return immediately with the current value
' of the GPIO lines for the first invocation after any of the
' following functions are called: aa_configure,
' aa_gpio_direction, or aa_gpio_pullup.
'
' If the function aa_gpio_get is called before calling
' aa_gpio_change, aa_gpio_change will only register any changes
' from the value last returned by aa_gpio_get.

Public Function aa_gpio_change (ByVal aardvark As Long, ByVal timeout As Integer) as Long
    If check_version() Then
        aa_gpio_change = vb6_aa_gpio_change(aardvark, timeout)
    Else
        aa_gpio_change = AA_INCOMPATIBLE_LIBRARY
    End If
End Function


