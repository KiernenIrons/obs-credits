[Setup]
AppName=OBS Credits Plugin
AppVersion=0.1.0
AppPublisher=Kiernen Irons
DefaultDirName={userappdata}\obs-studio\plugins\obs-credits
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=obs-credits-0.1.0-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayName=OBS Credits Plugin
SetupIconFile=
WizardStyle=modern
PrivilegesRequired=lowest

[Files]
Source: "..\build\Release\obs-credits.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "..\data\locale\en-US.ini"; DestDir: "{app}\data\locale"; Flags: ignoreversion
Source: "..\examples\credits.json"; DestDir: "{app}\data\examples"; Flags: ignoreversion

[Messages]
SetupWindowTitle=OBS Credits Plugin Installer
WelcomeLabel1=OBS Credits Plugin
WelcomeLabel2=This will install the OBS Credits Plugin v0.1.0 into your OBS Studio plugins directory.%n%nThe plugin adds a scrolling credits roll source to OBS.%n%nMake sure OBS Studio is closed before continuing.
FinishedLabel=The OBS Credits Plugin has been installed. Launch OBS Studio and add a "Credits Roll" source to use it.
