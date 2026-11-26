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
