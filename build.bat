@echo off
setlocal

if exist .\_project\vs2022\streamline.sln (
    rem find VS 2022
    for /f "usebackq tokens=1* delims=: " %%i in (`.\tools\vswhere.exe -version [17^,18^) -requires Microsoft.VisualStudio.Workload.NativeDesktop`) do (
        if /i "%%i"=="installationPath" (
            set VS_PATH=%%j
            GOTO :found_vs
        )
    )
    rem Try looking for VS Build Tools 2022
    for /f "usebackq tokens=1* delims=: " %%i in (`.\tools\vswhere.exe -products * -version [17^,18^) -requires Microsoft.VisualStudio.Workload.VCTools -requires Microsoft.VisualStudio.Workload.MSBuildTools`) do (
        if /i "%%i"=="installationPath" (
            set VS_PATH=%%j
            GOTO :found_vs
        )
    )
) else if exist .\_project\vs2019\streamline.sln (
    rem find VS 2019
    for /f "usebackq tokens=1* delims=: " %%i in (`.\tools\vswhere.exe -version [16^,17^) -requires Microsoft.VisualStudio.Workload.NativeDesktop `) do (
        if /i "%%i"=="installationPath" (
            set VS_PATH=%%j
            GOTO :found_vs
        )
    )
    rem Try looking for VS Build Tools 2019
    for /f "usebackq tokens=1* delims=: " %%i in (`.\tools\vswhere.exe -products * -version [16^,17^) -requires Microsoft.VisualStudio.Workload.VCTools -requires Microsoft.VisualStudio.Workload.MSBuildTools`) do (
        if /i "%%i"=="installationPath" (
            set VS_PATH=%%j
            GOTO :found_vs
        )
    )
) else (
    rem find VS 2017
    for /f "usebackq tokens=1* delims=: " %%i in (`.\tools\vswhere.exe -version [15^,16^) -requires Microsoft.VisualStudio.Workload.NativeDesktop`) do (
        if /i "%%i"=="installationPath" (
            set VS_PATH=%%j
            GOTO :found_vs
        )
    )
    rem Try looking for VS Build Tools 2017
    for /f "usebackq tokens=1* delims=: " %%i in (`.\tools\vswhere.exe -products * -version [15^,16^) -requires Microsoft.VisualStudio.Workload.VCTools -requires Microsoft.VisualStudio.Workload.MSBuildTools`) do (
        if /i "%%i"=="installationPath" (
            set VS_PATH=%%j
            GOTO :found_vs
        )
    )
)
:found_vs

echo off
set cfg=Debug
set bld=Clean,Build

:loop
IF NOT "%1"=="" (
    IF "%1"=="-debug" (
        SET cfg=Debug
        SHIFT
    )
    IF "%1"=="-develop" (
        SET cfg=Develop
        SHIFT
    )
    IF "%1"=="-production" (
        SET cfg=Production
        SHIFT
    )
    :: Deprecated build configuration names
    IF "%1"=="-release" (
        SET cfg=Debug
        SHIFT
    )
    IF "%1"=="-profiling" (
        SET cfg=Develop
        SHIFT
    )
    IF "%1"=="-relextdev" (
        SET cfg=Develop
        SHIFT
    )
    SHIFT
    GOTO :loop
)

if not exist "%VS_PATH%" (
    echo "%VS_PATH%" not found. Is Visual Studio installed? && goto :ErrorExit
)

for /f "delims=" %%F in ('dir /b /s "%VS_PATH%\vsdevcmd.bat" 2^>nul') do set VSDEVCMD_PATH=%%F
echo ********Executing %VSDEVCMD_PATH%********
call "%VSDEVCMD_PATH%"
goto :SetVSEnvFinished

:ErrorExit
exit /b 1

:SetVSEnvFinished

if exist .\_project\vs2022\streamline.sln (
    msbuild .\_project\vs2022\streamline.sln /m /t:%bld% /property:Configuration=%cfg% /property:Platform=x64
) else if exist .\_project\vs2019\streamline.sln (
    msbuild .\_project\vs2019\streamline.sln /m /t:%bld% /property:Configuration=%cfg% /property:Platform=x64
) else (
    echo WARNING vs2017 is deprecated and will be removed in SL 2.9
    msbuild .\_project\vs2017\streamline.sln /m /t:%bld% /property:Configuration=%cfg% /property:Platform=x64
)
