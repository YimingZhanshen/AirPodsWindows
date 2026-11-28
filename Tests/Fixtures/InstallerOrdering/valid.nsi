; Installer ordering gate fixture: executable instructions.
DetailPrint 'Literal comment markers stay text: /* x */ ; y'
/*
System::Call 'kernel32::OpenMutexW(i 0, i 0, w "Global\CommentedMutex") p.R0 ?e'
*/
System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "Global\AirPodsWindows_InstanceMutex") p.R0 ?e' ; executable
Pop $R2
nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM "AirPodsWindows.exe"' /* executable */
Pop $R2
File /r runtime ; executable

Section "Uninstall"
ReadEnvStr $R0 "AirPodsWindowsMigration"
StrCmp $R0 "1" apd_keep_user_data
SetShellVarContext current
DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "AirPodsWindows"
RMDir /r "$APPDATA\AirPodsWindows"
RMDir /r "$LOCALAPPDATA\AirPodsWindows"
SetShellVarContext all
System::Call 'kernel32::SetEnvironmentVariable(t, t) i("AirPodsWindowsMigration", "1").r0'
StrCmp $0 0 apd_migration_guard_failed
ExecWait '"$R3" /S _?=$R6' $R4
System::Call 'kernel32::SetEnvironmentVariable(t, t) i("AirPodsWindowsMigration", "").r0'
Goto apd_migration_guard_done
apd_migration_guard_failed:
Abort
apd_migration_guard_done:
SectionEnd
