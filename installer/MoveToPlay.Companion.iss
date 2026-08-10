#ifndef PublishDir
  #error "缺少 PublishDir，请通过 build-team-installer.ps1 构建。"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif

[Setup]
AppId={{7EE0D59E-1ED2-48E2-BE6A-D06163261253}
AppName=MoveToPlay Companion
AppVersion={#AppVersion}
AppPublisher=MoveToPlay Team
DefaultDirName={localappdata}\Programs\MoveToPlay
DefaultGroupName=MoveToPlay
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=MoveToPlay.Companion.Team.Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
UninstallDisplayIcon={app}\MoveToPlay.Companion.exe
VersionInfoVersion={#AppVersion}
VersionInfoCompany=MoveToPlay Team
VersionInfoDescription=MoveToPlay Companion 队友安装包
VersionInfoProductName=MoveToPlay Companion

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "快捷方式："; Flags: checkedonce

[Files]
Source: "{#PublishDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\MoveToPlay Companion"; Filename: "{app}\MoveToPlay.Companion.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\MoveToPlay Companion"; Filename: "{app}\MoveToPlay.Companion.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\MoveToPlay.Companion.exe"; Description: "启动 MoveToPlay Companion"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: files; Name: "{localappdata}\MoveToPlay\credentials\team-cloud.dat"
Type: dirifempty; Name: "{localappdata}\MoveToPlay\credentials"
