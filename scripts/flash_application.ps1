param([Parameter(Mandatory = $true)][string]$Firmware)

$firmwarePath = (Resolve-Path -LiteralPath $Firmware -ErrorAction Stop).Path
STM32_Programmer_CLI.exe -c port=SWD -w $firmwarePath 0x08010000 -v
exit $LASTEXITCODE