Set shell = CreateObject("Shell.Application")
Set fso = CreateObject("Scripting.FileSystemObject")
scriptDir = fso.GetParentFolderName(WScript.ScriptFullName)
ps1Path = scriptDir & "\tools\Universal-Control-Center.ps1"
args = "-STA -NoProfile -ExecutionPolicy Bypass -File """ & ps1Path & """"
shell.ShellExecute "powershell.exe", args, "", "runas", 1
