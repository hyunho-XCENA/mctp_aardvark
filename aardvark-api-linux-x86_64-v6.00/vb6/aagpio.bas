Attribute VB_Name = "aagpio"
'==========================================================================
' (c) 2004-2006  Total Phase, Inc.
'--------------------------------------------------------------------------
' Project : Aardvark Sample Code
' File    : aagpio.bas
'--------------------------------------------------------------------------
' Perform some simple GPIO operations with a single Aardvark adapter.
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
' MAIN PROGRAM
'==========================================================================
Sub aagpio_run()
    Dim handle As Long
    Dim aaext As AardvarkExt

    ' Open the device
    handle = aa_open_ext(0, aaext)
    If handle <= 0 Then
        Debug.Print ("Unable to open Aardvark device on port 0")
        Debug.Print ("Error code = " & handle)
        Exit Sub
    End If
    
    Debug.Print "Opened Aardvark adapter; features = " & aaext.features

    ' Configure the Aardvark adapter so all pins
    ' are now controlled by the GPIO subsystem
    Call aa_configure(handle, AA_CONFIG_GPIO_ONLY)
    
    ' Configure the Aardvark adapter so all pins
    ' are now controlled by the GPIO subsystem
    Call aa_configure(handle, AA_CONFIG_GPIO_ONLY)

    ' Turn off the external I2C line pullups
    Call aa_i2c_pullup(handle, AA_I2C_PULLUP_NONE)

    ' Make sure the charge has dissipated on those lines
    Call aa_gpio_set(handle, &H0)
    Call aa_gpio_direction(handle, &HFF)

    ' By default all GPIO pins are inputs.  Writing 1 to the
    ' bit position corresponding to the appropriate line will
    ' configure that line as an output
    Call aa_gpio_direction(handle, AA_GPIO_SS Or AA_GPIO_SCL)

    ' By default all GPIO outputs are logic low.  Writing a 1
    ' to the appropriate bit position will force that line
    ' high provided it is configured as an output.  If it is
    ' not configured as an output the line state will be
    ' cached such that if the direction later changed, the
    ' latest output value for the line will be enforced.
    Call aa_gpio_set(handle, AA_GPIO_SCL)
    Debug.Print "Setting SCL to logic low"

    ' The get method will return the line states of all inputs.
    ' If a line is not configured as an input the value of
    ' that particular bit position in the mask will be 0.
    Dim val As Long
    val = aa_gpio_get(handle)

    ' Check the state of SCK
    If (val And AA_GPIO_SCK) Then
        Debug.Print "Read the SCK line as logic high"
    Else:
        Debug.Print "Read the SCK line as logic low"
    End If
    
    ' Optionally we can set passive pullups on certain lines.
    ' This can prevent input lines from floating.  The pullup
    ' configuration is only valid for lines configured as inputs.
    ' If the line is not currently input the requested pullup
    ' state will take effect only if the line is later changed
    ' to be an input line.
    Call aa_gpio_pullup(handle, AA_GPIO_MISO Or AA_GPIO_MOSI)

    ' Now reading the MISO line should give a logic high,
    ' provided there is nothing attached to the Aardvark
    ' adapter that is driving the pin low.
    val = aa_gpio_get(handle)
    If (val And AA_GPIO_MISO) Then
        Debug.Print "Read the MISO line as logic high (passive pullup)"
    Else
        Debug.Print "Read the MISO line as logic low (is pin driven low?)"
    End If

    ' Now do a 1000 gets from the GPIO to test performance
    Debug.Print "Doing 1000 iterations of GPIO read..."
    For i = 1 To 1000
        aa_gpio_get (handle)
    Next
    
    ' Demonstrate use of aa_gpio_change
    Call aa_gpio_direction(handle, &H0)
    Dim oldval As Long
    Dim newval As Long
    oldval = aa_gpio_get(handle)
    Debug.Print "Calling aa_gpio_change for 2 seconds..."
    newval = aa_gpio_change(handle, 2000)
    If (newval <> oldval) Then
        Debug.Print "  GPIO inputs changed."
    Else
        Debug.Print "  GPIO inputs did not change."
    End If

    ' Turn on the I2C line pullups since we are done
    Call aa_i2c_pullup(handle, AA_I2C_PULLUP_BOTH)

    ' Configure the Aardvark adapter back to SPI/I2C mode.
    Call aa_configure(handle, AA_CONFIG_SPI_I2C)

    ' Close the device
    aa_close (handle)
    Debug.Print "Finished"
End Sub
