cd jni
call ndk-build
if %ERRORLEVEL% EQU 0 (
	echo ndk-build has failed, build cancelled
	cd..

	mkdir "assets"
	xcopy "..\data\*.*" "assets" /SY

	mkdir "res\drawable"
	xcopy "icon.png" "res\drawable" /Y
    mkdir "res\drawable-xhdpi";
	xcopy "banner.png" "res\drawable-xhdpi" /Y

	call ant debug -Dout.final.file=vulkansponza.apk
    if "%2" == "-deploy" (    
        adb install -r vulkansponza.apk
    )
) ELSE (
	echo error : ndk-build failed with errors!
	cd..
)
