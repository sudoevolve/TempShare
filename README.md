# TempShare

> Everything, temporarily connected.

TempShare 让同一局域网内的电脑、手机和平板临时连接，分享文件、同步内容和投递屏幕。电脑运行 Aino C++ 桌面客户端，手机扫描二维码或在浏览器中输入连接码即可加入。设备发现、配对和传输均在局域网内完成。

![TempShare 产品设计预览](docs/preview/image.png)

## 功能

| 功能 | 使用方式 |
| --- | --- |
| Share 文件分享 | 桌面可选择多个文件、将文件夹打包为 ZIP。Windows 支持把文件拖进窗口。网页上传需由桌面逐个确认，支持暂停、继续、取消和失败重试，并显示进度、速度和剩余时间。下载由浏览器管理。 |
| Sync 内容同步 | 配对后双向发送文本、链接、代码和图片。桌面可主动发送剪贴板文本，双方都可以复制收到的文本。文本历史保留在当前会话中。 |
| Cast 屏幕投递 | 从桌面端打开投递控制台，再在浏览器的系统选择器中选择屏幕或窗口。已配对的网页可以通过 WebRTC 观看。支持调节画质和帧率，音频取决于浏览器和共享来源。 |
| 临时连接 | 附近设备发现、二维码、六位连接码和桌面验证码核对。一个会话连接一台浏览器，有效期为 30 分钟，双方都可以结束连接。 |
| 设备信任 | HTTPS 下可保存、更新和撤销设备信任。已记住的浏览器可按保存的权限快速连接，上传仍需确认。 |

单个文件和文件夹归档的上限均为 1 GiB。上传断点在同一会话且网页仍持有文件时可用。接收文件保存到会话子目录，重名时自动添加序号。

## 构建

需要 CMake 3.20+、Git、C 和 C++17 编译器。首次配置拉取固定版本的 Aino、cpp-httplib、QR Code Generator 和 miniz。HTML、CSS 和 JavaScript 随可执行文件嵌入，运行时无需联网下载依赖。

### Windows

使用 Visual Studio 2022 或更新版本并安装 C++ 桌面开发组件。

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\tempshare.exe
```

### Linux / macOS

Linux 需要 X11 开发包，可安装 Zenity 使用文件选择对话框。macOS 使用 Xcode Command Line Tools。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/tempshare
```

当前已在 Windows 和 Linux 构建验证。macOS 是目标平台，尚待实机验证。

## 使用

1. 让两台设备接入同一局域网，打开桌面客户端。
2. 手机扫描二维码，或打开桌面显示的局域网地址，再输入六位连接码。
3. 在桌面端核对浏览器显示的验证码并点击「允许」。
4. 双方可在「文件分享」和「内容同步」中发送内容。需要投递屏幕时，在电脑端选择「屏幕投递」并打开控制台，再在手机端点击「开始观看」。

桌面端默认使用 TCP 8787，端口被占用时选择可用端口。切换 Wi-Fi 或网线时，连接地址和二维码会自动更新。电脑有多张网卡时，可在桌面端切换到手机所在子网的地址。附近设备发现使用 UDP 47871 和 IPv4 组播。组播不可用时可使用二维码或手动地址。

首次运行的接收目录是启动目录下的 `received/`。在设置页选择新目录后，下次启动会沿用该目录。`--receive-dir <目录>` 可为本次运行指定接收位置。

## 局域网与 HTTPS

服务识别本机的私有 IPv4 地址，只接受对应同一子网内的访问。WebRTC 媒体流使用加密直连，信令由本机服务处理。默认的 HTTP 模式下，文件、文本和信令在局域网内以明文传输。需要加密这些请求或使用设备信任时，用 OpenSSL 3 和 `-DTEMPSHARE_TLS=ON` 构建，再指定证书和私钥。

```sh
./build/tempshare --cert cert.pem --key key.pem
```

证书需由参与设备信任，证书的 SAN 应包含桌面端展示的 IPv4 地址。使用 HTTPS 投递控制台时，SAN 还应包含 `127.0.0.1`，本机浏览器也需要信任证书。控制台在本机浏览器的屏幕捕获权限下运行。观看端使用支持 WebRTC 的浏览器。

## 项目结构

| 路径 | 职责 |
| --- | --- |
| `src/client/` | C++ / aino 桌面 UI、系统文件选择与平台交互 |
| `src/core/` | 局域网发现、会话授权、设备信任、文件归档和 HTTP(S) 服务 |
| `src/web/` | 浏览器配对、文件传输、文本同步和 WebRTC 投递 |
| `tests/` | 授权、文件传输、文本同步、投递信令和网络变化集成测试 |

[产品定位和路线图](docs/产品定位.md)

## 许可证

TempShare 使用 GNU Lesser General Public License v3.0（`LGPL-3.0-only`）。依赖沿用各自的上游许可证，Aino 为 Apache-2.0，cpp-httplib、QR Code Generator 和 miniz 为 MIT。[LICENSE](LICENSE)
