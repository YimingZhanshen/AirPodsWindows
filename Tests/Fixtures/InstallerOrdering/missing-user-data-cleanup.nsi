; Installer ordering gate fixture: valid ordering, missing uninstall cleanup.
System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "Global\AirPodsWindows_InstanceMutex") p.R0 ?e'
Pop $R2
nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM "AirPodsWindows.exe"'
Pop $R2
File /r runtime

Section "Uninstall"
