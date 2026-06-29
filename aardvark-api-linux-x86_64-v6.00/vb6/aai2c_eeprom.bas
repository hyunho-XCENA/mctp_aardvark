Attribute VB_Name = "aai2c_eeprom"
'==========================================================================
' (c) 2004  Total Phase, Inc.
'--------------------------------------------------------------------------
' Project : Aardvark Sample Code
' File    : aai2c_eeprom.bas
'--------------------------------------------------------------------------
' Perform simple read and write operations to an I2C EEPROM device.
'--------------------------------------------------------------------------
' Redistribution and use of this file in source and binary forms, with
' or without modification, are permitted.
'
' THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
' "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
' LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
' FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
' COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
' INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
' BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
' LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
' CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
' LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
' ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
' POSSIBILITY OF SUCH DAMAGE.
'==========================================================================

'==========================================================================
' CONSTANTS
'==========================================================================
Const I2C_BITRATE     = 100  'kHz
Const I2C_BUS_TIMEOUT = 150  'ms
Const I2C_DEVICE      = &H50


'==========================================================================
' MAIN PROGRAM
'==========================================================================
Sub aai2c_eeprom_run()
    Dim handle As Long

    handle = aa_open(0)
    If (handle <= 0) Then
        Debug.Print ("Unable to open Aardvark device on port 0")
        Debug.Print ("Error code = " & handle)
        Exit Sub
    End If
    
    ' Ensure that the I2C subsystem is enabled
    Call aa_configure(handle, AA_CONFIG_SPI_I2C)
    
    ' Enable the I2C bus pullup resistors (2.2k resistors).
    ' This command is only effective on v2.0 hardware or greater.
    ' The pullup resistors on the v1.02 hardware are enabled by default.
    Call aa_i2c_pullup(handle, AA_I2C_PULLUP_BOTH)

    ' Power the board using the Aardvark adapter's power supply.
    ' This command is only effective on v2.0 hardware or greater.
    ' The power pins on the v1.02 hardware are not enabled by default.
    Call aa_target_power(handle, AA_TARGET_POWER_BOTH)

    ' Set the bitrate
    Dim bitrate As Long
    bitrate = aa_i2c_bitrate(handle, I2C_BITRATE)
    Debug.Print ("Bitrate set to " & bitrate & " kHz")

    ' Set the bus lock timeout
    Dim bus_timeout As Long
    bus_timeout = aa_i2c_bus_timeout(handle, I2C_BUS_TIMEOUT)
    Debug.Print ("Bus lock timeout set to " & bus_timeout & " ms")

    ' Write the offset and read the data
    Dim offset(0) As Byte
    Dim data(15) As Byte
    Dim result As Long

    offset(0) = 0
    Call aa_i2c_write(handle, I2C_DEVICE, AA_I2C_NO_STOP, 1, offset)

    result = aa_i2c_read(handle, I2C_DEVICE, AA_I2C_NO_FLAGS, 16, data)
    If result <= 0 Then
        Debug.Print "i2c read error"
    Else
        Dim i As Integer
        Debug.Print "Read data bytes:"
        For i = 0 To 15
            Debug.Print (data(i))
        Next
    End If

    ' Close the device and exit
    aa_close (handle)
End Sub
