VERSION 5.00
Begin VB.Form main 
   Caption         =   "Aardvark I2C/SPI Activity Board"
   ClientHeight    =   3270
   ClientLeft      =   60
   ClientTop       =   345
   ClientWidth     =   7830
   LinkTopic       =   "Form1"
   ScaleHeight     =   3270
   ScaleWidth      =   7830
   StartUpPosition =   3  'Windows Default
   Begin VB.CommandButton Command2 
      Caption         =   "aalights"
      Height          =   735
      Left            =   2880
      TabIndex        =   1
      Top             =   1080
      Width           =   2055
   End
   Begin VB.CommandButton Command1 
      Caption         =   "aadetect"
      Height          =   735
      Left            =   240
      TabIndex        =   0
      Top             =   1080
      Width           =   2175
   End
   Begin VB.CommandButton Command3 
      Caption         =   "aagpio"
      Height          =   735
      Left            =   5400
      TabIndex        =   3
      Top             =   1080
      Width           =   2175
   End
   Begin VB.CommandButton Command4 
      Caption         =   "aai2c_eeprom"
      Height          =   735
      Left            =   1440
      TabIndex        =   4
      Top             =   2280
      Width           =   2175
   End
   Begin VB.CommandButton Command5 
      Caption         =   "aaspi_eeprom"
      Height          =   735
      Left            =   4200
      TabIndex        =   5
      Top             =   2280
      Width           =   2175
   End
   Begin VB.Label Label1 
      Alignment       =   2  'Center
      Caption         =   "Aardvark I2C/SPI Activity Board"
      BeginProperty Font 
         Name            =   "MS Sans Serif"
         Size            =   18
         Charset         =   0
         Weight          =   700
         Underline       =   0   'False
         Italic          =   0   'False
         Strikethrough   =   0   'False
      EndProperty
      Height          =   495
      Left            =   120
      TabIndex        =   2
      Top             =   240
      Width           =   7575
   End
End
Attribute VB_Name = "main"
Attribute VB_GlobalNameSpace = False
Attribute VB_Creatable = False
Attribute VB_PredeclaredId = True
Attribute VB_Exposed = False
Private Sub Command1_Click()
    Call aadetect_run
End Sub

Private Sub Command2_Click()
    Call aalights_run
End Sub

Private Sub Command3_Click()
    Call aagpio_run
End Sub

Private Sub Command4_Click()
    Call aai2c_eeprom_run
End Sub

Private Sub Command5_Click()
    Call aaspi_eeprom_run
End Sub

