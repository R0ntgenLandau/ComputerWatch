param(
    [switch] $Uninstall,
    [switch] $RemovePawnIO,
    [string] $TargetSid
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$source = Join-Path $PSScriptRoot 'build/ComputerWatch.exe'
$mcpSource = Join-Path $PSScriptRoot 'build/mcp_server.py'
if (!$Uninstall -and (!(Test-Path -LiteralPath $source -PathType Leaf) -or
    !(Test-Path -LiteralPath $mcpSource -PathType Leaf))) {
    throw 'Run build.ps1 before installing.'
}

if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $systemPowerShell = Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell/v1.0/powershell.exe'
    $installArguments = @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'), '-TargetSid', $identity.User.Value)
    if ($Uninstall) { $installArguments += '-Uninstall' }
    if ($RemovePawnIO) { $installArguments += '-RemovePawnIO' }
    $setupProcess = Start-Process -FilePath $systemPowerShell -ArgumentList $installArguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    if ($setupProcess.ExitCode -ne 0) { throw "ComputerWatch setup failed (exit $($setupProcess.ExitCode))." }
    return
}

if ($TargetSid -and $TargetSid -ne $identity.User.Value) {
    throw 'Setup must be elevated using the same Windows account that will run ComputerWatch.'
}

$directory = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)) 'ComputerWatch'
$executable = Join-Path $directory 'ComputerWatch.exe'
$mcpScript = Join-Path $directory 'mcp_server.py'
$dashboardTask = 'ComputerWatch Dashboard'
$sensorTask = 'ComputerWatch CPU Sensors'
$taskNames = @($dashboardTask, $sensorTask)

foreach ($path in @($directory, $executable, $mcpScript,
    (Join-Path $directory 'dependencies'), (Join-Path $directory 'dependencies/PawnIO_setup.exe'))) {
    $item = Get-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Refusing to use a redirected install path: $path"
    }
}

foreach ($name in $taskNames) {
    $task = Get-ScheduledTask -TaskName $name -TaskPath '\' -ErrorAction SilentlyContinue
    if (!$task) { continue }
    $registeredUser = $task.Principal.UserId
    if ($registeredUser -and $registeredUser -notlike 'S-1-*') {
        $registeredUser = [Security.Principal.NTAccount]::new($registeredUser).Translate(
            [Security.Principal.SecurityIdentifier]).Value
    }
    if ($registeredUser -ne $identity.User.Value -or
        @($task.Actions).Count -ne 1 -or $task.Actions.Execute -ne $executable) {
        throw "A different application or user owns task '$name'."
    }
    if ($task.State -eq 'Running') {
        # Stop only ETW sessions belonging to this verified installation's sensor helper.
        if ($name -eq $sensorTask) {
            Get-CimInstance Win32_Process -Filter "Name = 'ComputerWatch.exe'" |
                Where-Object { $_.ExecutablePath -eq $executable -and $_.CommandLine -match '--cpu-sensors' } |
                ForEach-Object {
                    & "$env:SystemRoot/System32/logman.exe" stop "ComputerWatch.Network.$($_.ProcessId)" -ets 2>$null | Out-Null
                }
        }
        Stop-ScheduledTask -InputObject $task
    }
    if ($Uninstall) {
        Unregister-ScheduledTask -InputObject $task -Confirm:$false
    }
}

if ($Uninstall) {
    if ($RemovePawnIO) {
        $dependency = Join-Path $directory 'dependencies/PawnIO_setup.exe'
        if (!(Test-Path $dependency) -or
            (Get-FileHash $dependency).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032') {
            throw 'The bundled PawnIO uninstaller could not be verified.'
        }
        $removal = Start-Process -FilePath $dependency -ArgumentList '-uninstall -silent' -WindowStyle Hidden -Wait -PassThru
        if ($removal.ExitCode -notin 0, 3010) { throw "PawnIO removal failed ($($removal.ExitCode))." }
    }
    $dependency = Join-Path $directory 'dependencies/PawnIO_setup.exe'
    if (Test-Path $dependency) { Remove-Item -LiteralPath $dependency }
    $dependencyDirectory = Join-Path $directory 'dependencies'
    if ((Test-Path $dependencyDirectory) -and !(Get-ChildItem -LiteralPath $dependencyDirectory -Force)) {
        Remove-Item -LiteralPath $dependencyDirectory
    }
    if (Test-Path -LiteralPath $executable) { Remove-Item -LiteralPath $executable }
    if (Test-Path -LiteralPath $mcpScript -PathType Leaf) { Remove-Item -LiteralPath $mcpScript }
    if ((Test-Path -LiteralPath $directory) -and !(Get-ChildItem -Force -LiteralPath $directory)) {
        Remove-Item -LiteralPath $directory
    }
    return
}

New-Item -ItemType Directory -Path $directory -Force | Out-Null
$dependencySource = Join-Path $PSScriptRoot 'build/dependencies/PawnIO_setup.exe'
if (Test-Path $dependencySource) {
    if ((Get-FileHash $dependencySource).Hash -ne '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032' -or
        (Get-AuthenticodeSignature $dependencySource).Status -ne 'Valid') {
        throw 'Bundled PawnIO installer verification failed.'
    }
    $dependencyDirectory = Join-Path $directory 'dependencies'
    New-Item -ItemType Directory -Path $dependencyDirectory -Force | Out-Null
    $dependency = Join-Path $dependencyDirectory 'PawnIO_setup.exe'
    Copy-Item -LiteralPath $dependencySource -Destination $dependency -Force
    $existingPawn = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO' -ErrorAction SilentlyContinue
    if (!$existingPawn -or [version]$existingPawn.DisplayVersion -lt [version]'2.2.0') {
        $setup = Start-Process -FilePath $dependency -ArgumentList '-install -silent' -WindowStyle Hidden -Wait -PassThru
        if ($setup.ExitCode -notin 0, 3010) { throw "PawnIO installation failed ($($setup.ExitCode))." }
        if ($setup.ExitCode -eq 3010) { Write-Warning 'PawnIO requires a restart to finish installation.' }
    }
}
Copy-Item -LiteralPath $source -Destination $executable -Force
Copy-Item -LiteralPath $mcpSource -Destination $mcpScript -Force

$trigger = New-ScheduledTaskTrigger -AtLogOn -User $identity.Name
$settings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
$dashboardAction = New-ScheduledTaskAction -Execute $executable -Argument '--tray' -WorkingDirectory $directory
$sensorAction = New-ScheduledTaskAction -Execute $executable -Argument '--cpu-sensors' -WorkingDirectory $directory
$dashboardPrincipal = New-ScheduledTaskPrincipal -UserId $identity.User.Value -LogonType Interactive -RunLevel Limited
$sensorPrincipal = New-ScheduledTaskPrincipal -UserId $identity.User.Value -LogonType Interactive -RunLevel Highest

Register-ScheduledTask -TaskName $dashboardTask -Action $dashboardAction -Trigger $trigger -Settings $settings -Principal $dashboardPrincipal -Force | Out-Null
Register-ScheduledTask -TaskName $sensorTask -Action $sensorAction -Trigger $trigger -Settings $settings -Principal $sensorPrincipal -Force | Out-Null

Start-ScheduledTask -TaskName $dashboardTask
Start-ScheduledTask -TaskName $sensorTask
