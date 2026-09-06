<#
FlexNode SWD helper (Windows, xPack OpenOCD, ST-Link V2 clone bound to WinUSB via Zadig).
Mirrors moteus-r4-parent/fw/flash.py and fw/program_option_bytes.sh exactly.
Images come from tools/bench/out/ (written by build_fw.sh / export_bins.sh).

Run from the Claude Code "!" prompt (Git Bash) as:
  /c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -ExecutionPolicy Bypass -File C:/Users/adity/Documents/CATBOT/FlexNode/Dev/moteus/tools/bench/flexnode-swd.ps1 <mode>

  probe        read-only: IDCODE, option bytes, first flash words. Run after any harness change.
  probeslow    same at 300 kHz SWD, for a marginal link
  diag         read-only: halt/sample PC/resume x10 over ~2 s (alive vs stuck, and where)
  verify       read-only: byte-compare flash against the three images
  flash        program + verify all three images, then reset
  optionbytes  clear nSWBOOT0 -> always boot from flash (PB8/BOOT0 is held high by the I2C pull-up)
  ledtest      program ledtest/ledtest.bin (948 B bare-metal WS2812 chime) at 0x08000000 and reset
  erase        mass-erase bank 0; explicit, never implied

See notes/build-and-flash.md and notes/debugging-over-swd.md.
#>
param([Parameter(Mandatory=$true)][ValidateSet('probe','probeslow','flash','optionbytes','verify','ledtest','erase','diag')][string]$Mode)

$ErrorActionPreference = 'Continue'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$oc = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\xpack-dev-tools.openocd-xpack*" -Recurse -Filter openocd.exe | Select-Object -First 1 -ExpandProperty FullName
if (-not $oc) { throw "openocd.exe not found (winget install xpack-dev-tools.openocd-xpack)" }

$b0 = ($here + '/out/out.08000000.bin').Replace('\','/')
$bl = ($here + '/out/out.0800c000.bin').Replace('\','/')
$b1 = ($here + '/out/out.08010000.bin').Replace('\','/')
if ($Mode -in 'flash','verify') { foreach ($f in @($b0,$bl,$b1)) { if (-not (Test-Path $f)) { throw "missing image $f (run build_fw.sh)" } } }

# gdb/tcl/telnet ports disabled: a stale openocd once held 3333 and every run failed to bind.
# reset_config none separate: SWD only; openocd never drives NRST.
$common = @('-f','interface/stlink.cfg','-f','target/stm32g4x.cfg',
            '-c','gdb_port disabled','-c','tcl_port disabled','-c','telnet_port disabled',
            '-c','init','-c','reset_config none separate')

switch ($Mode) {
  'probe' {
    # FLASH_OPTR @0x40022020: bit22 DBANK, bit26 nSWBOOT0 (0 = boot from flash), bit27 nBOOT0
    $cmds = @('-c','halt','-c','echo ==DBGMCU_IDCODE==','-c','mdw 0xE0042000',
              '-c','echo ==FLASH_OPTR_bit26_nSWBOOT0_bit27_nBOOT0_bit22_DBANK==','-c','mdw 0x40022020',
              '-c','echo ==FLASH_0x08000000==','-c','mdw 0x08000000 16',
              '-c','resume','-c','exit')
  }
  'probeslow' {
    $common = @('-f','interface/stlink.cfg','-f','target/stm32g4x.cfg',
                '-c','gdb_port disabled','-c','tcl_port disabled','-c','telnet_port disabled',
                '-c','adapter speed 300','-c','init','-c','reset_config none separate')
    $cmds = @('-c','halt','-c','echo ==DBGMCU_IDCODE==','-c','mdw 0xE0042000',
              '-c','echo ==FLASH_OPTR==','-c','mdw 0x40022020','-c','resume','-c','exit')
  }
  'diag' {
    $cmds = @('-c','halt','-c','reg pc','-c','resume')
    for ($i = 0; $i -lt 9; $i++) {
      $cmds += @('-c','sleep 180','-c','halt','-c','reg pc','-c','resume')
    }
    $cmds += @('-c','exit')
  }
  'flash' {
    $cmds = @('-c','halt',
              '-c',"program $b0 verify 0x08000000",
              '-c',"program $bl verify 0x0800c000",
              '-c',"program $b1 verify 0x08010000",
              '-c','reset','-c','exit')
  }
  'verify' {
    $cmds = @('-c','halt',
              '-c',"verify_image $b0 0x08000000 bin",
              '-c',"verify_image $bl 0x0800c000 bin",
              '-c',"verify_image $b1 0x08010000 bin",
              '-c','resume','-c','exit')
  }
  'ledtest' {
    $lt = ($here + '/ledtest/ledtest.bin').Replace('\','/')
    if (-not (Test-Path $lt)) { throw "missing $lt (run ledtest/build.sh)" }
    $cmds = @('-c','halt','-c',"program $lt verify 0x08000000",'-c','reset','-c','exit')
  }
  'erase' {
    $cmds = @('-c','halt','-c','stm32l4x mass_erase 0','-c','exit')
  }
  'optionbytes' {
    # identical to fw/program_option_bytes.sh: clear nSWBOOT0 (mask 0x04000000) in option word 0x20
    $cmds = @('-c','reset halt',
              '-c','stm32l4x option_write 0 0x20 0x00000000 0x04000000',
              '-c','stm32l4x option_load 0',
              '-c','reset','-c','exit')
  }
}
Write-Host ">> $oc $($common + $cmds -join ' ')"
& $oc @common @cmds
exit $LASTEXITCODE
