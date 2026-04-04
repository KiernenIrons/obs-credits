[Setup]
AppName=Credits Plugin
AppVersion=1.0.0
AppPublisher=Kiernen Irons
DefaultDirName={commonpf}\obs-studio
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=credits-plugin-1.0.0-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName=Credits Plugin
WizardStyle=modern
PrivilegesRequired=admin

[Files]
Source: "..\build\Release\obs-credits.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\data\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\obs-credits\locale"; Flags: ignoreversion
Source: "..\examples\credits.json"; DestDir: "{app}\data\obs-plugins\obs-credits\examples"; Flags: ignoreversion

[Messages]
SetupWindowTitle=Credits Plugin Installer
WelcomeLabel1=Credits Plugin
WelcomeLabel2=This will install the Credits Plugin v1.0.0 for OBS Studio.%n%nThe plugin adds scrolling credits with Discord and YouTube chat integration.%n%nMake sure OBS Studio is closed before continuing.
FinishedLabel=The Credits Plugin has been installed. Launch OBS Studio and add a "Credits" source to use it.
