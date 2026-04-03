[Setup]
AppName=OBS Credits Plugin
AppVersion=1.0.0
AppPublisher=Kiernen Irons
DefaultDirName={commonpf}\obs-studio
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=obs-credits-1.0.0-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName=OBS Credits Plugin
WizardStyle=modern
PrivilegesRequired=admin

[Files]
Source: "..\build\Release\obs-credits.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "..\data\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\obs-credits\locale"; Flags: ignoreversion
Source: "..\examples\credits.json"; DestDir: "{app}\data\obs-plugins\obs-credits\examples"; Flags: ignoreversion

[Messages]
SetupWindowTitle=OBS Credits Plugin Installer
WelcomeLabel1=OBS Credits Plugin
WelcomeLabel2=This will install the OBS Credits Plugin v1.0.0 into your OBS Studio installation.%n%nThe plugin adds scrolling credits to OBS with Discord and YouTube chat integration.%n%nMake sure OBS Studio is closed before continuing.
FinishedLabel=The OBS Credits Plugin has been installed. Launch OBS Studio and add a "Credits" source to use it.
