; Installer ordering gate fixture: missing migration environment contract.
System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "Global\AirPodsWindows_InstanceMutex") p.R0 ?e'
Pop $R2
nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM "AirPodsWindows.exe"'
Pop $R2
File /r runtime

Section "Uninstall"
ReadEnvStr $R0 "AirPodsWindowsMigration"
StrCmp $R0 "1" apd_keep_user_data
SetShellVarContext current
DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "AirPodsWindows"
RMDir /r "$APPDATA\AirPodsWindows"
RMDir /r "$LOCALAPPDATA\AirPodsWindows"
SetShellVarContext all
SectionEnd
