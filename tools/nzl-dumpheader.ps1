<#
Разбор заголовка Windows crash dump без WinDbg и без символов.

Почему это отдельный файл: стоп-код и его четыре параметра лежат в первых
0x60 байтах любого дампа (kernel dump, MEMORY.DMP, и маленький 64 КБ
minidump из C:\Windows\Minidump). WinDbg на машине владельца нет, а
локализованное сообщение события 1001 прячет числа в текст. Читаем байты
напрямую - получаем числа.

Раскладка _DUMP_HEADER64 (x64):
  0x00 'PAGE'          0x04 'DU64'        0x08 Major     0x0C Minor
  0x10 DirectoryTableBase   0x18 PfnDataBase
  0x20 PsLoadedModuleList   0x28 PsActiveProcessHead
  0x30 MachineImageType     0x34 NumberProcessors
  0x38 BugCheckCode         0x3C (padding)
  0x40..0x58 BugCheckParameter1..4  (по 8 байт)

x86-дампы ('DUMP') здесь сознательно не разбираются: на машине владельца
x64, а угадывать смещения для платформы, которой нет, - это как раз тот
случай, когда лучше явно вернуть "не умею".
#>

function Format-NzlBugCheck([uint32]$Code) {
    # Имя + какие параметры что значат. Только те коды, которые у нас уже
    # были или вероятны при загрузке вручную отображённого драйвера.
    switch ($Code) {
        0x0000001E { return 'KMODE_EXCEPTION_NOT_HANDLED  p1=exception code  p2=address that raised it  p4=instruction' }
        0x0000000A { return 'IRQL_NOT_LESS_OR_EQUAL        p1=referenced memory  p2=IRQL  p3=1 write/0 read  p4=instruction' }
        0x00000050 { return 'PAGE_FAULT_IN_NONPAGED_AREA   p1=referenced memory  p2=1 write/0 read  p3=instruction' }
        0x000000D1 { return 'DRIVER_IRQL_NOT_LESS_OR_EQUAL  p1=referenced memory  p2=IRQL  p3=1 write/0 read/8 execute  p4=instruction' }
        0x0000007E { return 'SYSTEM_THREAD_EXCEPTION       p1=exception code  p2=address  p4=instruction' }
        0x0000003B { return 'SYSTEM_SERVICE_EXCEPTION      p1=exception code  p2=instruction  p3=context record' }
        0x000000BE { return 'ATTEMPTED_WRITE_TO_READONLY_MEMORY  p1=VA  p2=PTE  p3=reserved  p4=instruction' }
        0x000000C2 { return 'BAD_POOL_CALLER               p1=type  p2=pool tag high  p3=pool tag low' }
        0x000000C4 { return 'DRIVER_VERIFIER_DETECTED_VIOLATION  p1=violation class (special pool/overrun/etc)' }
        0x000000C5 { return 'DRIVER_CORRUPTED_EXPOOL       p1=referenced memory  p2=IRQL  p3=1 write/0 read  p4=instruction' }
        0x0000001A { return 'MEMORY_MANAGEMENT             p1=subtype (0x41793 etc.)' }
        0x0000004E { return 'PFN_LIST_CORRUPT             p1=subtype  p2=page frame number' }
        0x00000109 { return 'CRITICAL_STRUCTURE_CORRUPTION  p1=type  p2=address  p3=expected  p4=actual' }
        0x00000133 { return 'DPC_WATCHDOG_VIOLATION        p1=0 single DPC / 1 time limit  p2..p4 timings' }
        0x00000139 { return 'KERNEL_SECURITY_CHECK_FAILURE p1=subtype  p2=address of trap frame' }
        0x000000EF { return 'CRITICAL_PROCESS_DIED         p1=PEPROCESS of the dead process' }
        0x0000009F { return 'DRIVER_POWER_STATE_FAILURE    p1=0x3 device object stuck in a power transition' }
        0x000000ED { return 'UNMOUNTABLE_BOOT_VOLUME' }
        0x000000F4 { return 'CRITICAL_OBJECT_TERMINATION   p1=0/3  p2=PEPROCESS  p3=ETHREAD' }
        default    { return 'unknown code - send me the four parameters as they are' }
    }
}

function Read-NzlDumpHeader([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    $fs = $null
    try {
        $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read,
                                     [System.IO.FileShare]::ReadWrite)
        $h = New-Object byte[] 0x60
        $read = 0
        while ($read -lt 0x60) {
            $n = $fs.Read($h, $read, 0x60 - $read)
            if ($n -le 0) { break }
            $read += $n
        }
        if ($read -lt 0x60) { return $null }
        # 'PAGE'
        if ($h[0] -ne 0x50 -or $h[1] -ne 0x41 -or $h[2] -ne 0x47 -or $h[3] -ne 0x45) { return $null }
        $valid = [System.BitConverter]::ToUInt32($h, 4)
        if ($valid -ne 0x34365544) {          # 'DU64'
            return [pscustomobject]@{ Path = $Path; IsX64 = $false; Code = 0; Reason = 'not an x64 dump header' }
        }
        return [pscustomobject]@{
            Path      = $Path
            IsX64     = $true
            Major     = [System.BitConverter]::ToUInt32($h, 0x08)
            Minor     = [System.BitConverter]::ToUInt32($h, 0x0C)
            Processors= [System.BitConverter]::ToUInt32($h, 0x34)
            Code      = [System.BitConverter]::ToUInt32($h, 0x38)
            P1        = [System.BitConverter]::ToUInt64($h, 0x40)
            P2        = [System.BitConverter]::ToUInt64($h, 0x48)
            P3        = [System.BitConverter]::ToUInt64($h, 0x50)
            P4        = [System.BitConverter]::ToUInt64($h, 0x58)
            Reason    = ''
        }
    } catch {
        return [pscustomobject]@{ Path = $Path; IsX64 = $false; Code = 0; Reason = $_.Exception.Message }
    } finally {
        if ($fs) { $fs.Dispose() }
    }
}

function Format-NzlDumpHeader($Header) {
    if (-not $Header) { return 'dump header: unreadable or not a dump' }
    if (-not $Header.IsX64) { return "dump header: $($Header.Reason) ($($Header.Path))" }
    $ver = "$($Header.Major).$($Header.Minor)"
    $line = 'BUGCHECK 0x{0:X8}  {1}' -f $Header.Code, (Format-NzlBugCheck $Header.Code)
    return @(
        "dump: $($Header.Path)  header v$ver  cpus=$($Header.Processors)"
        $line
        ('  p1=0x{0:X16}  p2=0x{1:X16}' -f $Header.P1, $Header.P2)
        ('  p3=0x{0:X16}  p4=0x{1:X16}' -f $Header.P3, $Header.P4)
    ) | Out-String
}
