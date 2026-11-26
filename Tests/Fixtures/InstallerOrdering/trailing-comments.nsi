System::Call 'kernel32::CloseHandle(p R0)' ; kernel32::OpenMutexW(i 0x00100000, i 0, w "Global\AirPodsWindows_InstanceMutex")
nsExec::Exec 'echo setup' ; taskkill.exe /F /IM AirPodsWindows.exe
System::Call 'kernel32::CloseHandle(p R0)' /* kernel32::OpenMutexW(i 0x00100000, i 0, w "Global\AirPodsWindows_InstanceMutex") */
nsExec::Exec 'echo setup' /* taskkill.exe /F /IM AirPodsWindows.exe */
File /r runtime
