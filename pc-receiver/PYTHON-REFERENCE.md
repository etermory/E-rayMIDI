# Windows에서 USB로 E-ray MIDI 소리 받기

같은 USB 케이블에서 **PC → 휴대폰은 기존 USB MIDI**, **휴대폰 → PC는 ADB 오디오
전송**을 사용합니다. Wi-Fi, 루팅, USB 테더링은 사용하지 않습니다. PC에서는 이
수신 프로그램이 소리를 재생합니다. Windows에 새 사운드카드나 녹음 장치를 등록하는
기능은 아닙니다.

## 준비

1. 새로 빌드한 E-ray MIDI APK를 휴대폰에 설치합니다.
2. Windows에 **Python 3.10 이상**을 설치합니다. Python Launcher(`py`)도 설치해 주세요.
3. Google의 [SDK Platform-Tools for Windows](https://developer.android.com/tools/releases/platform-tools)를
   내려받아 이 폴더 안에 압축을 풉니다. `pc-receiver/platform-tools/adb.exe`가 있으면 됩니다.
   이미 `adb`가 PATH에 있으면 별도로 받을 필요가 없습니다.
4. 이 폴더의 `setup-windows.bat`을 실행합니다. 최초 설정 시 Python 패키지를 받으므로
   인터넷 연결이 필요합니다. 재생 중에는 인터넷이 필요 없습니다.
5. 휴대폰 개발자 옵션에서 **USB 디버깅**을 켜고, 데이터 전송이 가능한 케이블로 PC에
   연결합니다. 휴대폰에 뜨는 이 PC의 USB 디버깅 허용 창을 승인합니다.
6. 휴대폰의 USB 연결 방식을 **MIDI**로 선택합니다. MIDI와 USB 디버깅이 동시에
   활성화되어 있어야 합니다. 제조사/기기에 따라 동시 제공 여부가 다를 수 있습니다.

## 재생

1. 앱에서 `USB MIDI기기`, 원하는 엔진을 선택하고 **연결 시작**을 누릅니다.
2. 앱 상단 **출력: 휴대폰** 버튼에서 **PC만 (USB)** 또는 **휴대폰 + PC (USB)**를 선택합니다.
3. Windows에서 `start-windows.bat`을 실행합니다.
4. PC의 게임/DAW/MIDI 플레이어에서 휴대폰을 MIDI 출력 장치로 선택합니다.
5. 앱에 **PC 연결**, PC에 `Connected: phone ... Hz -> PC ... Hz`가 표시되면
   Windows의 기본 출력 장치로 소리가 나옵니다.

**PC만**은 수신 프로그램이 연결되지 않아도 휴대폰을 무음으로 만듭니다. 연결이 끊기면
수신 프로그램이 자동으로 재시도합니다. 종료는 PC에서 `Ctrl+C`, 휴대폰에서 출력 모드를
**휴대폰**으로 바꾸시면 됩니다. 앱을 다시 실행하면 기본 출력은 휴대폰입니다.

휴대폰의 미디어 볼륨 대신 PC 볼륨으로 수신 음량을 조절해 주세요. 전송하는 오디오는
엔진이 생성한 PCM이며 Android의 최종 시스템 볼륨/오디오 효과 적용 전의 데이터입니다.

## 출력 장치 / 버퍼 선택

명령 프롬프트에서 이 폴더로 이동한 뒤 실행합니다.

```bat
.venv\Scripts\python.exe receiver.py --list-devices
.venv\Scripts\python.exe receiver.py --device 5
.venv\Scripts\python.exe receiver.py --buffer-ms 100
.venv\Scripts\python.exe receiver.py --serial YOUR_USB_SERIAL
.venv\Scripts\python.exe receiver.py --adb "C:\Android\platform-tools\adb.exe"
```

기본 수신 버퍼는 60ms입니다. 끊기면 100~150ms로 늘려 보세요. 실제 지연에는 엔진의
기존 렌더 버퍼, USB/ADB 전송, 리샘플러, Windows 출력 지연도 더해집니다. 전문 오디오
인터페이스 수준의 지연을 보장하지 않습니다. 장시간 재생 중 두 기기의 오디오 클럭
차이는 PC에서 리샘플링 비율을 조금씩 조정하여 따라갑니다. 심한 지연은 오래된 데이터를
버리고 복구하며, 버퍼 부족은 무음 후 다시 채워 재생합니다.

## 연결이 안 될 때

```bat
platform-tools\adb.exe devices
platform-tools\adb.exe -d shell getprop sys.usb.state
```

- `unauthorized`: 휴대폰 잠금을 풀고 USB 디버깅 허용 창을 확인합니다.
- 장치 없음: 케이블과 Windows의 제조사 ADB 드라이버를 확인합니다.
- USB 상태에 `midi`와 `adb`가 모두 있어야 합니다. MIDI 모드에서 ADB가 사라지는
  기기는 이 방식의 동시 사용을 지원하지 않을 수 있습니다.
- `Waiting for USB audio`: 휴대폰 앱에서 PC 출력을 선택하고 엔진을 시작했는지 확인합니다.
- 연결은 되는데 소리가 없음: ROM/SF2 로드 상태와 MIDI 출력 대상, PC 출력 장치/볼륨을 확인합니다.
- USB 휴대폰은 한 대만 연결합니다. `--serial`은 연결된 기기의 일련번호를 확인합니다. 무선 ADB는 사용하지 않습니다.
- Windows의 장치가 모노 출력만 지원하면 스테레오 출력 장치를 선택해 주세요.

## 구현 / 검증

- 다섯 엔진의 AAudio 콜백에서 스테레오 S16 PCM을 고정 크기 큐에 복사합니다.
  콜백에서는 메모리 할당, 소켓 I/O, 대기 잠금을 하지 않습니다.
- 공유 라이브러리 `pc-audio`의 전송 스레드는 **휴대폰 127.0.0.1:49155**만 리슨합니다.
  PC는 USB ADB의 `forward tcp:0 tcp:49155`로 사용 가능한 로컬 포트를 할당받습니다.
  Wi-Fi에 서버를 공개하거나 ADB 무선 모드를 켜지 않습니다.
- 전송 형식: 16바이트 `<4sIIHH` 헤더(`ERA1`, 샘플레이트, 프레임 수, 채널=2,
  비트=16) + 최대 512프레임의 little-endian 스테레오 PCM.
- SC-55의 66207Hz, MT-32의 32000Hz 등 **엔진 원본 레이트는 변경하지 않습니다**.
  PC의 SoXR 스트리밍 리샘플러가 PC 출력 장치의 레이트에 맞춥니다.
- 소켓 지연이나 전송 큐 초과 시 연결을 닫고 재연결하여 지연 누적을 제한합니다.
- 이 기능은 앱 자체가 합성한 소리만 전송합니다. 마이크나 다른 앱의 오디오를 녹음하지 않습니다.

호스트 단위/통합 테스트:

```sh
python -m unittest discover -s pc-receiver -p 'test_*.py' -v
clang++ -std=c++20 -pthread -I android-app/app/src/main/cpp \
  android-app/app/src/main/cpp/PcAudio.cpp pc-receiver/test_transport.cpp \
  -o /tmp/eray-pc-transport-test
/tmp/eray-pc-transport-test
```

소스 테스트와 APK 빌드는 실제 Windows+Android USB 왕복 재생 검증을 대신하지 않습니다.
배포 전에는 다섯 엔진의 청취, MIDI+ADB 동시 연결, 케이블 재연결, 엔진 전환,
휴대폰 화면 끄기/장시간 재생을 대상 기기에서 확인해 주세요.

전송 방식 참고: [ADB 명령 문서](https://android.googlesource.com/platform/packages/modules/adb/+/refs/heads/main/docs/user/adb.1.md),
[AOSP MIDI+ADB USB 구성](https://android.googlesource.com/platform/system/core/+/refs/heads/main/rootdir/init.usb.configfs.rc).
