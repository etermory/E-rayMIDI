# E-ray MIDI Windows 수신 프로그램

**`E-rayMIDI-Receiver.exe` 하나로 실행하는 Windows 10/11 x64용 프로그램입니다.**
Python, pip, 별도 오디오 라이브러리, ADB 설치가 필요 없습니다. C++로 작성했으며
Windows 기본 오디오 기능(WASAPI)으로 재생합니다. 콘솔 창에 연결 상태가 표시됩니다.

## 실행 방법

1. PC 오디오 출력 기능이 들어간 E-ray MIDI APK를 휴대폰에 설치합니다.
2. 데이터 전송용 USB 케이블로 PC와 연결합니다.
3. 휴대폰에서 **USB 디버깅**을 켜고 PC 연결을 허용합니다. USB 연결 방식은 **MIDI**로 설정합니다.
4. 앱에서 `USB MIDI기기`와 엔진을 선택하고 **연결 시작**을 누릅니다.
5. 앱 상단 **출력**에서 **PC만 (USB)** 또는 **휴대폰 + PC (USB)**를 선택합니다.
6. PC에서 **`E-rayMIDI-Receiver.exe`를 더블클릭**합니다.
7. PC의 게임/DAW/MIDI 플레이어에서 휴대폰을 MIDI 출력 장치로 선택합니다.

`USB audio connected`와 `Playing through Windows audio`가 표시되면 Windows 기본
출력 장치로 소리가 나옵니다. 음량은 Windows 볼륨 믹서로 조절해 주세요.
종료는 수신 창에서 **Ctrl+C**입니다. 연결이 끊기면 자동으로 재시도합니다.

처음 실행할 때 EXE에 포함된 Google ADB와 관련 파일을
`%LOCALAPPDATA%\E-rayMIDI\adb-<내용 해시>\`에 풉니다. 인터넷 다운로드나 관리자 권한은
필요하지 않습니다. USB 드라이버가 없는 PC에서는 휴대폰 제조사 ADB 드라이버가 별도로
필요할 수 있습니다. ADB의 오픈소스 고지 `NOTICE.txt`도 같은 폴더에 함께 저장됩니다.
ADB 서버는 다른 개발 도구와 공유할 수 있으므로 종료 시 강제로 종료하지 않습니다.
수신 프로그램이 만든 포트 전달 규칙만 해제합니다.

## 설정

기본값은 Windows 기본 출력 장치, 스테레오, 수신 버퍼 60ms입니다.
명령 프롬프트에서 다음과 같이 실행할 수 있습니다.

```bat
E-rayMIDI-Receiver.exe --list-devices
E-rayMIDI-Receiver.exe --device 0
E-rayMIDI-Receiver.exe --buffer-ms 100
E-rayMIDI-Receiver.exe --serial YOUR_USB_SERIAL
E-rayMIDI-Receiver.exe --extract-adb
E-rayMIDI-Receiver.exe --help
```

`--list-devices`에서 확인한 출력 장치 번호를 `--device`에 넣으시면 됩니다.
출력 장치를 바꾼 경우 프로그램을 다시 실행해 주세요. USB 휴대폰은 한 대만 연결해 주세요.
`--serial`은 연결된 휴대폰의 일련번호를 확인하는 옵션입니다. 무선 ADB는 사용하지 않습니다.

## 동작과 제한

- 같은 케이블에서 **PC → 휴대폰은 USB MIDI**, **휴대폰 → PC는 ADB 오디오 전송**입니다.
  표준 USB 사운드카드로 등록되거나 Windows 녹음 장치가 새로 생기지는 않습니다.
- 휴대폰이 MIDI와 USB 디버깅을 동시에 제공해야 합니다.
- 다섯 엔진의 원본 PCM을 받으며, SC-55의 66207Hz 등 원본 레이트는 휴대폰에서
  변경하지 않습니다. PC의 WASAPI 고품질 샘플레이트 변환을 사용합니다.
- 휴대폰과 PC의 클럭 차이는 재생 속도를 최대 ±0.3% 범위에서 서서히 보정합니다.
  수신 버퍼가 넘치면 오래된 데이터를 버리고, 부족하면 무음으로 채운 뒤 재개합니다.
- 기본 수신 버퍼 외에 엔진·USB·Windows 출력 지연도 더해집니다. 끊기면
  `--buffer-ms 100` 또는 `150`을 사용해 보세요.
- **PC만** 모드에서는 수신 프로그램 연결 전에도 휴대폰이 무음입니다.
- 앱의 합성 오디오만 전달합니다. 마이크/다른 앱 소리와 Android 최종 시스템 볼륨은
  전달하지 않습니다.
- EXE는 코드 서명되지 않았습니다. Windows 실기기에서의 실행과 USB 왕복 청취는
  별도 검증이 필요합니다. 크로스 빌드 및 코어 테스트 통과와 실기기 검증은 구분합니다.

## 문제 해결

- `unauthorized`: 휴대폰 잠금을 풀고 USB 디버깅 허용 창을 확인합니다.
- `no devices`: 데이터 케이블과 Windows 제조사 ADB 드라이버를 확인합니다.
- `Set phone USB mode to MIDI`: 휴대폰 USB 연결 방식을 MIDI로 바꿉니다.
- `Phone audio disconnected or idle`: 앱의 엔진과 PC 출력 모드를 켰는지 확인합니다.
- `Output: ... failed`: Windows에서 사용 가능한 출력 장치를 확인하고
  `--list-devices`, `--device`로 지정해 보세요.
- 소리 없음: PC에서 선택한 MIDI 출력 대상, 앱 ROM/SF2 로딩 상태, Windows 볼륨을 확인합니다.

## 소스 빌드 및 테스트

소스는 `native/receiver.cpp`, 공통 패킷·버퍼 코드는 `native/audio_core.h`입니다.
Zig 0.15.2의 C++ 크로스 컴파일러와 공식 Windows Platform-Tools를 사용합니다.
사용자의 실행 환경에는 빌드 도구나 Python이 필요하지 않습니다.

```sh
ZIG=/path/to/zig RC=/path/to/llvm-rc \
  bash pc-receiver/native/build.sh /path/to/windows/platform-tools

clang++ -std=c++20 -Wall -Wextra -Werror -fsanitize=address,undefined \
  pc-receiver/native/test_audio_core.cpp -o /tmp/eray-native-core-test
/tmp/eray-native-core-test
```

`RC`를 생략하면 `zig rc`를 사용합니다. 빌드 결과는 `build/E-rayMIDI-Receiver.exe`입니다.
C++ 런타임은 정적 링크하며 Python/PortAudio/SoXR DLL은 사용하지 않습니다.
ADB 리소스는 빌드 때 포함되므로 원본 Platform-Tools와 고지 파일이 필요합니다.

기존 Python 수신기는 개발용 비교 구현으로 남아 있습니다.
사용법은 [Python 비교 구현 안내](PYTHON-REFERENCE.md)를 참고하세요.

참고: [Google Platform-Tools](https://developer.android.com/tools/releases/platform-tools),
[WASAPI 오디오 변환·클럭 조정](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audclnt-streamflags-xxx-constants).

USB 연결 수정판: Windows에서 `get-devpath`가 `unknown`인 정상 USB 기기도 인식합니다.
모든 ADB 명령에 `-d`를 사용하여 USB 연결만 선택합니다.
