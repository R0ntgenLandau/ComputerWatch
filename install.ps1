param(
    [switch] $Uninstall,
    [string] $TargetSid
)

$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$source = Join-Path $PSScriptRoot 'build/ComputerWatch.exe'
if (!$Uninstall -and !(Test-Path -LiteralPath $source -PathType Leaf)) {
    throw 'Run build.ps1 before installing.'
}

if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $systemPowerShell = Join-Path ([Environment]::SystemDirectory) 'WindowsPowerShell/v1.0/powershell.exe'
    $installArguments = @('-NoProfile', '-File', ('"' + $PSCommandPath + '"'), '-TargetSid', $identity.User.Value)
    if ($Uninstall) { $installArguments += '-Uninstall' }
    $setupProcess = Start-Process -FilePath $systemPowerShell -ArgumentList $installArguments -Verb RunAs -WindowStyle Hidden -Wait -PassThru
    if ($setupProcess.ExitCode -ne 0) { throw "ComputerWatch setup failed (exit $($setupProcess.ExitCode))." }
    return
}

if ($TargetSid -and $TargetSid -ne $identity.User.Value) {
    throw 'Setup must be elevated using the same Windows account that will run ComputerWatch.'
}

$directory = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)) 'ComputerWatch'
$executable = Join-Path $directory 'ComputerWatch.exe'
$dashboardTask = 'ComputerWatch Dashboard'
$sensorTask = 'ComputerWatch CPU Sensors'
$taskNames = @($dashboardTask, $sensorTask)

foreach ($path in @($directory, $executable)) {
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
        Stop-ScheduledTask -InputObject $task
    }
    if ($Uninstall) {
        Unregister-ScheduledTask -InputObject $task -Confirm:$false
    }
}

if ($Uninstall) {
    if (Test-Path -LiteralPath $executable) { Remove-Item -LiteralPath $executable }
    if ((Test-Path -LiteralPath $directory) -and !(Get-ChildItem -Force -LiteralPath $directory)) {
        Remove-Item -LiteralPath $directory
    }
    return
}

New-Item -ItemType Directory -Path $directory -Force | Out-Null
Copy-Item -LiteralPath $source -Destination $executable -Force

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
