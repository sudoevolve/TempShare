# TempShare

> Everything, temporarily connected.

TempShare 让同一局域网内的电脑、手机和平板临时连接并互传文件。电脑运行原生桌面客户端，手机扫描二维码或在浏览器输入连接码即可加入。设备发现、配对和传输都在局域网内完成。

![TempShare 产品设计预览](docs/preview/image.png)

## V0.1 Share

- 在桌面端查看附近运行 TempShare 的设备，或使用二维码和六位连接码加入会话。
- 桌面端核对浏览器验证码并确认配对。每个会话允许一台浏览器参与，默认有效期为 30 分钟。
- 桌面端选择多个文件分享，浏览器逐个下载。Windows 可将文件拖入桌面窗口。
- 浏览器可批量提交文件。桌面端逐个确认接收，浏览器显示上传进度，支持取消和失败重试。单个文件上限为 1 GiB。
- 桌面端可设置接收目录。文件写入对应会话的子目录，同名文件会添加序号。结束会话会撤销访问权限，已接收文件保存在本地。

## 构建

需要 CMake 3.20+、Git 和 C++17 编译器。首次配置从 GitHub 拉取固定版本的 [aino](https://github.com/sudoevolve/aino)、[cpp-httplib](https://github.com/yhirose/cpp-httplib) 和 [QR Code Generator](https://github.com/nayuki/QR-Code-generator)。HTML、CSS 和 JavaScript 随可执行文件嵌入，运行时无需联网下载依赖。

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
4. 电脑点击「选择文件」后，手机可下载。手机选择文件后，电脑点击「接收」开始传输。

桌面端默认使用 TCP 8787，端口被占用时选择可用端口。切换 Wi-Fi 或网线时，连接地址和二维码会自动更新。电脑有多张网卡时，可在桌面端切换到手机所在子网的地址。附近设备发现使用 UDP 47871 和 IPv4 组播。组播不可用时可使用二维码或手动地址。

默认接收目录是启动目录下的 `received/`，也可以通过 `--receive-dir <目录>` 设置启动时的接收位置。

## 局域网与 HTTPS

服务识别本机的私有 IPv4 地址，只接受对应同一子网内的访问。默认传输使用 HTTP，内容在局域网上以明文传输。需要加密时，用 OpenSSL 3 和 `-DTEMPSHARE_TLS=ON` 构建，然后启动时指定证书和私钥。

```sh
./build/tempshare --cert cert.pem --key key.pem
```

证书需由参与设备信任，并涵盖桌面端展示的 IPv4 地址。证书和密钥均可由局域网内自行管理。

## 项目结构

| 路径 | 职责 |
| --- | --- |
| `src/client/` | C++ / aino 桌面 UI、系统文件选择与平台交互 |
| `src/core/` | 局域网发现、会话授权、文件流和 HTTP(S) 服务 |
| `src/web/` | 浏览器配对、文件选择、上传进度和下载 |
| `tests/` | 会话、权限和双向文件传输集成测试 |

后续版本按产品路线图扩展 Sync 文本同步、Cast 屏幕投递和局域网连接恢复。[产品定位和路线图](docs/产品定位.md)

## 许可证

TempShare 使用 GNU Lesser General Public License v3.0（`LGPL-3.0-only`）。依赖沿用各自的上游许可证，aino 为 Apache-2.0，cpp-httplib 和 QR Code Generator 为 MIT。[LICENSE](LICENSE)
