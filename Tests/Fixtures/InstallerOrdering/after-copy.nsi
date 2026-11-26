; Keep original line numbers through comments.
/*
System::Call 'kernel32::OpenMutexW(i 0, i 0, w "Global\CommentedMutex") p.R0 ?e'
*/
File /r runtime
; padding after the first executable copy
System::Call 'kernel32::OpenMutexW(i 0x00100000, i 0, w "Global\AirPodsWindows_InstanceMutex") p.R0 ?e'
Pop $R2
nsExec::Exec '"$SYSDIR\taskkill.exe" /F /IM "AirPodsWindows.exe"'
Pop $R2
